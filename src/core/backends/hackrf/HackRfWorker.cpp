#include "HackRfWorker.h"

#include <hackrf.h>

#include <QDebug>
#include <QLoggingCategory>
#include <QMetaObject>

Q_LOGGING_CATEGORY(lcHackRf, "aether.hackrf", QtWarningMsg)

namespace AetherSDR::hackrf {

namespace {

QString errName(int rc)
{
    return QString::fromLatin1(hackrf_error_name(static_cast<enum hackrf_error>(rc)));
}

} // namespace

HackRfWorker::HackRfWorker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<std::complex<float>>>("QVector<std::complex<float>>");
}

HackRfWorker::~HackRfWorker()
{
    close();
}

bool HackRfWorker::open(const QString& serial)
{
    if (m_device) {
        qCWarning(lcHackRf) << "HackRfWorker: open() called while already open";
        return false;
    }

    // Process-wide init — see the header comment on the single-instance
    // assumption this relies on.
    const int initRc = hackrf_init();
    if (initRc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_init failed:" << errName(initRc);
        return false;
    }

    hackrf_device* dev = nullptr;
    const int openRc = serial.isEmpty()
        ? hackrf_open(&dev)
        : hackrf_open_by_serial(serial.toLatin1().constData(), &dev);
    if (openRc != HACKRF_SUCCESS || !dev) {
        qCWarning(lcHackRf) << "HackRfWorker: open failed:" << errName(openRc);
        hackrf_exit();
        return false;
    }

    m_device = dev;
    qCDebug(lcHackRf) << "HackRfWorker: opened device"
                         << (serial.isEmpty() ? QStringLiteral("(first found)") : serial);
    return true;
}

bool HackRfWorker::openByIndex(int index)
{
    if (m_device) {
        qCWarning(lcHackRf) << "HackRfWorker: openByIndex() called while already open";
        return false;
    }
    if (index < 0) {
        qCWarning(lcHackRf) << "HackRfWorker: openByIndex() requires a non-negative index";
        return false;
    }

    const int initRc = hackrf_init();
    if (initRc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_init failed:" << errName(initRc);
        return false;
    }

    hackrf_device_list_t* list = hackrf_device_list();
    if (!list) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_device_list failed";
        hackrf_exit();
        return false;
    }
    if (index >= list->devicecount) {
        qCWarning(lcHackRf) << "HackRfWorker: index" << index << "out of range ("
                            << list->devicecount << "device(s) enumerated)";
        hackrf_device_list_free(list);
        hackrf_exit();
        return false;
    }

    hackrf_device* dev = nullptr;
    const int openRc = hackrf_device_list_open(list, index, &dev);
    hackrf_device_list_free(list);  // the opened handle stays valid after this — see hackrf.h
    if (openRc != HACKRF_SUCCESS || !dev) {
        qCWarning(lcHackRf) << "HackRfWorker: openByIndex failed:" << errName(openRc);
        hackrf_exit();
        return false;
    }

    m_device = dev;
    qCDebug(lcHackRf) << "HackRfWorker: opened device at index" << index;
    return true;
}

void HackRfWorker::close()
{
    if (!m_device) return;

    // Both directions are mutually exclusive on the wire, but nothing stops
    // a caller from having left one running — tear down whichever is live
    // rather than assume the arbitration state machine already did.
    if (m_rxStreaming.load(std::memory_order_relaxed)) stopRx();
    if (m_txStreaming.load(std::memory_order_relaxed)) stopTx();

    const int rc = hackrf_close(m_device);
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_close failed:" << errName(rc);
    }
    m_device = nullptr;
    hackrf_exit();
}

bool HackRfWorker::setFreqHz(std::uint64_t hz)
{
    if (!m_device) return false;
    const int rc = hackrf_set_freq(m_device, hz);
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_set_freq failed:" << errName(rc)
                               << "requested" << hz << "Hz";
        return false;
    }
    return true;
}

bool HackRfWorker::setSampleRateHz(double hz)
{
    if (!m_device) return false;
    const int rc = hackrf_set_sample_rate(m_device, hz);
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_set_sample_rate failed:" << errName(rc)
                               << "requested" << hz << "Hz";
        return false;
    }
    return true;
}

bool HackRfWorker::setLnaGainDb(int requestedDb)
{
    if (!m_device) return false;
    const int db = clampLnaGainDb(requestedDb);
    const int rc = hackrf_set_lna_gain(m_device, static_cast<std::uint32_t>(db));
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_set_lna_gain failed:" << errName(rc);
        return false;
    }
    return true;
}

bool HackRfWorker::setVgaGainDb(int requestedDb)
{
    if (!m_device) return false;
    const int db = clampVgaGainDb(requestedDb);
    const int rc = hackrf_set_vga_gain(m_device, static_cast<std::uint32_t>(db));
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_set_vga_gain failed:" << errName(rc);
        return false;
    }
    return true;
}

bool HackRfWorker::setTxVgaGainDb(int requestedDb)
{
    if (!m_device) return false;
    const int db = clampTxVgaGainDb(requestedDb);
    const int rc = hackrf_set_txvga_gain(m_device, static_cast<std::uint32_t>(db));
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_set_txvga_gain failed:" << errName(rc);
        return false;
    }
    return true;
}

