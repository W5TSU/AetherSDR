#include "core/backends/hackrf/HackRfDdc.h"

#include <QCoreApplication>
#include <QObject>
#include <QVector>

#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>

using namespace AetherSDR::hackrf;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

void expectTrue(const char* name, bool ok) { report(name, ok); }

void expectNear(const char* name, double got, double want, double eps)
{
    if (std::fabs(got - want) < eps) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %f, want %f (eps %f)\n", name, got, want, eps);
        ++g_failed;
    }
}

// A synthetic wideband capture: a station at ABSOLUTE frequency `toneHz`,
// received with the wideband tuner centered at `centerHz`, appears in the
// IQ stream as a complex exponential at BASEBAND frequency (toneHz -
// centerHz) — exactly what a real HackRfWorker::rxIqReady payload would
// carry for a real signal that far from center.
QVector<std::complex<float>> syntheticTone(double toneHz, double centerHz,
                                           double sampleRateHz, int count)
{
    QVector<std::complex<float>> out;
    out.reserve(count);
    const double basebandHz = toneHz - centerHz;
    const double stepRad = 2.0 * std::numbers::pi * basebandHz / sampleRateHz;
    for (int n = 0; n < count; ++n) {
        out.append(std::complex<float>(static_cast<float>(std::cos(stepRad * n)),
                                       static_cast<float>(std::sin(stepRad * n))));
    }
    return out;
}

// Records every decimatedIqReady() emission, concatenated.
struct Recorder : public QObject {
    Q_OBJECT
public:
    explicit Recorder(HackRfDdc& d)
    {
        connect(&d, &HackRfDdc::decimatedIqReady, this,
                [this](const QVector<std::complex<float>>& iq) { all.append(iq); },
                Qt::DirectConnection);
    }
    QVector<std::complex<float>> all;
};

// Average phase advance per sample, in radians, over consecutive output
// samples — angle(out[i] * conj(out[i-1])), averaged. Near zero means the
// signal landed at DC (correctly tuned); non-zero means it's still
// rotating (a residual/mistuned offset survived decimation).
double averagePhaseAdvance(const QVector<std::complex<float>>& iq)
{
    if (iq.size() < 2) return 0.0;
    double sum = 0.0;
    for (int i = 1; i < iq.size(); ++i) {
        const std::complex<float> prod = iq[i] * std::conj(iq[i - 1]);
        sum += std::atan2(static_cast<double>(prod.imag()), static_cast<double>(prod.real()));
    }
    return sum / (iq.size() - 1);
}

// ── Decimation ratio ─────────────────────────────────────────────────────

