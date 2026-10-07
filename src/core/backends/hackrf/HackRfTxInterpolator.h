#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace AetherSDR::hackrf {

// Raises transmit baseband IQ (SSB at 48 kHz, AM/DSB at 24 kHz) to the
// HackRF's sample rate (8 or 16 MS/s, up to 20 on a HackRF One) without
// spurious images.
//
// Holding each sample across the output samples it spans, as the FM modulator
// does with audio, is not good enough for IQ: it leaves a copy of the signal
// at every multiple of the input rate only ~13 dB down, and those copies go
// out on the air. Instead: a cascade of half-band x2 stages up to ~384 kHz,
// then linear interpolation for the rest, which is fractional (8 MS/s from
// 384 kHz is 20.83). By then the signal is a small fraction of the rate, and
// linear interpolation's images have an amplitude of about (f / rate)^2:
// 80+ dB down for voice (hackrf_tx_interpolator_test).
//
// The mirror image of HackRfZoomDecimator: here the FIRST stage (the lowest
// rate, where the signal fills most of the band) needs the sharp filter and
// the later ones only short filters: 55 taps, then 23, then 15. Each is a
// Blackman-windowed half-band (4k+3 taps), so only half its taps are nonzero
// and every second output is a copy of an input.
//
// Pure (no Qt); unit-tested. Not thread-safe: one owner.
class HackRfTxInterpolator {
public:
    // Rates in Hz; outRate > inRate. Resets the state.
    void configure(double inRateHz, double outRateHz);
    void reset();
    std::vector<std::complex<float>> process(const std::vector<std::complex<float>>& in);
    std::vector<std::complex<float>> process(const std::complex<float>* in, std::size_t n);

    double inputRateHz() const { return m_inRate; }
    double outputRateHz() const { return m_outRate; }

private:
    struct Stage {
        std::vector<float> evenTaps;   // 2 * h[2i]: the filtered output phase
        int delay = 0;                 // the copied phase's delay, in inputs
        std::vector<std::complex<float>> hist;   // last taps inputs, newest last
    };
    static Stage makeStage(int taps);
    void runStage(Stage& s, const std::vector<std::complex<float>>& in,
                  std::vector<std::complex<float>>& out);

    double m_inRate = 48'000.0;
    double m_outRate = 8'000'000.0;
    double m_midRate = 48'000.0;   // after the half-bands, into the linear step
    std::vector<Stage> m_stages;

    // Linear step: output n sits at position m_pos between m_prev and m_cur.
    std::complex<float> m_prev{0, 0}, m_cur{0, 0};
    double m_pos = 0.0;
    double m_step = 0.0;   // midRate / outRate

    std::vector<std::complex<float>> m_b;   // scratch: a stage's work buffer
};

} // namespace AetherSDR::hackrf
