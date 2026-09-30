#pragma once

#include <QObject>
#include <QVector>

#include <complex>
#include <cstdint>
#include <vector>

namespace AetherSDR::hackrf {

// Hand-rolled FM transmit modulator: processed TX audio in, baseband IQ out
// at HackRF's own TX sample rate, ready for HackRfWorker::submitTxIq().
//
// NOT WdspChannel's TXA mode, and that is a considered choice, not an
// oversight — see HackRfBackend's own class comment for the full reasoning.
// In short: Hl2TxDsp (the seam's own TX precedent, which this backend's
// design doc says to follow) already tried WDSP's transmit channel and
// abandoned it after real, silent failures ("Underrun on most blocks and
// zeros on the rest") that were expensive to debug and impossible to detect
// from the air. Hl2TxDsp's own remedy — a small, directly-measurable
// hand-written modulator — is the one this class follows, and FM needs even
// less machinery than Hl2TxDsp's SSB phasing network: no Hilbert transform,
// just phase integration from the audio.
//
// RATES. Audio arrives at whatever rate IRadioBackend::submitTxAudio() is
// called with (AudioEngine's TX rate); HackRF's TX callback drains IQ at the
// full hardware sample rate (the same rate RX uses — hackrf_set_sample_rate
// applies to both directions), typically megasamples/sec, an order of
// magnitude above audio. Each audio sample is held (zero-order) across
// however many output samples it should span, using the same fractional
// phase-accumulator technique HackRfDdc/RtlSdrDdc use for their own
// non-integer rate ratios — just running in the interpolating direction
// instead of the decimating one, so it stays exact over time despite a
// ratio like 8,000,000/48,000 = 166.67 with no clean integer form.
//
// Deviation is narrow-FM by default (5 kHz), not broadcast WFM's 75 kHz —
// this backend's stated goal is amateur satellite/repeater FM (#42 design
// doc), which uses the same convention as 2m/70cm FM voice.
class HackRfTxDsp : public QObject {
    Q_OBJECT

public:
    explicit HackRfTxDsp(QObject* parent = nullptr);

    struct Config {
        int audioSampleRateHz = 24'000;             // AudioEngine's TX rate (matches Hl2Backend's own submitTxAudio expectation)
        double outputSampleRateHz = 8'000'000.0;    // HackRF's TX (== RX) sample rate
        double maxDeviationHz = 5'000.0;            // narrow-FM voice convention
    };

    Q_INVOKABLE void configure(const Config& config);
    [[nodiscard]] const Config& config() const noexcept { return m_config; }

public slots:
    // Mono TX audio at Config::audioSampleRateHz, range [-1, 1]. Values
    // outside that range are clamped, not rejected — an over-driven mic
    // becomes over-deviation, not a crash or a dropped block.
    void processAudioBlock(const std::vector<float>& mono);
    // Drop accumulated phase/resample state — call on unkey, so the next
    // transmission's carrier does not inherit the previous one's phase
    // (harmless for FM correctness, but keeps a fresh PTT edge starting
    // from a clean, predictable state rather than an arbitrary carry-over).
    void reset();

signals:
    void iqReady(const QVector<std::complex<float>>& iq);   // at outputSampleRateHz

private:
    Config m_config;
    double m_phase = 0.0;            // radians, wrapped to (-pi, pi]
    double m_resamplePhase = 0.0;    // fractional output-sample accumulator
};

}  // namespace AetherSDR::hackrf