bool HackRfWorker::setAmpEnable(bool on)
{
    if (!m_device) return false;
    const int rc = hackrf_set_amp_enable(m_device, on ? 1 : 0);
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_set_amp_enable failed:" << errName(rc);
        return false;
    }
    return true;
}

// ── RX ───────────────────────────────────────────────────────────────────

bool HackRfWorker::startRx()
{
    if (!m_device) return false;
    if (m_txStreaming.load(std::memory_order_relaxed)) {
        qCWarning(lcHackRf) << "HackRfWorker: startRx() refused — TX is streaming"
                                  " (caller must stop TX first; see the RX/TX"
                                  " arbitration state machine in HackRfBackend)";
        return false;
    }
    const int rc = hackrf_start_rx(m_device, &HackRfWorker::rxCallback, this);
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_start_rx failed:" << errName(rc);
        return false;
    }
    m_rxStreaming.store(true, std::memory_order_relaxed);
    return true;
}

bool HackRfWorker::stopRx()
{
    if (!m_device) return false;
    const int rc = hackrf_stop_rx(m_device);
    m_rxStreaming.store(false, std::memory_order_relaxed);
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_stop_rx failed:" << errName(rc);
        return false;
    }
    return true;
}

int HackRfWorker::rxCallback(hackrf_transfer* transfer)
{
    auto* worker = static_cast<HackRfWorker*>(transfer->rx_ctx);
    if (!worker) return 1;  // no context: stop being called
    return worker->handleRxTransfer(transfer);
}

int HackRfWorker::handleRxTransfer(hackrf_transfer* transfer)
{
    // Runs on libhackrf/libusb's own internal thread — no libhackrf calls
    // from here (per hackrf.h's own callback contract), only the pure
    // conversion helper and a queued Qt signal emission (safe from any
    // thread; Qt resolves direct-vs-queued delivery from the RECEIVER's
    // thread affinity, not the emitting thread's).
    std::vector<std::complex<float>> converted;
    unpackRxIq(transfer->buffer, static_cast<std::size_t>(transfer->valid_length), converted);

    QVector<std::complex<float>> qv;
    qv.reserve(static_cast<int>(converted.size()));
    for (const auto& s : converted) qv.append(s);

    emit rxIqReady(qv);
    return 0;  // keep streaming
}

// ── TX ───────────────────────────────────────────────────────────────────

bool HackRfWorker::startTx()
{
    if (!m_device) return false;
    if (m_rxStreaming.load(std::memory_order_relaxed)) {
        qCWarning(lcHackRf) << "HackRfWorker: startTx() refused — RX is streaming"
                                  " (caller must stop RX first; see the RX/TX"
                                  " arbitration state machine in HackRfBackend)";
        return false;
    }
    const int rc = hackrf_start_tx(m_device, &HackRfWorker::txCallback, this);
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_start_tx failed:" << errName(rc);
        return false;
    }
    m_txStreaming.store(true, std::memory_order_relaxed);
    return true;
}

bool HackRfWorker::stopTx()
{
    if (!m_device) return false;
    const int rc = hackrf_stop_tx(m_device);
    m_txStreaming.store(false, std::memory_order_relaxed);
    {
        QMutexLocker locker(&m_txQueueMutex);
        m_txQueue.clear();  // stale audio from a previous transmission must not leak into the next
    }
    if (rc != HACKRF_SUCCESS) {
        qCWarning(lcHackRf) << "HackRfWorker: hackrf_stop_tx failed:" << errName(rc);
        return false;
    }
    return true;
}

void HackRfWorker::submitTxIq(const QVector<std::complex<float>>& iq)
{
    QMutexLocker locker(&m_txQueueMutex);
    for (const auto& s : iq) m_txQueue.push_back(s);
}

int HackRfWorker::txCallback(hackrf_transfer* transfer)
{
    auto* worker = static_cast<HackRfWorker*>(transfer->tx_ctx);
    if (!worker) return 1;
    return worker->handleTxTransfer(transfer);
}

int HackRfWorker::handleTxTransfer(hackrf_transfer* transfer)
{
    const std::size_t wantedSamples = static_cast<std::size_t>(transfer->buffer_length) / 2;
    std::vector<std::complex<float>> block;
    block.reserve(wantedSamples);

    bool underrun = false;
    {
        QMutexLocker locker(&m_txQueueMutex);
        for (std::size_t i = 0; i < wantedSamples; ++i) {
            if (m_txQueue.empty()) {
                underrun = true;
                block.emplace_back(0.0f, 0.0f);  // silence-pad rather than transmit garbage
                continue;
            }
            block.push_back(m_txQueue.front());
            m_txQueue.pop_front();
        }
    }

    packTxIq(block, transfer->buffer);
    transfer->valid_length = static_cast<int>(block.size() * 2);

    if (underrun) {
        // Signal emission from a non-Qt thread is fine (see handleRxTransfer);
        // QueuedConnection is what makes this safe to deliver to a
        // Qt-event-loop receiver.
        emit txUnderrun();
    }
    return 0;  // keep streaming
}

} // namespace AetherSDR::hackrf
