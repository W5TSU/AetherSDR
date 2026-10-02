// HackRfCwTx: the keyed CW carrier HackRF transmits, as IQ.
//
// Pins the timing model (edges placed by their arrival timestamp plus a fixed
// latency, so GUI-thread scheduling jitter never reaches the air), the
// raised-cosine edges (no key clicks), exact element lengths, the first
// element surviving a late TX start, and a late edge being clamped rather than
// lost. Runs at 48 kHz so it is fast; the generator is rate-agnostic.
#include "core/backends/hackrf/HackRfCwTx.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hackrf;

namespace {

int g_failed = 0;
void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failed;
}

constexpr double kRate = 48'000.0;
constexpr double kLatencyS = 0.050;
constexpr double kRampS = 0.005;
constexpr float kAmp = 0.5f;

HackRfCwTx makeTx()
{
    HackRfCwTx tx;
    HackRfCwTx::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.rampMs = kRampS * 1000.0;
    cfg.latencyMs = kLatencyS * 1000.0;
    cfg.amplitude = kAmp;
    tx.configure(cfg);
    return tx;
}

std::vector<float> renderMag(HackRfCwTx& tx, std::size_t n)
{
    std::vector<std::complex<float>> iq(n);
    tx.render(iq.data(), n);
    std::vector<float> mag(n);
    for (std::size_t i = 0; i < n; ++i) mag[i] = std::abs(iq[i]);
    return mag;
}

// First sample at or above half amplitude, from `from`; -1 if none.
long firstAbove(const std::vector<float>& m, std::size_t from, float thr)
{
    for (std::size_t i = from; i < m.size(); ++i) if (m[i] >= thr) return static_cast<long>(i);
    return -1;
}
long firstBelow(const std::vector<float>& m, std::size_t from, float thr)
{
    for (std::size_t i = from; i < m.size(); ++i) if (m[i] < thr) return static_cast<long>(i);
    return -1;
}

} // namespace

