// aetherd HL2 Phase 1a — Hl2Spectrum unit test. Feeds synthetic IQ through the
// FFT panadapter path and checks: a complex tone peaks at the expected
// fftshifted bin, DC lands at the centre bin, partial frames accumulate across
// calls, and a large DC offset (the direct-sampling ADC bias) is removed so it
// does not swamp a real tone — mirroring the tools/hl2/spectrum.py behavior.
//
// Plan 3 adds frame averaging: setAveraging(1, *) is a passthrough, an
// unweighted boxcar is the arithmetic mean of the last k frame traces (dB),
// a weighted EMA steps monotonically toward a new level and settles there,
// switching averaging mid-stream never resizes or restarts the FFT, and
// reset() drops the averaging history.

#include "core/backends/hl2/Hl2Spectrum.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr double kPi = 3.14159265358979323846;

// Complex tone at integer bin k0: amp * exp(i 2π k0 n / N).
static std::vector<std::complex<float>> tone(int n, int k0, float amp, std::complex<float> dc = {})
{
    std::vector<std::complex<float>> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * k0 * i / n;
        v[static_cast<std::size_t>(i)] =
            amp * std::complex<float>(static_cast<float>(std::cos(ph)),
                                      static_cast<float>(std::sin(ph))) + dc;
    }
    return v;
}

static int argmax(const std::vector<float>& v)
{
    int m = 0;
    for (int i = 1; i < static_cast<int>(v.size()); ++i)
        if (v[static_cast<std::size_t>(i)] > v[static_cast<std::size_t>(m)]) m = i;
    return m;
}

