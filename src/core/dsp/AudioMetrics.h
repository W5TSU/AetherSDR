#pragma once

#include <cstddef>
#include <vector>

namespace AetherSDR::AudioMetrics {

struct DominantTone {
    double hz{0.0};
    double magnitude{0.0}; // linear spectral magnitude at the peak bin
};

// Up to `maxTones` strongest spectral peaks, descending by magnitude.
// `mono` is `frames` float samples at `sampleRate` Hz. Analyses the first
// power-of-two run of samples with a Hann window; empty when the input is
// too short or the parameters are degenerate.
[[nodiscard]] std::vector<DominantTone> dominantFrequencies(
    const float* mono, std::size_t frames, int sampleRate, int maxTones);

// Largest absolute sample value, linear (0 for an empty buffer).
[[nodiscard]] double peakAbs(const float* mono, std::size_t frames);

// RMS level in dBFS relative to full scale 1.0. Returns -160.0 for silence
// and for an empty buffer.
[[nodiscard]] double rmsDbfs(const float* mono, std::size_t frames);

// Fraction of samples at or beyond `threshold` in magnitude (0.0 for an
// empty buffer).
[[nodiscard]] double clippedFraction(
    const float* mono, std::size_t frames, float threshold = 0.999f);

// Spacing, in Hz, of an evenly spaced harmonic comb, found as the first strong
// peak of the mean-removed magnitude spectrum's normalised autocorrelation
// (a comb of spacing df makes the spectrum periodic with that period).
// Returns 0.0 when no comb stands clear of the floor.
[[nodiscard]] double combSpacingHz(
    const float* mono, std::size_t frames, int sampleRate);

// Average interleaved channels down to mono. `channels <= 1` copies through.
[[nodiscard]] std::vector<float> toMono(
    const float* interleaved, std::size_t frames, int channels);

} // namespace AetherSDR::AudioMetrics