int main()
{
    const float half = kAmp * 0.5f;
    const long latencySamples = std::lround(kLatencyS * kRate);

    // ---- silence before any key ----
    {
        auto tx = makeTx();
        const auto m = renderMag(tx, 4800);
        float peak = 0; for (float v : m) peak = std::max(peak, v);
        check(peak == 0.0f, "no key: silence");
        check(tx.isIdle(), "no key: idle");
    }

    // ---- a 30 WPM dit (40 ms) is exactly 40 ms, delayed by the latency ----
    {
        auto tx = makeTx();
        tx.keyEdge(true, 10.000);
        tx.keyEdge(false, 10.040);
        const auto m = renderMag(tx, static_cast<std::size_t>(0.2 * kRate));
        const long on = firstAbove(m, 0, half);
        const long off = firstBelow(m, static_cast<std::size_t>(std::max(on, 0L)), half);
        std::printf("key-on at sample %ld (latency %ld), length %ld samples (want %ld)\n",
                    on, latencySamples, off - on, std::lround(0.040 * kRate));
        check(on >= latencySamples && on <= latencySamples + std::lround(kRampS * kRate),
              "the element starts one latency after its key-down");
        check(std::labs((off - on) - std::lround(0.040 * kRate)) <= 2,
              "the element is exactly 40 ms long at half amplitude");
        float plateau = m[static_cast<std::size_t>(on + std::lround(0.020 * kRate))];
        check(std::fabs(plateau - kAmp) < 1e-4f, "full amplitude mid-element");
        check(m.back() == 0.0f, "silence after the element");
        check(tx.isIdle(), "idle once the element and its fall are rendered");
    }

    // ---- raised-cosine edges: monotonic, no step bigger than a smooth ramp ----
    {
        auto tx = makeTx();
        tx.keyEdge(true, 0.0);
        const auto m = renderMag(tx, static_cast<std::size_t>(0.1 * kRate));
        const long start = latencySamples;
        const long rampN = std::lround(kRampS * kRate);
        bool monotonic = true;
        float maxStep = 0.0f;
        for (long i = start; i < start + rampN + 2; ++i) {
            if (m[static_cast<std::size_t>(i + 1)] + 1e-7f < m[static_cast<std::size_t>(i)]) monotonic = false;
            maxStep = std::max(maxStep, m[static_cast<std::size_t>(i + 1)] - m[static_cast<std::size_t>(i)]);
        }
        // A raised cosine over N samples has max slope A*pi/(2N).
        const float bound = kAmp * 3.15f / (2.0f * static_cast<float>(rampN)) * 1.05f;
        check(monotonic, "the rise is monotonic");
        check(maxStep <= bound, "the rise has no step steeper than a raised cosine (no click)");
        check(m[static_cast<std::size_t>(start - 1)] == 0.0f, "nothing before the edge");
    }

    // ---- TX starting late does not clip the first element ----
    {
        auto tx = makeTx();
        tx.keyEdge(true, 5.000);
        tx.keyEdge(false, 5.060);    // 60 ms element
        // (rendering begins whenever TX streams: the timeline anchors there)
        const auto m = renderMag(tx, static_cast<std::size_t>(0.3 * kRate));
        const long on = firstAbove(m, 0, half);
        const long off = firstBelow(m, static_cast<std::size_t>(on), half);
        check(std::labs((off - on) - std::lround(0.060 * kRate)) <= 2,
              "the first element keeps its full length");
    }

    // ---- relative timing across elements is preserved ----
    {
        auto tx = makeTx();
        // dit, 40 ms gap, dah (120 ms) at 30 WPM
        tx.keyEdge(true, 1.000);  tx.keyEdge(false, 1.040);
        tx.keyEdge(true, 1.080);  tx.keyEdge(false, 1.200);
        const auto m = renderMag(tx, static_cast<std::size_t>(0.4 * kRate));
        const long on1 = firstAbove(m, 0, half);
        const long off1 = firstBelow(m, static_cast<std::size_t>(on1), half);
        const long on2 = firstAbove(m, static_cast<std::size_t>(off1), half);
        const long off2 = firstBelow(m, static_cast<std::size_t>(on2), half);
        check(std::labs((on2 - on1) - std::lround(0.080 * kRate)) <= 2,
              "element starts keep their spacing");
        check(std::labs((off2 - on2) - std::lround(0.120 * kRate)) <= 2,
              "the dah is exactly 120 ms");
    }

    // ---- an edge that arrives after its slot was rendered is clamped, not lost ----
    {
        auto tx = makeTx();
        tx.keyEdge(true, 2.000);
        (void)renderMag(tx, static_cast<std::size_t>(0.2 * kRate));   // render well past
        tx.keyEdge(false, 2.010);   // its slot (latency + 10 ms) is already history
        const auto m = renderMag(tx, static_cast<std::size_t>(0.05 * kRate));
        check(m.back() == 0.0f, "a late key-up still ends the element");
    }

    // ---- PTT held first (rendering already running): timing is still exact ----
    {
        auto tx = makeTx();
        (void)renderMag(tx, static_cast<std::size_t>(1.0 * kRate));   // 1 s of keyed-up PTT
        tx.keyEdge(true, 7.000);  tx.keyEdge(false, 7.040);
        tx.keyEdge(true, 7.080);  tx.keyEdge(false, 7.120);
        const auto m = renderMag(tx, static_cast<std::size_t>(0.4 * kRate));
        const long on1 = firstAbove(m, 0, half);
        const long off1 = firstBelow(m, static_cast<std::size_t>(on1), half);
        const long on2 = firstAbove(m, static_cast<std::size_t>(off1), half);
        check(on1 >= latencySamples && on1 <= latencySamples + std::lround(kRampS * kRate),
              "under a held PTT the first element still starts one latency later");
        check(std::labs((off1 - on1) - std::lround(0.040 * kRate)) <= 2
                  && std::labs((on2 - on1) - std::lround(0.080 * kRate)) <= 2,
              "...and element lengths and spacing are exact, not clamped to now");
    }

    // ---- reset starts a fresh transmission ----
    {
        auto tx = makeTx();
        tx.keyEdge(true, 3.0);
        (void)renderMag(tx, 4800);
        tx.reset();
        const auto m = renderMag(tx, 4800);
        float peak = 0; for (float v : m) peak = std::max(peak, v);
        check(peak == 0.0f && tx.isIdle(), "reset: silence and idle");
    }

    std::printf("%s\n", g_failed == 0 ? "hackrf_cw_tx_test: OK" : "hackrf_cw_tx_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
