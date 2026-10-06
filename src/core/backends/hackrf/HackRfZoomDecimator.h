#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace AetherSDR::hackrf {

// Decimates HackRF's IQ for the narrow-zoom spectrum (spans below 8 MHz).
//
// The pan centre is the hardware LO, so a zoomed view is the middle of the
// capture, already at DC: no mixing, only lowpass + decimate. A cascade of
// half-band x2 stages (log2(D) of them) does it cheaply. Each stage is a 55-tap
// Blackman-windowed half-band filter (passband to 0.2 of its input rate,
// stopband from 0.3, ~74 dB down; every even-offset tap but the centre is
// zero, so an output costs ~28 multiply-adds). A boxcar like the slice DDC's
// is not good enough here: it lets strong signals just outside the view fold
// into it, which shows on a display as signals that are not there.
//
// Pure (no Qt); unit-tested (hackrf_zoom_decimator_test). Not thread-safe:
// one owner (HackRfRxDsp's thread).
class HackRfZoomDecimator {
public:
    HackRfZoomDecimator();
    // 1, 2, 4 .. 128 (other values round down to a power of two, max 128).
    // Resets the filter state when it changes.
    void setDecimation(int decimation);
    int decimation() const { return m_decimation; }
    void reset();
    // The decimated samples for `in`; state carries across calls, so chunking
    // does not change the output.
    std::vector<std::complex<float>> process(const std::vector<std::complex<float>>& in);
    std::vector<std::complex<float>> process(const std::complex<float>* in, std::size_t n);

private:
    struct Stage {
        std::vector<std::complex<float>> history;   // last taps-1 inputs
        bool phase = false;                          // emit on every second input
    };
    std::vector<float> m_taps;
    std::vector<Stage> m_stages;
    int m_decimation{1};
};

} // namespace AetherSDR::hackrf
