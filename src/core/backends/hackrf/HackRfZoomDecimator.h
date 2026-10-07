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
    // Each stage keeps its inputs not yet consumed, split into I and Q (it
    // starts with taps-1 zeros: the filter's empty history). Output m reads
    // pending[2m .. 2m+taps-1]. Split by phase, a half-band output is a short
    // FIR over the even inputs plus the centre tap on the odd ones, both
    // contiguous, so the inner loops vectorize: on a laptop's low-power core
    // the scalar complex loop alone took 38-50% of the core and the DSP thread
    // fell behind the moment the operator zoomed in.
    // Only the LAST stage has to be sharp. An earlier stage only protects the
    // final view, which is a small fraction of its rate, so its transition
    // band is wide and a short filter does it: 55 taps for the last stage,
    // 23 for the one before, 15 for the rest (each 4k+3, so the centre is odd
    // and every even-offset tap but the centre is zero). 11 taps measured only
    // 54 dB against a fold; the test requires 60.
    struct Stage {
        std::vector<float> re, im;
        std::vector<float> pairTaps;   // taps[mid+k] for odd k = 1, 3, ..
        float centreTap = 0.5f;
        int taps = 55;
    };
    void runStage(Stage& s, const float* inRe, const float* inIm, std::size_t n,
                  std::vector<float>& outRe, std::vector<float>& outIm);
    static Stage makeStage(int taps);
    std::vector<Stage> m_stages;
    int m_decimation{1};
    // Scratch, reused across calls.
    std::vector<float> m_evRe, m_evIm, m_odRe, m_odIm;
    std::vector<float> m_aRe, m_aIm, m_bRe, m_bIm;
};

} // namespace AetherSDR::hackrf
