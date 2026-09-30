#pragma once

#include <QObject>
#include <QVector>

#include <atomic>
#include <complex>

namespace AetherSDR::hackrf {

// Per-slice digital downconverter (#42 design doc) — the one RX-side piece
// with no direct precedent: RtlSdrBackend only ever needed one DDC, and
// HackRF's v1 wants N concurrent instances tuning independent slices out of
// ONE shared wideband capture (matching Flex/HL2's multi-receiver model).
//
// Extends RtlSdrDdc's proven NCO-shift + boxcar-decimate technique (its own
// "Stage 2: fractional boxcar resampling" comment — the trick that avoids
// drift for input/output rate PAIRS with no clean integer ratio, e.g.
// 8,000,000 Hz in / 48,000 Hz out = 166.67) rather than inventing a new one.
// The scope is deliberately narrower than RtlSdrDdc, though: RtlSdrDdc does
// EVERYTHING (FFT spectrum, NCO+decimate, AND its own home-grown FM/AM/SSB
// demodulator), because RTL has nothing else to demodulate with.
// HackRfBackend's design intends a WDSP RXA channel per slice (HL2's
// pattern) to do demodulation/filtering/AGC — not yet wired (a separate
// #42 item) — so HackRfDdc's job stops at producing correctly-tuned,
// correctly-decimated IQ at WDSP's expected 48000 Hz input rate (see
// Hl2RxDsp::Config::inputSampleRateHz's own comment on why 48 kHz: every
// HL2 IQ rate divides evenly into it, and the same reasoning applies to any
// consumer expecting that rate). Wideband spectrum/waterfall for the
// panadapter is a separate, backend-level concern computed once from the
// shared raw feed — duplicating an FFT per slice here would be wasteful,
// and HackRfDdc's job is narrowband, not the whole span.
//
// Multi-instance safety: an instance holds no state shared with any other
// instance and no global/static data — N instances fed the same wideband
// samples (each with its own setSliceFrequencyHz()) are independent by
// construction, not by any explicit synchronization this class adds.
class HackRfDdc : public QObject {
    Q_OBJECT

public:
    explicit HackRfDdc(QObject* parent = nullptr);

    // The wideband capture's own parameters — the same for every HackRfDdc
    // instance fed from one HackRfWorker, set by HackRfBackend whenever the
    // hardware's tuned center or sample rate changes.
    void setInputSampleRateHz(double hz);
    void setCenterFrequencyHz(double hz);

    // This instance's target — independent per slice.
    void setSliceFrequencyHz(double hz);

    // Defaults to 48000 Hz, matching Hl2RxDsp::Config::inputSampleRateHz —
    // a WDSP RXA channel's expected IQ input rate. Settable rather than
    // hardcoded so a future consumer with different needs (a lower-rate
    // CW-only channel, a test) isn't locked to WDSP's number.
    void setOutputSampleRateHz(double hz);

public slots:
    // Runs on whatever thread calls it — HackRfBackend, after receiving
    // HackRfWorker::rxIqReady on its own thread (already queued there by
    // Qt), calls this directly on each active slice's HackRfDdc. No
    // internal locking: the atomics below only guard cross-thread
    // PARAMETER updates (setSliceFrequencyHz() etc., called from wherever
    // IRadioBackend::setSliceFrequency() runs), not concurrent process()
    // calls — exactly RtlSdrDdc's own division of responsibility.
    void process(const QVector<std::complex<float>>& wideband);

signals:
    void decimatedIqReady(QVector<std::complex<float>> iq);

private:
    std::atomic<double> m_inputRateHz{8'000'000.0};
    std::atomic<double> m_centerHz{100'000'000.0};
    std::atomic<double> m_sliceHz{100'000'000.0};
    std::atomic<double> m_outputRateHz{48'000.0};

    // NCO state — see RtlSdrDdc::processAudio's own comment on why the
    // phasor is periodically renormalized (floating-point magnitude drift
    // over a long-running multiply chain).
    std::complex<float> m_ncoPhasor{1.0f, 0.0f};
    std::uint32_t m_ncoNormalizeCounter{0};

    // Boxcar accumulator (anti-alias lowpass) + fractional resample phase —
    // RtlSdrDdc's stage-2 technique, generalized to decimate directly to
    // complex IQ instead of post-demod mono audio.
    std::complex<float> m_decimAcc{0.0f, 0.0f};
    int m_decimCount{0};
    double m_resamplePhase{0.0};
};

} // namespace AetherSDR::hackrf
