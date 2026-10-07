#pragma once

#include "core/backends/hackrf/HackRfDdc.h"
#include "core/backends/hackrf/HackRfZoomDecimator.h"
#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/dsp/WdspChannel.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QVector>

#include <atomic>
#include <complex>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace AetherSDR::hackrf {

// HackRF's receive chain on ITS OWN THREAD: the per-slice DDC, the wideband
// spectrum FFT, and WDSP demodulation. Live on a QThread of its own (the
// backend owns that thread); the GUI thread only ever sees finished spectrum
// and audio frames.
//
// It used to run on the GUI thread. WDSP is opened with blockForOutput, so each
// block waited for WDSP's worker; when WFM demodulation at 384 kHz ran even
// slightly slower than real time, the GUI stalled longer on every block (1 s,
// 4 s, 9 s, 27 s in one session) while IQ queued behind it without limit. Here
// that wait blocks only this thread, and the input is BOUNDED: a DSP that falls
// behind drops the oldest blocks, which costs an audio glitch, never a frozen
// application.
//
// Threading contract:
//   - enqueueIq(): any thread (the libhackrf/libusb callback thread in
//     practice). Never runs DSP; only queues and, if needed, schedules a drain.
//   - ddc() setters and setSpectrumIntervalMs(): any thread (atomics).
//   - installChannel / applySettings / clearChannel: any thread; queued onto
//     this object's thread so they never race a block in flight.
//   - spectrumFrame / audioFrame: emitted on this object's thread.
class HackRfRxDsp : public QObject {
    Q_OBJECT
public:
    // The live demodulator settings, applied whole so a channel installed late
    // (it is built off-thread at connect) starts on the CURRENT state.
    struct RxSettings {
        WdspChannel::Mode mode = WdspChannel::Mode::Wbfm;
        int filterLowHz = -40'000;
        int filterHighHz = 40'000;
        int agcMode = 3;
        double agcMaxGainDb = 65.0;
    };

    HackRfRxDsp(int spectrumFftSize, std::size_t audioBlockSize,
                std::size_t maxQueuedBlocks, QObject* parent = nullptr);

    HackRfDdc& ddc() { return *m_ddc; }
    void setSpectrumIntervalMs(int ms) { m_spectrumIntervalMs.store(ms); }
    // Narrow zoom: the spectrum is computed from the capture decimated by this
    // (1 = the raw capture). Any thread; applied at the next block.
    void setZoomDecimation(int decimation) { m_zoomDecimation.store(decimation); }

    void enqueueIq(QVector<std::complex<float>> block);
    // Blocks dropped because the DSP fell behind, since construction.
    quint64 droppedBlocks() const { return m_dropped.load(); }

    void installChannel(std::shared_ptr<WdspChannel> channel, RxSettings settings);
    void applySettings(RxSettings settings);
    // Drops the channel, any queued IQ and partial audio: a session ended.
    void clearChannel();

signals:
    void spectrumFrame(QByteArray binsDbfs);   // float32 bins
    void audioFrame(QByteArray pcmStereoFloat);

private:
    void drain();
    void processBlock(const QVector<std::complex<float>>& block);
    void onDecimated(const QVector<std::complex<float>>& iq);
    void applySettingsNow(const RxSettings& s);

    HackRfDdc* m_ddc;   // child: moves with this object
    hl2::Hl2Spectrum m_spectrum;
    std::vector<float> m_specBins;
    std::atomic<int> m_spectrumIntervalMs{33};
    QElapsedTimer m_clock;
    // Samples still to go before the next spectrum frame. Gated on the SAMPLE
    // clock so the frame rate does not depend on how much IQ one USB transfer
    // carries (65 ms at 2 MS/s, 6.5 ms at 20 MS/s).
    std::int64_t m_samplesToNextFrame{0};
    void processSpectrum(const std::complex<float>* data, std::size_t n, double rateHz);
    std::atomic<int> m_zoomDecimation{1};
    HackRfZoomDecimator m_zoom;   // this thread only

    // Input queue. Touched by the producer thread and this one, under m_queueMutex.
    std::mutex m_queueMutex;
    std::deque<QVector<std::complex<float>>> m_queue;
    const std::size_t m_maxQueued;
    std::atomic<bool> m_drainPending{false};
    std::atomic<quint64> m_dropped{0};
    quint64 m_droppedReported{0};
    qint64 m_lastDropLogMs{-1};

    // This thread only.
    std::shared_ptr<WdspChannel> m_channel;
    const std::size_t m_audioBlock;
    std::vector<std::complex<float>> m_audioIq;   // awaiting a full WDSP block
    std::vector<float> m_audioI, m_audioQ, m_left, m_right;
};

} // namespace AetherSDR::hackrf
