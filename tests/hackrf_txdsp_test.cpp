// Unit test for HackRfTxDsp (#42) — the hand-rolled FM transmit modulator.
// No hardware needed: pure DSP math, verified the same way HackRfDdc's own
// tests are (phase-advance-per-sample), not just structural asserts.

#include "core/backends/hackrf/HackRfTxDsp.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

// Instantaneous frequency (Hz) between two consecutive complex samples, at
// the given sample rate — the same phase-difference technique
// RtlSdrDdc/HackRfDdc's own FM demod uses, run here to verify the modulator
// rather than to build a receiver.
double instantaneousFreqHz(const std::complex<float>& a, const std::complex<float>& b,
                           double sampleRateHz)
{
    const std::complex<float> prod = b * std::conj(a);
    const double phaseDiff = std::atan2(prod.imag(), prod.real());
    return phaseDiff * sampleRateHz / (2.0 * std::numbers::pi);
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Silence produces an unmodulated carrier ─────────────────────────
    {
        hackrf::HackRfTxDsp dsp;
        hackrf::HackRfTxDsp::Config cfg;
        cfg.audioSampleRateHz = 48'000;
        cfg.outputSampleRateHz = 192'000.0;   // integer ratio (4x) for simple assertions
        cfg.maxDeviationHz = 5'000.0;
        dsp.configure(cfg);

        QSignalSpy spy(&dsp, &hackrf::HackRfTxDsp::iqReady);
        const std::vector<float> silence(100, 0.0f);
        dsp.processAudioBlock(silence);
        check(spy.size() == 1, "silence produces exactly one iqReady emission");
        if (spy.size() == 1) {
            const auto iq = spy.at(0).at(0).value<QVector<std::complex<float>>>();
            check(iq.size() == 400, "192kHz/48kHz = 4x -> 100 audio samples produce 400 IQ samples");
            bool allZeroFreq = true;
            for (int i = 1; i < iq.size(); ++i) {
                const double freq = instantaneousFreqHz(iq[i - 1], iq[i], cfg.outputSampleRateHz);
                if (std::abs(freq) > 1.0) allZeroFreq = false;
            }
            check(allZeroFreq, "silence modulates to an unmodulated (0 Hz) carrier");
            for (const auto& s : iq) {
                check(std::abs(std::abs(s) - 1.0f) < 1e-5f, "every IQ sample has unit magnitude");
            }
        }
    }

    // ── Full positive/negative deviation ────────────────────────────────
    {
        hackrf::HackRfTxDsp dsp;
        hackrf::HackRfTxDsp::Config cfg;
        cfg.audioSampleRateHz = 48'000;
        cfg.outputSampleRateHz = 192'000.0;
        cfg.maxDeviationHz = 5'000.0;
        dsp.configure(cfg);

        QSignalSpy spy(&dsp, &hackrf::HackRfTxDsp::iqReady);
        const std::vector<float> maxPositive(200, 1.0f);
        dsp.processAudioBlock(maxPositive);
        check(spy.size() == 1, "constant audio produces one emission");
        if (spy.size() == 1) {
            const auto iq = spy.at(0).at(0).value<QVector<std::complex<float>>>();
            // Skip the first few samples (settling from the initial phase=0 start).
            bool allAtMaxDeviation = true;
            for (int i = 10; i < iq.size(); ++i) {
                const double freq = instantaneousFreqHz(iq[i - 1], iq[i], cfg.outputSampleRateHz);
                if (std::abs(freq - cfg.maxDeviationHz) > 1.0) allAtMaxDeviation = false;
            }
            check(allAtMaxDeviation, "audio at +1.0 deviates at exactly +maxDeviationHz");
        }

        dsp.reset();
        QSignalSpy spy2(&dsp, &hackrf::HackRfTxDsp::iqReady);
        const std::vector<float> maxNegative(200, -1.0f);
        dsp.processAudioBlock(maxNegative);
        check(spy2.size() == 1, "constant negative audio produces one emission");
        if (spy2.size() == 1) {
            const auto iq = spy2.at(0).at(0).value<QVector<std::complex<float>>>();
            bool allAtMinDeviation = true;
            for (int i = 10; i < iq.size(); ++i) {
                const double freq = instantaneousFreqHz(iq[i - 1], iq[i], cfg.outputSampleRateHz);
                if (std::abs(freq + cfg.maxDeviationHz) > 1.0) allAtMinDeviation = false;
            }
            check(allAtMinDeviation, "audio at -1.0 deviates at exactly -maxDeviationHz");
        }
    }

    // ── Audio outside [-1, 1] is clamped, not rejected ──────────────────
    {
        hackrf::HackRfTxDsp dsp;
        hackrf::HackRfTxDsp::Config cfg;
        cfg.audioSampleRateHz = 48'000;
        cfg.outputSampleRateHz = 192'000.0;
        cfg.maxDeviationHz = 5'000.0;
        dsp.configure(cfg);

        QSignalSpy spy(&dsp, &hackrf::HackRfTxDsp::iqReady);
        const std::vector<float> overdriven(200, 3.5f);   // way past full scale
        dsp.processAudioBlock(overdriven);
        check(spy.size() == 1, "overdriven audio still produces one emission");
        if (spy.size() == 1) {
            const auto iq = spy.at(0).at(0).value<QVector<std::complex<float>>>();
            bool clampedToMax = true;
            for (int i = 10; i < iq.size(); ++i) {
                const double freq = instantaneousFreqHz(iq[i - 1], iq[i], cfg.outputSampleRateHz);
                if (std::abs(freq - cfg.maxDeviationHz) > 1.0) clampedToMax = false;
            }
            check(clampedToMax, "overdriven audio clamps to +maxDeviationHz, not beyond");
        }
    }

    // ── Non-integer ratio (the real HackRF rate pair) still tracks the
    // long-run output count exactly, the same property hackrf_ddc_test
    // pins for the decimating direction. ────────────────────────────────
    {
        hackrf::HackRfTxDsp dsp;
        hackrf::HackRfTxDsp::Config cfg;
        cfg.audioSampleRateHz = 48'000;
        cfg.outputSampleRateHz = 8'000'000.0;   // 166.667:1, no clean integer ratio
        cfg.maxDeviationHz = 5'000.0;
        dsp.configure(cfg);

        QSignalSpy spy(&dsp, &hackrf::HackRfTxDsp::iqReady);
        const std::vector<float> tone(4800, 0.0f);   // 100 ms of silence at 48 kHz
        dsp.processAudioBlock(tone);
        qsizetype totalOut = 0;
        for (const auto& call : spy) {
            totalOut += call.at(0).value<QVector<std::complex<float>>>().size();
        }
        const double expected = 4800.0 * (cfg.outputSampleRateHz / cfg.audioSampleRateHz);
        check(std::abs(static_cast<double>(totalOut) - expected) / expected < 0.01,
              "non-integer input/output ratio still tracks correctly within 1%");
    }

    // ── Empty input produces no emission ────────────────────────────────
    {
        hackrf::HackRfTxDsp dsp;
        QSignalSpy spy(&dsp, &hackrf::HackRfTxDsp::iqReady);
        dsp.processAudioBlock({});
        check(spy.isEmpty(), "empty input produces no output");
    }

    if (g_failures == 0) {
        std::printf("hackrf_txdsp_test: OK\n");
    } else {
        std::fprintf(stderr, "hackrf_txdsp_test: %d failure(s)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