int main()
{
    constexpr int N = 64;
    const int half = N / 2;

    // ---- tone at bin 10 -> peak at fftshifted bin (10 + 32) % 64 = 42 ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        const int frames = spec.process(tone(N, 10, 0.5f), bins);
        check(frames == 1, "one frame from N samples");
        check(bins.size() == static_cast<std::size_t>(N), "N bins produced");
        const int peak = argmax(bins);
        check(peak == (10 + half) % N, "tone peaks at expected fftshifted bin");
        check(bins[static_cast<std::size_t>(peak)] > -8.0f, "peak near -6 dBFS (amp 0.5)");
        check(bins[static_cast<std::size_t>((peak + half) % N)] < -30.0f, "opposite bin is floor");
    }

    // ---- DC (bin 0) lands at the centre bin ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        // A pure DC tone would be removed by DC subtraction, so use a near-DC
        // bin (k0 = 1) and confirm it maps just off centre, and a real DC-bin
        // signal that survives: feed bin 1.
        spec.process(tone(N, 1, 0.5f), bins);
        check(argmax(bins) == (1 + half) % N, "bin-1 tone maps adjacent to centre");
    }

    // ---- partial frames accumulate across process() calls ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        const auto t = tone(N, 7, 0.5f);
        std::span<const std::complex<float>> s(t);
        check(spec.process(s.subspan(0, 30), bins) == 0, "30/64 -> no frame yet");
        check(spec.process(s.subspan(30), bins) == 1, "remaining 34 completes the frame");
        check(argmax(bins) == (7 + half) % N, "accumulated frame decodes the tone");
    }

    // ---- large DC offset is removed, tone survives ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        // DC offset of 0.4 on I (as a real HL2 ADC bias) + a smaller tone at bin 20.
        spec.process(tone(N, 20, 0.1f, std::complex<float>(0.4f, 0.0f)), bins);
        const int peak = argmax(bins);
        check(peak == (20 + half) % N, "tone peak survives a large DC offset (DC removed)");
        check(bins[static_cast<std::size_t>(half)] < bins[static_cast<std::size_t>(peak)],
              "centre (DC) bin is below the tone after DC removal");
    }

    // ==== Plan 3: frame averaging ==========================================

    // The single-frame (unaveraged) dBFS value at a tone's fftshifted peak bin.
    auto rawPeakDb = [&](int k0, float amp) {
        Hl2Spectrum s(N);
        std::vector<float> b;
        s.process(tone(N, k0, amp), b);
        return b[static_cast<std::size_t>((k0 + half) % N)];
    };

    // ---- setAveraging(1, *) is a bit-for-bit passthrough ----
    {
        Hl2Spectrum plain(N);
        Hl2Spectrum avg1(N);
        avg1.setAveraging(1, true);   // frames <= 1 -> no averaging at all
        std::vector<float> a, b;
        plain.process(tone(N, 12, 0.5f), a);
        avg1.process(tone(N, 12, 0.5f), b);
        check(a.size() == b.size(), "avg(1): same bin count as unaveraged");
        bool same = a.size() == b.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) {
            if (std::fabs(a[i] - b[i]) > 1e-4f) {
                same = false;
            }
        }
        check(same, "setAveraging(1) is identical to the single-frame spectrum");
    }

    // ---- unweighted boxcar: output is the mean of the last k frame traces ----
    {
        constexpr int K = 4;
        constexpr int k0 = 9;
        const int pk = (k0 + half) % N;
        Hl2Spectrum s(N);
        s.setAveraging(K, false);
        std::vector<float> bins;
        for (int f = 0; f < K - 1; ++f) {
            s.process(tone(N, k0, 0.5f), bins);   // K-1 frames at -6 dBFS
        }
        s.process(tone(N, k0, 0.05f), bins);      // one frame ~20 dB down
        const float hi = rawPeakDb(k0, 0.5f);
        const float lo = rawPeakDb(k0, 0.05f);
        const float expected = ((K - 1) * hi + lo) / K;
        check(std::fabs(bins[static_cast<std::size_t>(pk)] - expected) < 0.05f,
              "boxcar output is the arithmetic mean of the last k frame values");
    }

    // ---- boxcar of a steady tone equals that tone's single-frame value ----
    {
        constexpr int K = 8;
        constexpr int k0 = 15;
        const int pk = (k0 + half) % N;
        Hl2Spectrum s(N);
        s.setAveraging(K, false);
        std::vector<float> bins;
        for (int f = 0; f < 4 * K; ++f) {
            s.process(tone(N, k0, 0.3f), bins);
        }
        check(std::fabs(bins[static_cast<std::size_t>(pk)] - rawPeakDb(k0, 0.3f)) < 1e-3f,
              "a steady tone through the boxcar reaches the single-frame value");
    }

    // ---- weighted EMA: converge, then a monotone partway step to a new level ----
    {
        constexpr int K = 10;
        constexpr int k0 = 11;
        const int pk = (k0 + half) % N;
        Hl2Spectrum s(N);
        s.setAveraging(K, true);                  // EMA, alpha = 2 / (K + 1)
        std::vector<float> bins;
        for (int f = 0; f < 200; ++f) {
            s.process(tone(N, k0, 0.5f), bins);
        }
        const float hi = rawPeakDb(k0, 0.5f);
        const float lo = rawPeakDb(k0, 0.05f);
        check(std::fabs(bins[static_cast<std::size_t>(pk)] - hi) < 0.2f,
              "EMA converges to a steady tone's single-frame value");
        float prev = bins[static_cast<std::size_t>(pk)];
        bool monotoneDown = true;
        for (int f = 0; f < K; ++f) {
            s.process(tone(N, k0, 0.05f), bins);
            const float cur = bins[static_cast<std::size_t>(pk)];
            if (cur > prev + 1e-3f) {
                monotoneDown = false;
            }
            prev = cur;
        }
        check(monotoneDown, "EMA steps monotonically toward a lower level");
        check(prev < hi - 0.3f * (hi - lo) && prev > lo + 0.05f * (hi - lo),
              "after k frames the EMA is partway to the new level, not snapped");
        for (int f = 0; f < 400; ++f) {
            s.process(tone(N, k0, 0.05f), bins);
        }
        check(std::fabs(bins[static_cast<std::size_t>(pk)] - lo) < 0.2f,
              "EMA eventually settles at the new level");
    }

    // ---- switching averaging at runtime never resizes or restarts the FFT ----
    {
        Hl2Spectrum s(N);
        std::vector<float> bins;
        s.process(tone(N, 8, 0.4f), bins);
        check(s.fftSize() == N, "fftSize is N before setAveraging");
        s.setAveraging(6, true);
        check(s.fftSize() == N, "fftSize is unchanged after setAveraging");
        const int frames = s.process(tone(N, 8, 0.4f), bins);
        check(frames == 1 && bins.size() == static_cast<std::size_t>(N),
              "a full-size frame still comes out right after switching averaging");
        check(argmax(bins) == (8 + half) % N,
              "the tone still decodes after switching averaging mid-stream");
    }

    // ---- tone + noise: averaging cuts the noise floor's frame-to-frame swing ----
    {
        // The property story 7 asks for — "trade responsiveness against a
        // smoother noise floor". Feed a fixed tone plus fresh white noise every
        // frame; watch one off-tone bin over many frames through the boxcar and
        // through no averaging, and confirm the averaged bin's temporal spread
        // is markedly smaller. (dB-domain averaging settles toward the mean of
        // the logs, a stable value — the point here is the reduced variance,
        // not an unbiased power estimate.)
        constexpr int K = 16;
        constexpr int k0 = 10;
        const int floorBin = (k0 + half) % N + 6;   // well off the tone
        std::uint32_t rng = 0x1234567u;
        auto noisyTone = [&]() {
            std::vector<std::complex<float>> v(static_cast<std::size_t>(N));
            for (int i = 0; i < N; ++i) {
                const double ph = 2.0 * kPi * k0 * i / N;
                auto u = [&]() {
                    rng = rng * 1664525u + 1013904223u;
                    return (static_cast<float>(rng >> 8) / 16777216.0f - 0.5f);
                };
                v[static_cast<std::size_t>(i)] =
                    0.3f * std::complex<float>(static_cast<float>(std::cos(ph)),
                                               static_cast<float>(std::sin(ph)))
                    + 0.08f * std::complex<float>(u(), u());
            }
            return v;
        };
        auto spread = [&](bool averaged) {
            Hl2Spectrum s(N);
            if (averaged) {
                s.setAveraging(K, false);
            }
            std::vector<float> bins;
            for (int f = 0; f < 4 * K; ++f) {   // let the boxcar fill
                s.process(noisyTone(), bins);
            }
            double sum = 0.0, sum2 = 0.0;
            constexpr int M = 60;
            for (int f = 0; f < M; ++f) {
                s.process(noisyTone(), bins);
                const double v = bins[static_cast<std::size_t>(floorBin)];
                sum += v;
                sum2 += v * v;
            }
            return std::sqrt(sum2 / M - (sum / M) * (sum / M));
        };
        const double raw = spread(false);
        const double avg = spread(true);
        check(avg < raw * 0.6,
              "boxcar averaging shrinks the noise-floor bin's frame-to-frame stddev");
    }

    // ---- peak detector: infinite max-hold (averaging factor <= 1) ----
    {
        constexpr int k0 = 12;
        const int pk = (k0 + half) % N;
        Hl2Spectrum s(N);
        s.setPeakHold(true);                       // frames stays 1 -> infinite hold
        std::vector<float> bins;
        s.process(tone(N, k0, 0.05f), bins);       // a quiet frame first
        const float low = bins[static_cast<std::size_t>(pk)];
        s.process(tone(N, k0, 0.5f), bins);        // then a loud one — peak jumps up
        const float high = bins[static_cast<std::size_t>(pk)];
        check(high > low + 10.0f, "max-hold rises to a louder frame");
        s.process(tone(N, k0, 0.05f), bins);       // quiet again — peak must NOT fall
        check(std::fabs(bins[static_cast<std::size_t>(pk)] - high) < 1e-3f,
              "max-hold holds the peak when the signal drops");
        check(bins[static_cast<std::size_t>(pk)] >= rawPeakDb(k0, 0.5f) - 0.05f,
              "the held value is the loud frame's single-frame level");
    }

    // ---- peak detector: sliding window (averaging factor N) ages a peak out ----
    {
        constexpr int K = 5;
        constexpr int k0 = 14;
        const int pk = (k0 + half) % N;
        Hl2Spectrum s(N);
        s.setAveraging(K, false);
        s.setPeakHold(true);                       // windowed max over K frames
        std::vector<float> bins;
        s.process(tone(N, k0, 0.5f), bins);        // one loud frame
        for (int f = 0; f < K - 1; ++f) {
            s.process(tone(N, k0, 0.05f), bins);   // K-1 quiet frames — loud still in window
        }
        check(bins[static_cast<std::size_t>(pk)] >= rawPeakDb(k0, 0.5f) - 0.05f,
              "windowed peak still shows the loud frame while it is in the window");
        s.process(tone(N, k0, 0.05f), bins);       // one more — the loud frame ages out
        check(bins[static_cast<std::size_t>(pk)] < rawPeakDb(k0, 0.5f) - 10.0f,
              "windowed peak drops once the loud frame leaves the window");
    }

    // ---- setPeakHold(false) returns the stage to sample / average ----
    {
        Hl2Spectrum s(N);
        s.setPeakHold(true);
        std::vector<float> bins;
        s.process(tone(N, 9, 0.5f), bins);
        s.process(tone(N, 9, 0.02f), bins);        // held high
        s.setPeakHold(false);                      // back to sample (frames == 1)
        s.process(tone(N, 9, 0.02f), bins);
        check(std::fabs(bins[static_cast<std::size_t>((9 + half) % N)]
                        - rawPeakDb(9, 0.02f)) < 0.05f,
              "clearing peak-hold returns the bare single-frame trace");
    }

    // ---- reset() drops the detector history (average and held peak) ----
    {
        constexpr int k0 = 13;
        const int pk = (k0 + half) % N;
        Hl2Spectrum s(N);
        s.setAveraging(8, false);
        std::vector<float> bins;
        for (int f = 0; f < 8; ++f) {
            s.process(tone(N, k0, 0.5f), bins);   // fill the boxcar at -6 dBFS
        }
        s.reset();
        s.process(tone(N, k0, 0.05f), bins);      // one low frame after the reset
        check(std::fabs(bins[static_cast<std::size_t>(pk)] - rawPeakDb(k0, 0.05f)) < 0.05f,
              "reset() clears the averaging history — the next frame is unaveraged");

        // and the held peak
        Hl2Spectrum p(N);
        p.setPeakHold(true);
        p.process(tone(N, k0, 0.5f), bins);       // hold a loud peak
        p.reset();
        p.process(tone(N, k0, 0.05f), bins);
        check(std::fabs(bins[static_cast<std::size_t>(pk)] - rawPeakDb(k0, 0.05f)) < 0.05f,
              "reset() clears the held peak — the next frame starts a fresh hold");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_spectrum_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