void testDecimationRatioMatchesRateRatio()
{
    HackRfDdc ddc;
    ddc.setInputSampleRateHz(8'000'000.0);
    ddc.setOutputSampleRateHz(48'000.0);
    ddc.setCenterFrequencyHz(100'000'000.0);
    ddc.setSliceFrequencyHz(100'000'000.0);  // DC tone -- decimation ratio is what's under test here
    Recorder rec(ddc);

    const int inputCount = 800'000;  // 0.1 s of wideband capture at 8 MSps
    ddc.process(syntheticTone(100'000'000.0, 100'000'000.0, 8'000'000.0, inputCount));

    const double expected = inputCount * (48'000.0 / 8'000'000.0);  // 4800
    // A few samples' slop is expected at the accumulator's edges; the ratio
    // is what matters, not sample-exact alignment.
    expectTrue("output count matches the input/output rate ratio within 1%",
              std::fabs(rec.all.size() - expected) < expected * 0.01);
}

void testDecimationRatioHoldsForANonIntegerRatio()
{
    // 8,000,000 / 48,000 = 166.6667 -- no clean integer divisor, exactly the
    // case RtlSdrDdc's fractional-resample technique exists for.
    HackRfDdc ddc;
    ddc.setInputSampleRateHz(10'000'000.0);
    ddc.setOutputSampleRateHz(48'000.0);
    ddc.setCenterFrequencyHz(100'000'000.0);
    ddc.setSliceFrequencyHz(100'000'000.0);
    Recorder rec(ddc);

    const int inputCount = 1'000'000;
    ddc.process(syntheticTone(100'000'000.0, 100'000'000.0, 10'000'000.0, inputCount));

    const double expected = inputCount * (48'000.0 / 10'000'000.0);  // 4800
    expectTrue("non-integer input/output ratio still tracks correctly",
              std::fabs(rec.all.size() - expected) < expected * 0.01);
}

// ── Tuning correctness ───────────────────────────────────────────────────

void testTunedExactlyToSliceLandsAtDc()
{
    HackRfDdc ddc;
    ddc.setInputSampleRateHz(8'000'000.0);
    ddc.setOutputSampleRateHz(48'000.0);
    ddc.setCenterFrequencyHz(100'000'000.0);
    // A station 500 kHz above the wideband tuner's center, tuned exactly.
    ddc.setSliceFrequencyHz(100'500'000.0);
    Recorder rec(ddc);

    ddc.process(syntheticTone(100'500'000.0, 100'000'000.0, 8'000'000.0, 400'000));

    expectTrue("produced output", rec.all.size() > 100);
    const double phaseAdvance = averagePhaseAdvance(rec.all);
    expectNear("a station tuned exactly to the slice frequency lands at DC (no rotation)",
              phaseAdvance, 0.0, 1e-3);
}

void testMistunedStationStillRotatesAtTheResidualOffset()
{
    HackRfDdc ddc;
    ddc.setInputSampleRateHz(8'000'000.0);
    ddc.setOutputSampleRateHz(48'000.0);
    ddc.setCenterFrequencyHz(100'000'000.0);
    // The station is at 100.500 MHz, but the slice is tuned 1 kHz off —
    // exactly the residual any real mistuning would leave.
    const double residualHz = 1'000.0;
    ddc.setSliceFrequencyHz(100'500'000.0 - residualHz);
    Recorder rec(ddc);

    ddc.process(syntheticTone(100'500'000.0, 100'000'000.0, 8'000'000.0, 400'000));

    const double phaseAdvance = averagePhaseAdvance(rec.all);
    const double expectedAdvance = 2.0 * std::numbers::pi * residualHz / 48'000.0;
    expectNear("a mistuned station rotates at exactly the residual offset, at the OUTPUT rate",
              phaseAdvance, expectedAdvance, expectedAdvance * 0.05);
}

// ── Multi-instance independence (the design doc's own "no precedent" claim) ─

void testTwoInstancesIsolateDifferentSlicesFromOneSharedFeed()
{
    // Two stations in the same wideband capture: 100.100 MHz and
    // 100.900 MHz, both received relative to one 100.500 MHz-centered
    // wideband tuner. Two independent HackRfDdc instances, each tuned to
    // one station, fed the IDENTICAL wideband samples (as HackRfBackend
    // will fan out one HackRfWorker::rxIqReady payload to N slices).
    const double centerHz = 100'500'000.0;
    const double inputRateHz = 8'000'000.0;

    QVector<std::complex<float>> wideband;
    wideband.reserve(400'000);
    {
        const auto a = syntheticTone(100'100'000.0, centerHz, inputRateHz, 400'000);
        const auto b = syntheticTone(100'900'000.0, centerHz, inputRateHz, 400'000);
        for (int i = 0; i < a.size(); ++i) wideband.append(a[i] + b[i]);
    }

    HackRfDdc ddcA;
    ddcA.setInputSampleRateHz(inputRateHz);
    ddcA.setOutputSampleRateHz(48'000.0);
    ddcA.setCenterFrequencyHz(centerHz);
    ddcA.setSliceFrequencyHz(100'100'000.0);
    Recorder recA(ddcA);

    HackRfDdc ddcB;
    ddcB.setInputSampleRateHz(inputRateHz);
    ddcB.setOutputSampleRateHz(48'000.0);
    ddcB.setCenterFrequencyHz(centerHz);
    ddcB.setSliceFrequencyHz(100'900'000.0);
    Recorder recB(ddcB);

    ddcA.process(wideband);
    ddcB.process(wideband);

    // Each instance's own target station lands near DC; the OTHER station
    // (800 kHz away) is far outside a boxcar decimator's passband and is
    // heavily attenuated, but what actually proves isolation is that each
    // instance's dominant residual is near zero, not near the other
    // station's 800 kHz separation.
    const double advanceA = averagePhaseAdvance(recA.all);
    const double advanceB = averagePhaseAdvance(recB.all);
    expectNear("instance A isolates its own station (100.100 MHz) at DC", advanceA, 0.0, 1e-2);
    expectNear("instance B isolates its own station (100.900 MHz) at DC", advanceB, 0.0, 1e-2);
}

// ── Parameter changes mid-stream take effect on the next process() call ──

void testRetuningChangesTheTargetOnTheNextCall()
{
    HackRfDdc ddc;
    ddc.setInputSampleRateHz(8'000'000.0);
    ddc.setOutputSampleRateHz(48'000.0);
    ddc.setCenterFrequencyHz(100'000'000.0);
    ddc.setSliceFrequencyHz(100'500'000.0);
    Recorder rec(ddc);

    ddc.process(syntheticTone(100'500'000.0, 100'000'000.0, 8'000'000.0, 200'000));
    const double beforeRetune = averagePhaseAdvance(rec.all);
    expectNear("tuned station is at DC before retuning", beforeRetune, 0.0, 1e-3);

    rec.all.clear();
    ddc.setSliceFrequencyHz(100'600'000.0);  // retune 100 kHz away from the station
    ddc.process(syntheticTone(100'500'000.0, 100'000'000.0, 8'000'000.0, 200'000));
    const double afterRetune = averagePhaseAdvance(rec.all);
    const double expectedAdvance = 2.0 * std::numbers::pi * (-100'000.0) / 48'000.0;
    // Wrapped to (-pi, pi] by atan2 -- a raw 100 kHz/48 kHz residual aliases,
    // so compare against the wrapped expectation rather than the raw value.
    auto wrap = [](double a) {
        while (a > std::numbers::pi) a -= 2.0 * std::numbers::pi;
        while (a < -std::numbers::pi) a += 2.0 * std::numbers::pi;
        return a;
    };
    expectNear("retuning away from the station reintroduces the expected residual rotation",
              afterRetune, wrap(expectedAdvance), 0.05);
}

void testEmptyInputProducesNoOutputAndDoesNotCrash()
{
    HackRfDdc ddc;
    Recorder rec(ddc);
    ddc.process({});
    expectTrue("empty input produces no output", rec.all.isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testDecimationRatioMatchesRateRatio();
    testDecimationRatioHoldsForANonIntegerRatio();
    testTunedExactlyToSliceLandsAtDc();
    testMistunedStationStillRotatesAtTheResidualOffset();
    testTwoInstancesIsolateDifferentSlicesFromOneSharedFeed();
    testRetuningChangesTheTargetOnTheNextCall();
    testEmptyInputProducesNoOutputAndDoesNotCrash();

    return g_failed == 0 ? 0 : 1;
}

#include "hackrf_ddc_test.moc"
