#pragma once

#include <QObject>
#include <QVector>

#include "core/backends/hackrf/HackRfTxInterpolator.h"

#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace AetherSDR::hl2 { class Hl2TxDsp; }

namespace AetherSDR::hackrf {

// HackRF's transmit modulator: processed TX audio in, baseband IQ out at
// HackRF's own TX sample rate, ready for HackRfWorker::submitTxIq().
//
// MODES. FM was the only one at first; SSB, AM and DSB were refused as
// receive-only. Now (hackrf_tx_modes_test):
//   - FM (FM, FMN, DFM): phase integration, as described below.
//   - SSB (USB/DIGU/RTTY, LSB/DIGL): Hl2TxDsp's phasing modulator -- the one
//     proven on the air for the HL2, with its transmit bandpass and
//     protection-only ALC -- at 48 kHz. Its output is conjugated for the
//     HPSDR wire, whose handedness is the opposite of the analytic
//     convention; HackRF's IQ is analytic, so it is conjugated back here.
//   - AM (AM, SAM): carrier plus band-limited audio, carrier at half scale so
//     full modulation peaks at full scale. DSB: the audio alone.
// Every mode then goes through HackRfTxInterpolator up to the hardware rate,
// FM included (its audio, before integration): holding samples left copies
// of the signal at multiples of the low rate only 13-60 dB down, which go
// out on the air.
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
    ~HackRfTxDsp() override;

    struct Config {
        int audioSampleRateHz = 24'000;             // AudioEngine's TX rate (matches Hl2Backend's own submitTxAudio expectation)
        double outputSampleRateHz = 8'000'000.0;    // HackRF's TX (== RX) sample rate
        double maxDeviationHz = 5'000.0;            // narrow-FM voice convention
        enum class Modulation { Fm, Usb, Lsb, Am, Dsb };
        Modulation modulation = Modulation::Fm;
        // The audio passband for SSB, AM and DSB (positive, audio-domain:
        // the modulation picks the sideband). hl2::defaultTxPassbandForModeName.
        double filterLowHz = 300.0;
        double filterHighHz = 2700.0;
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
    void emitInterpolated(const std::vector<std::complex<float>>& baseband);
    void designAudioBandpass();

    Config m_config;
    double m_phase = 0.0;            // FM: radians, wrapped to (-pi, pi]
    HackRfTxInterpolator m_interp;   // baseband (audio rate, or 48 kHz SSB) -> output rate
    std::unique_ptr<hl2::Hl2TxDsp> m_ssb;   // SSB only
    // AM/DSB: a windowed-sinc audio bandpass at the audio rate.
    std::vector<float> m_bandpass;
    std::vector<float> m_bpHist;
    std::size_t m_bpPos = 0;
};

}  // namespace AetherSDR::hackrf
