// Pure unit tests for AudioMetrics -- synthetic signals with closed-form
// answers. No Qt, no aethercore: the analysis helpers depend only on the
// standard library, and this target stays buildable when the app does not.

#include "core/dsp/AudioMetrics.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR::AudioMetrics;

namespace {

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

std::vector<float> tone(double hz, int sampleRate, int frames, float amp)
{
    std::vector<float> v(static_cast<std::size_t>(frames));
    for (int n = 0; n < frames; ++n) {
        v[static_cast<std::size_t>(n)] = amp
            * static_cast<float>(std::sin(2.0 * kPi * hz * n / sampleRate));
    }
    return v;
}

} // namespace

int main()
{
    constexpr int kRate = 24000;
    constexpr int kFrames = 24000; // 1 s

    // 1. Dominant frequency of a clean 1 kHz tone is 1 kHz (within a bin).
    {
        auto s = tone(1000.0, kRate, kFrames, 0.5f);
        auto d = dominantFrequencies(s.data(), s.size(), kRate, 2);
        check(!d.empty() && std::fabs(d[0].hz - 1000.0) <= 2.0,
              "dominantFrequencies finds 1 kHz");
    }

    // 2. Two tones: 700 Hz (loud) then 1900 Hz (quieter), in that order.
    {
        auto a = tone(700.0, kRate, kFrames, 0.5f);
        auto b = tone(1900.0, kRate, kFrames, 0.2f);
        for (std::size_t i = 0; i < a.size(); ++i) {
            a[i] += b[i];
        }
        auto d = dominantFrequencies(a.data(), a.size(), kRate, 2);
        check(d.size() == 2 && std::fabs(d[0].hz - 700.0) <= 2.0
                  && std::fabs(d[1].hz - 1900.0) <= 2.0,
              "dominantFrequencies orders two tones by magnitude");
    }

    // 3. rmsDbfs of a 0.5-amplitude sine is 20*log10(0.5/sqrt(2)) ~= -9.03 dBFS.
    {
        auto s = tone(1000.0, kRate, kFrames, 0.5f);
        double db = rmsDbfs(s.data(), s.size());
        check(std::fabs(db - (-9.03)) < 0.2, "rmsDbfs of half-scale sine ~= -9.03");
    }

    // 4. clippedFraction detects hard clipping and reports 0 for a clean signal.
    {
        auto s = tone(300.0, kRate, kFrames, 2.0f);
        for (auto& x : s) {
            x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
        }
        // |2*sin| >= 1 for 2/3 of the cycle, so ~0.667 of samples rail.
        double frac = clippedFraction(s.data(), s.size());
        check(frac > 0.55 && frac < 0.75, "clippedFraction detects hard clipping");
    }
    {
        auto clean = tone(300.0, kRate, kFrames, 0.5f);
        check(clippedFraction(clean.data(), clean.size()) == 0.0,
              "clippedFraction is 0 for a clean signal");
    }

    // 5. peakAbs of a 0.5-amplitude sine is ~0.5.
    {
        auto s = tone(1000.0, kRate, kFrames, 0.5f);
        check(std::fabs(peakAbs(s.data(), s.size()) - 0.5) < 0.01,
              "peakAbs of half-scale sine ~= 0.5");
    }

    // 6. combSpacingHz finds a 100 Hz harmonic stack, and 0 for a single tone.
    {
        std::vector<float> s(static_cast<std::size_t>(kFrames), 0.0f);
        for (int k = 1; k <= 20; ++k) {
            auto h = tone(100.0 * k, kRate, kFrames, 0.05f);
            for (std::size_t i = 0; i < s.size(); ++i) {
                s[i] += h[i];
            }
        }
        double spacing = combSpacingHz(s.data(), s.size(), kRate);
        check(std::fabs(spacing - 100.0) <= 5.0, "combSpacingHz finds a 100 Hz comb");
    }
    {
        auto s = tone(1234.0, kRate, kFrames, 0.5f);
        check(combSpacingHz(s.data(), s.size(), kRate) == 0.0,
              "combSpacingHz is 0 for a single tone");
    }

    // 7. toMono averages interleaved stereo.
    {
        std::vector<float> stereo{1.0f, 0.0f, 0.5f, -0.5f, -1.0f, 1.0f};
        auto m = toMono(stereo.data(), 3, 2);
        check(m.size() == 3 && std::fabs(m[0] - 0.5f) < 1e-6f
                  && std::fabs(m[1] - 0.0f) < 1e-6f
                  && std::fabs(m[2] - 0.0f) < 1e-6f,
              "toMono averages L/R");
    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
