#include "HackRfDiscovery.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtGlobal>
#include <QSet>

#ifdef AETHER_BACKEND_HACKRF
#include <hackrf.h>
#endif

namespace AetherSDR {

// ---------------------------------------------------------------------------
// HackRfDiscovery  —  pattern-matches RtlSdrDiscovery for ConnectionPanel (#42)
// ---------------------------------------------------------------------------

HackRfDiscovery::HackRfDiscovery(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, &HackRfDiscovery::onScanTimer);
    connect(&m_watcher, &QFutureWatcher<QVector<RadioInfo>>::finished,
            this, &HackRfDiscovery::onScanFinished);
}

HackRfDiscovery::~HackRfDiscovery()
{
    stop();
    m_watcher.waitForFinished();
}

bool HackRfDiscovery::isRunning() const noexcept
{
    return m_timer->isActive() || m_scanning;
}

bool HackRfDiscovery::isAvailable()
{
#ifdef AETHER_BACKEND_HACKRF
    return true;
#else
    return false;
#endif
}

void HackRfDiscovery::start(int intervalMs)
{
    if (!isAvailable()) {
        return;
    }
    m_timer->start(intervalMs);
    onScanTimer(); // immediate first scan
}

void HackRfDiscovery::stop()
{
    m_timer->stop();
}

// ---------------------------------------------------------------------------
// Scan logic: enumerate USB devices in a background worker thread via QtConcurrent
// ---------------------------------------------------------------------------

void HackRfDiscovery::onScanTimer()
{
    if (!isAvailable() || m_scanning) {
        return; // Background scan still in progress or backend disabled
    }

    m_scanning = true;
    m_watcher.setFuture(QtConcurrent::run([]() -> QVector<RadioInfo> {
        QVector<RadioInfo> current;

#ifdef AETHER_BACKEND_HACKRF
        // hackrf_init()/hackrf_exit() are independently reference-counted by
        // libhackrf itself (both documented "can be safely called multiple
        // times", and hackrf_exit() returns HACKRF_ERROR_NOT_LAST_DEVICE
        // rather than tearing down shared state if a device is still open
        // elsewhere) — so this scan's own balanced init/exit pair is safe to
        // run concurrently with HackRfWorker's separate one on a connected
        // session, exactly like RtlSdrDiscovery's scan needs no coordination
        // with a connected RtlSdrWorker.
        if (hackrf_init() == HACKRF_SUCCESS) {
            hackrf_device_list_t* list = hackrf_device_list();
            if (list) {
                QSet<QString> usedSerials;
                for (int i = 0; i < list->devicecount; ++i) {
                    RadioInfo info;
                    info.family = QStringLiteral("hackrf");

                    const QString usbSerial = (list->serial_numbers && list->serial_numbers[i])
                        ? QString::fromUtf8(list->serial_numbers[i]).trimmed()
                        : QString();
                    info.serialIdentity.reportedSerial = usbSerial;
                    if (!usbSerial.isEmpty() && !usedSerials.contains(usbSerial)) {
                        info.serial = usbSerial;
                    } else {
                        info.serial = QStringLiteral("hackrf:%1").arg(i);
                        info.serialIdentity.indexLocator = true;
                    }
                    usedSerials.insert(info.serial);

                    QString model = QStringLiteral("HackRF");
                    if (list->usb_board_ids) {
                        const char* boardName = hackrf_usb_board_id_name(list->usb_board_ids[i]);
                        if (boardName && *boardName) {
                            model = QString::fromUtf8(boardName);
                        }
                    }
                    info.model = model;
                    info.name = model;
                    info.status = QStringLiteral("Available");
                    info.inUse = false;

                    current.append(info);
                }
                hackrf_device_list_free(list);
            }
            hackrf_exit();
        }
#endif
        return current;
    }));
}

void HackRfDiscovery::onScanFinished()
{
    m_scanning = false;
    const QVector<RadioInfo> current = m_watcher.result();

    if (!m_timer->isActive()) {
        return;
    }

    // Track which devices are still present; report lost ones
    QStringList lostSerials;
    for (auto it = m_seen.begin(); it != m_seen.end(); ++it) {
        const QString& serial = it.key();
        bool found = false;
        for (const auto& cur : current) {
            if (cur.serial == serial) {
                found = true;
                it.value().missedScans = 0;
                break;
            }
        }
        if (!found) {
            ++it.value().missedScans;
            if (it.value().missedScans >= kMissedScansBeforeLost) {
                lostSerials.append(serial);
            }
        }
    }

    // Remove lost entries
    for (const auto& serial : lostSerials) {
        emit radioLost(serial);
        m_seen.remove(serial);
    }

    // Report newly discovered or updated devices
    for (const auto& info : current) {
        if (!m_seen.contains(info.serial)) {
            m_seen[info.serial] = Seen{info, 0};
            emit radioDiscovered(info);
        } else {
            if (m_seen[info.serial].info.name != info.name ||
                m_seen[info.serial].info.model != info.model ||
                m_seen[info.serial].info.serialIdentity.reportedSerial
                    != info.serialIdentity.reportedSerial ||
                m_seen[info.serial].info.serialIdentity.indexLocator
                    != info.serialIdentity.indexLocator) {
                m_seen[info.serial].info = info;
                emit radioUpdated(info);
            }
        }
    }
}

}  // namespace AetherSDR
