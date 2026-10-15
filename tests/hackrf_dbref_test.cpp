// HackRfDbReference: the measured gain table, the dBFS -> dBm offset, and the
// operator's per-device correction in the settings store.

#include "core/backends/hackrf/HackRfDbReference.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::hackrf;

namespace {
int g_failures = 0;
void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_failures;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hackrf-dbref-test"));
    if (!profile.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    using R = HackRfDbReference;

    // The One takes its documented 8 dB LNA steps and dB-for-dB VGA.
    check(near(R::gainDb(HackRfBoard::One, 16, 20, false), 36.0), "One: LNA 16 + VGA 20 = 36 dB");
    check(near(R::gainDb(HackRfBoard::One, 0, 0, true), 11.0), "One: the amp adds 11 dB");

    // The Pro's LNA has three measured steps: codes pair up.
    check(near(R::gainDb(HackRfBoard::Pro, 0, 0, false), R::gainDb(HackRfBoard::Pro, 8, 0, false)),
          "Pro: LNA 0 and 8 are the same gain");
    check(near(R::gainDb(HackRfBoard::Pro, 16, 0, false), 18.0)
              && near(R::gainDb(HackRfBoard::Pro, 24, 0, false), 18.0),
          "Pro: LNA 16 and 24 measure +18 dB");
    check(near(R::gainDb(HackRfBoard::Pro, 40, 0, false), 35.5), "Pro: LNA 32/40 measure +35.5 dB");
    check(near(R::gainDb(HackRfBoard::Pro, 0, 0, true), 12.3), "Pro: the amp measures +12.3 dB");

    // A gain change moves the offset by exactly the gain, so a signal's dBm
    // reading does not move.
    const double a = R::offsetDb(HackRfBoard::Pro, 16, 20, false, 0.0);
    const double b = R::offsetDb(HackRfBoard::Pro, 16, 30, false, 0.0);
    check(near(a - b, 10.0), "10 dB more VGA lowers the offset by 10 dB");
    check(near(a, R::kFullScaleDbm - 38.0), "offset = full-scale reference - gain");
    check(near(R::offsetDb(HackRfBoard::Pro, 16, 20, false, -2.5), a - 2.5),
          "the operator correction adds straight on");
    check(near(R::offsetDb(HackRfBoard::Pro, 16, 20, false, 1e9), a + R::kMaxOffsetDb),
          "the operator correction is clamped");
    check(near(R::clampOffset(std::nan("")), 0.0), "a NaN correction is 0");

    // The correction is per device and survives a reload; 0 removes it.
    const RadioSettingsScope unitA(QStringLiteral("hackrf"), QStringLiteral("serialA"));
    const RadioSettingsScope unitB(QStringLiteral("hackrf"), QStringLiteral("serialB"));
    check(near(R::loadOffsetDb(unitA), 0.0), "an uncalibrated device reads 0 dB");
    R::saveOffsetDb(unitA, -3.26);
    check(near(R::loadOffsetDb(unitA), -3.3), "a saved correction loads, in tenths of a dB");
    check(near(R::loadOffsetDb(unitB), 0.0), "another device does not inherit it");
    R::saveOffsetDb(unitA, 0.0);
    check(unitA.featureExact(QLatin1String(R::kFeature)).isEmpty(), "a reset removes the document");

    std::printf("hackrf_dbref_test: %s\n", g_failures ? "FAILED" : "OK");
    return g_failures ? 1 : 0;
}
