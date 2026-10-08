#include "HackRfRxDsp.h"

#include <QLoggingCategory>
#include <QMetaObject>

#include <span>
#include <utility>

Q_DECLARE_LOGGING_CATEGORY(lcHackRf)  // defined in HackRfWorker.cpp

namespace AetherSDR::hackrf {

HackRfRxDsp::HackRfRxDsp(int spectrumFftSize, std::size_t audioBlockSize,
                         std::size_t maxQueuedBlocks, QObject* parent)
    : QObject(parent)
    , m_ddc(new HackRfDdc(this))
    , m_spectrum(spectrumFftSize)
    , m_maxQueued(maxQueuedBlocks > 0 ? maxQueuedBlocks : 1)
    , m_audioBlock(audioBlockSize)
{
    m_clock.start();
    // Same thread as this object (the DDC is a child), so this is a direct
    // call inside processBlock(), not a queued hop.
    connect(m_ddc, &HackRfDdc::decimatedIqReady, this,
            [this](QVector<std::complex<float>> iq) { onDecimated(iq); });
}

void HackRfRxDsp::enqueueIq(QVector<std::complex<float>> block)
{
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_queue.size() >= m_maxQueued) {
            // Behind real time: lose the OLDEST data, so what plays next is as
            // close to now as the queue allows.
            m_queue.pop_front();
            ++m_dropped;
        }
        m_queue.push_back(std::move(block));
    }
    // One scheduled drain at a time. drain() clears the flag BEFORE it starts
    // popping, so a block queued while it runs schedules the next one.
    if (!m_drainPending.exchange(true))
        QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
}

void HackRfRxDsp::drain()
{
    m_drainPending.store(false);
    for (;;) {
        QVector<std::complex<float>> block;
        {
            const std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_queue.empty())
                break;
            block = std::move(m_queue.front());
            m_queue.pop_front();
        }
        // A settings change lands at the next block, not after the queue
        // empties: this loop runs as long as IQ keeps arriving, and a change
        // queued behind it on the event loop waited that long.
        applyPendingSettings();
        processBlock(block);
    }

    // Report drops at most every 2 s, with the running total.
    const quint64 dropped = m_dropped.load();
    if (dropped != m_droppedReported) {
        const qint64 now = m_clock.elapsed();
        if (m_lastDropLogMs < 0 || now - m_lastDropLogMs >= 2000) {
            qCWarning(lcHackRf) << "HackRF RX DSP behind real time: dropped"
                                << (dropped - m_droppedReported) << "IQ block(s),"
                                << dropped << "total";
            m_droppedReported = dropped;
            m_lastDropLogMs = now;
        }
    }
}

void HackRfRxDsp::processBlock(const QVector<std::complex<float>>& block)
{
    // Per-slice DDC -> onDecimated() -> WDSP, synchronously on this thread.
    m_ddc->process(block);

    // Narrow zoom: the pan centre is the LO, so the zoomed view is the middle
    // of the capture, already at DC: decimate it and transform that. A change
    // of zoom restarts the spectrum so no frame mixes two scales.
    const int zoom = m_zoomDecimation.load();
    if (zoom != m_zoom.decimation()) {
        m_zoom.setDecimation(zoom);
        m_spectrum.reset();
        m_samplesToNextFrame = 0;
    }
    const double rate = m_ddc->inputSampleRateHz();
    if (m_zoom.decimation() > 1) {
        const std::vector<std::complex<float>> zoomed =
            m_zoom.process(block.constData(), static_cast<std::size_t>(block.size()));
        processSpectrum(zoomed.data(), zoomed.size(), rate / m_zoom.decimation());
    } else {
        processSpectrum(block.constData(), static_cast<std::size_t>(block.size()), rate);
    }
}

// Wideband spectrum from the raw capture, one frame per interval of SAMPLES.
// Each frame is a single FFT of the most recent fftSize samples: between frames
// only the tail of the data is kept (Hl2Spectrum::accumulate), and one more
// sample through process() completes the frame. A transfer longer than the
// interval (a narrow zoom) therefore yields several frames instead of one.
void HackRfRxDsp::processSpectrum(const std::complex<float>* data, std::size_t n, double rateHz)
{
    const std::span<const std::complex<float>> all(data, n);
    const int interval = m_spectrumIntervalMs.load();
    if (interval <= 0) {                        // uncapped: every complete frame
        if (m_spectrum.process(all, m_specBins) > 0)
            emit spectrumFrame(QByteArray(reinterpret_cast<const char*>(m_specBins.data()),
                                          static_cast<int>(m_specBins.size() * sizeof(float))));
        return;
    }
    const std::int64_t every = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(rateHz * interval / 1000.0));
    const std::size_t keep = static_cast<std::size_t>(std::max(1, m_spectrum.fftSize() - 1));
    auto feedTail = [&](std::size_t from, std::size_t to) {   // [from, to)
        if (to <= from) return;
        const std::size_t len = std::min(to - from, keep);
        m_spectrum.accumulate(all.subspan(to - len, len));
    };
    std::size_t pos = 0;
    while (pos < n) {
        const std::size_t toDue = static_cast<std::size_t>(std::max<std::int64_t>(0, m_samplesToNextFrame));
        if (toDue >= n - pos) {
            feedTail(pos, n);
            m_samplesToNextFrame -= static_cast<std::int64_t>(n - pos);
            break;
        }
        feedTail(pos, pos + toDue);
        if (m_spectrum.process(all.subspan(pos + toDue, 1), m_specBins) > 0) {
            emit spectrumFrame(QByteArray(reinterpret_cast<const char*>(m_specBins.data()),
                                          static_cast<int>(m_specBins.size() * sizeof(float))));
        }
        pos += toDue + 1;
        m_samplesToNextFrame = every - 1;
    }
}

