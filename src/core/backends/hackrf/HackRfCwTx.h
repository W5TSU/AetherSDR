#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace AetherSDR::hackrf {

// The keyed CW carrier HackRF transmits, rendered as IQ. Pure (no Qt, no
// libhackrf), so its timing is unit-testable.
//
// HackRF has no CW hardware: the host makes the waveform. The carrier sits on
// the dial frequency (the CW convention: the marker is on the signal), which
// is DC in the TX IQ, so a sample is just the envelope amplitude.
//
// TIMING. Key edges reach the backend on the GUI thread, whose scheduling
// jitter (5-20 ms) would be audible at 25+ WPM if edges were applied when they
// arrived. Instead each edge carries its arrival timestamp and lands at
//     sample = anchor + (edgeTime - firstEdgeTime) * rate
// where anchor is the render position when the first edge arrived plus the
// latency, on a sample counter that runs with the transmission. Element
// lengths and spacing are therefore exact; the transmission is merely delayed
// by `latency` (plus however long the RX->TX switch took, which also means the
// first element is never clipped by that switch). An edge
// whose slot was already rendered (it arrived later than the latency allows)
// is applied at the current position rather than lost.
//
// EDGES. A raised cosine of rampMs on both rise and fall, so the half-amplitude
// points are exactly the edge times and the spectrum stays clean (no key clicks).
class HackRfCwTx {
public:
    struct Config {
        double sampleRateHz = 8'000'000.0;
        double rampMs = 5.0;
        double latencyMs = 50.0;
        float amplitude = 0.7f;
    };

    void configure(const Config& config);
    // A new transmission: silence, no pending edges, timeline unanchored.
    void reset();
    // A key edge at `timeSeconds` (any monotonic clock, seconds).
    void keyEdge(bool down, double timeSeconds);
    // The next `n` samples of the transmission.
    void render(std::complex<float>* out, std::size_t n);
    // Nothing pending and the carrier fully down.
    bool isIdle() const { return m_edges.empty() && m_rampPos == 0 && !m_target; }

private:
    Config m_cfg;
    std::vector<float> m_ramp;          // raised cosine, m_ramp[0] = 0 .. m_ramp[N] = 1
    std::int64_t m_latencySamples{0};
    std::deque<std::pair<std::int64_t, bool>> m_edges;   // (sample, down)
    std::int64_t m_pos{0};              // next sample to render
    bool m_originSet{false};
    double m_originSeconds{0.0};
    std::int64_t m_anchorSample{0};     // sample the first edge lands on
    bool m_target{false};               // key state the envelope moves toward
    std::size_t m_rampPos{0};           // index into m_ramp
};

} // namespace AetherSDR::hackrf
