#include "core/dsp/AudioMetrics.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace AetherSDR::AudioMetrics {
namespace {

constexpr double kPi = 3.14159265358979323846;

// In-place iterative radix-2 Cooley-Tukey. `a.size()` must be a power of two.
void fftRadix2(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::size_t floorPow2(std::size_t n)
{
    std::size_t p = 1;
    while ((p << 1) <= n) {
        p <<= 1;
    }
    return p;
}

// Hann-windowed magnitude spectrum (first n/2 bins) of the first
// floorPow2(frames) samples.
std::vector<double> magnitudeSpectrum(const float* mono, std::size_t frames)
{
    const std::size_t n = floorPow2(frames);
    std::vector<std::complex<double>> buf(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double denom = (n > 1) ? static_cast<double>(n - 1) : 1.0;
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / denom);
        buf[i] = std::complex<double>(static_cast<double>(mono[i]) * w, 0.0);
    }
    fftRadix2(buf);
    std::vector<double> mag(n / 2);
    for (std::size_t i = 0; i < n / 2; ++i) {
        mag[i] = std::abs(buf[i]);
    }
    return mag;
}

} // namespace

std::vector<float> toMono(const float* interleaved, std::size_t frames, int channels)
{
    if (channels <= 1) {
        return std::vector<float>(interleaved, interleaved + frames);
    }
    std::vector<float> mono(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        double acc = 0.0;
        for (int c = 0; c < channels; ++c) {
            acc += static_cast<double>(
                interleaved[f * static_cast<std::size_t>(channels)
                            + static_cast<std::size_t>(c)]);
        }
        mono[f] = static_cast<float>(acc / channels);
    }
    return mono;
}

std::vector<DominantTone> dominantFrequencies(
    const float* mono, std::size_t frames, int sampleRate, int maxTones)
{
    std::vector<DominantTone> out;
    if (frames < 4 || sampleRate <= 0 || maxTones <= 0) {
        return out;
    }
    const std::vector<double> mag = magnitudeSpectrum(mono, frames);
    const std::size_t n = mag.size() * 2;
    const double binHz = static_cast<double>(sampleRate) / static_cast<double>(n);

    // Local maxima, skipping DC and its immediate neighbour.
    std::vector<std::pair<double, std::size_t>> peaks;
    for (std::size_t i = 2; i + 1 < mag.size(); ++i) {
        if (mag[i] > mag[i - 1] && mag[i] >= mag[i + 1]) {
            peaks.emplace_back(mag[i], i);
        }
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });

    const double floorMag = peaks.empty() ? 0.0 : peaks.front().first * 0.05;
    for (const auto& [m, i] : peaks) {
        if (static_cast<int>(out.size()) >= maxTones) {
            break;
        }
        if (m < floorMag) {
            break;
        }
        // Parabolic interpolation for sub-bin accuracy.
        const double a = mag[i - 1];
        const double b = mag[i];
        const double c = mag[i + 1];
        const double denom = (a - 2.0 * b + c);
        const double delta = (denom != 0.0) ? 0.5 * (a - c) / denom : 0.0;
        out.push_back({(static_cast<double>(i) + delta) * binHz, m});
    }
    return out;
}

double peakAbs(const float* mono, std::size_t frames)
{
    double pk = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        pk = std::max(pk, std::fabs(static_cast<double>(mono[i])));
    }
    return pk;
}

double rmsDbfs(const float* mono, std::size_t frames)
{
    if (frames == 0) {
        return -160.0;
    }
    double acc = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double s = static_cast<double>(mono[i]);
        acc += s * s;
    }
    const double rms = std::sqrt(acc / static_cast<double>(frames));
    return (rms > 0.0) ? 20.0 * std::log10(rms) : -160.0;
}

double clippedFraction(const float* mono, std::size_t frames, float threshold)
{
    if (frames == 0) {
        return 0.0;
    }
    std::size_t hit = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        if (std::fabs(mono[i]) >= threshold) {
            ++hit;
        }
    }
    return static_cast<double>(hit) / static_cast<double>(frames);
}

double combSpacingHz(const float* mono, std::size_t frames, int sampleRate)
{
    if (frames < 64 || sampleRate <= 0) {
        return 0.0;
    }
    std::vector<double> mag = magnitudeSpectrum(mono, frames);
    const std::size_t bins = mag.size();
    const std::size_t n = bins * 2;
    const double binHz = static_cast<double>(sampleRate) / static_cast<double>(n);

    // A harmonic comb of spacing df makes the magnitude spectrum periodic with
    // period df/binHz bins, so its normalised autocorrelation peaks at that
    // lag. Subtract the mean first so a broadband slope does not dominate.
    double mean = 0.0;
    for (double m : mag) {
        mean += m;
    }
    mean /= static_cast<double>(bins);
    for (double& m : mag) {
        m -= mean;
    }

    double e0 = 0.0;
    for (double m : mag) {
        e0 += m * m;
    }
    if (e0 <= 0.0) {
        return 0.0;
    }

    const std::size_t minLag = std::max<std::size_t>(
        3, static_cast<std::size_t>(std::ceil(40.0 / binHz))); // spacing >= 40 Hz
    const std::size_t maxLag = bins / 2;
    if (maxLag <= minLag) {
        return 0.0;
    }

    double best = 0.0;
    std::size_t bestLag = 0;
    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        double acc = 0.0;
        for (std::size_t i = 0; i + lag < bins; ++i) {
            acc += mag[i] * mag[i + lag];
        }
        const double norm = acc / e0;
        if (norm > best) {
            best = norm;
            bestLag = lag;
        }
    }

    if (bestLag == 0 || best < 0.30) {
        return 0.0;
    }
    return static_cast<double>(bestLag) * binHz;
}

} // namespace AetherSDR::AudioMetrics