void HackRfRxDsp::onDecimated(const QVector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;
    m_audioIq.insert(m_audioIq.end(), iq.begin(), iq.end());

    const std::size_t outN = m_channel->outputBlockSize();
    if (m_audioI.size() != m_audioBlock) {
        m_audioI.assign(m_audioBlock, 0.0f);
        m_audioQ.assign(m_audioBlock, 0.0f);
    }
    if (m_left.size() != outN) {
        m_left.assign(outN, 0.0f);
        m_right.assign(outN, 0.0f);
    }

    QByteArray pcm;
    std::size_t consumed = 0;
    while (m_audioIq.size() - consumed >= m_audioBlock) {
        for (std::size_t n = 0; n < m_audioBlock; ++n) {
            // CONJUGATED. WDSP's RXA, as this repo configures it, selects the
            // OPPOSITE sign to its passband bounds: USB [+100, +2900] passes
            // NEGATIVE frequencies (measured, and the reason Hl2RxDsp feeds
            // it the HPSDR wire, which is itself conjugated). HackRF's IQ is
            // analytic -- a signal above the dial is a positive frequency --
            // so fed straight in, USB heard the lower sideband and LSB the
            // upper (hackrf_rx_modes_test). FM and AM are symmetric, which is
            // how it went unnoticed.
            m_audioI[n] = m_audioIq[consumed + n].real();
            m_audioQ[n] = -m_audioIq[consumed + n].imag();
        }
        consumed += m_audioBlock;
        // blockForOutput: waits for WDSP's worker. On THIS thread, which is the
        // point of the class.
        if (m_channel->processIq(m_audioI, m_audioQ, m_left, m_right)
            != WdspChannel::ProcessResult::Ok) {
            continue;   // the pipeline is still filling
        }
        const int before = pcm.size();
        pcm.resize(before + static_cast<int>(outN * 2 * sizeof(float)));
        auto* out = reinterpret_cast<float*>(pcm.data() + before);
        for (std::size_t k = 0; k < outN; ++k) {
            out[2 * k] = m_left[k];
            out[2 * k + 1] = m_right[k];
        }
    }
    if (consumed > 0)
        m_audioIq.erase(m_audioIq.begin(), m_audioIq.begin() + static_cast<std::ptrdiff_t>(consumed));
    if (!pcm.isEmpty())
        emit audioFrame(pcm);
}

void HackRfRxDsp::installChannel(std::shared_ptr<WdspChannel> channel, RxSettings settings)
{
    QMetaObject::invokeMethod(this, [this, channel = std::move(channel), settings] {
        m_channel = channel;
        m_audioIq.clear();
        // Built from the state at connect: catch up with anything changed since.
        applySettingsNow(settings);
    }, Qt::QueuedConnection);
}

void HackRfRxDsp::applySettings(RxSettings settings)
{
    {
        const std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_pendingSettings = settings;
    }
    // Also queued, for when no IQ is flowing to carry it in.
    QMetaObject::invokeMethod(this, [this] { applyPendingSettings(); }, Qt::QueuedConnection);
}

void HackRfRxDsp::applyPendingSettings()
{
    std::optional<RxSettings> s;
    {
        const std::lock_guard<std::mutex> lock(m_settingsMutex);
        s.swap(m_pendingSettings);
    }
    if (s)
        applySettingsNow(*s);
}

void HackRfRxDsp::applySettingsNow(const RxSettings& s)
{
    if (!m_channel)
        return;
    m_channel->setMode(s.mode);
    m_channel->setFilter(s.filterLowHz, s.filterHighHz);
    m_channel->setAgc(s.agcMode, s.agcMaxGainDb);
}

void HackRfRxDsp::clearChannel()
{
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }
    QMetaObject::invokeMethod(this, [this] {
        m_channel.reset();
        m_audioIq.clear();
        m_samplesToNextFrame = 0;
    }, Qt::QueuedConnection);
}

} // namespace AetherSDR::hackrf
