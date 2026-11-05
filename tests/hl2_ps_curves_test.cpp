// WDSP's display data turned into what the PureSignal window draws. calcc reports
// each sample by its feedback level: `x` is the output, `ym` the ratio of drive to
// output, `yc` and `ys` the phase between them. None of that is a plot yet.

#include "core/backends/hl2/Hl2PsCurves.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}
static bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

int main()
{
    // Drive is output times the ratio, so a ratio of 1 is the straight line and a
    // compressing amplifier needs more drive for the same output.
    {
        Hl2PsDispView d;
        const std::vector<double> x{0.25, 0.5, 1.0};
        const std::vector<double> ym{1.0, 0.9, 1.0};
        const std::vector<double> yc{1.0, 1.0, 1.0};
        const std::vector<double> ys{0.0, 0.0, 0.0};
        d.x = x; d.ym = ym; d.yc = yc; d.ys = ys;
        const PureSignalCurves c = hl2PsCurves(d, 7);
        check(c.calibration == 7, "carries the calibration count");
        check(c.amplitude.size() == 3, "one point per sample");
        check(near(c.amplitude[1].x(), 0.45) && near(c.amplitude[1].y(), 0.5),
              "x is drive, y is output");
        check(near(c.amplitude[2].x(), 1.0) && near(c.amplitude[2].y(), 1.0), "top of the curve");
    }

    // Phase reads from the same reference as the correction: WDSP moves the
    // correction so its top end sits at 0.
    {
        Hl2PsDispView d;
        const std::vector<double> x{0.5};
        const std::vector<double> ym{1.0};
        const std::vector<double> yc{std::cos(M_PI / 6.0)};
        const std::vector<double> ys{std::sin(M_PI / 6.0)};
        d.x = x; d.ym = ym; d.yc = yc; d.ys = ys;
        d.phaseReferenceDeg = 10.0;
        const PureSignalCurves c = hl2PsCurves(d, 1);
        check(c.phaseDeg.size() == 1, "phase point");
        check(near(c.phaseDeg[0].x(), 0.5) && near(c.phaseDeg[0].y(), 20.0, 1e-4),
              "phase is against the correction's reference");
    }

    // Round the circle the short way, or a curve near 180 degrees jumps the plot.
    {
        Hl2PsDispView d;
        const std::vector<double> x{0.5};
        const std::vector<double> ym{1.0};
        const std::vector<double> yc{std::cos(170.0 * M_PI / 180.0)};
        const std::vector<double> ys{std::sin(170.0 * M_PI / 180.0)};
        d.x = x; d.ym = ym; d.yc = yc; d.ys = ys;
        d.phaseReferenceDeg = -30.0;
        const PureSignalCurves c = hl2PsCurves(d, 1);
        check(near(c.phaseDeg[0].y(), -160.0, 1e-4), "wrapped to +/-180");
    }

    // A slot with no sample in it is not a point at the origin.
    {
        Hl2PsDispView d;
        const std::vector<double> x{0.0, 0.5};
        const std::vector<double> ym{0.0, 1.0};
        const std::vector<double> yc{0.0, 1.0};
        const std::vector<double> ys{0.0, 0.0};
        d.x = x; d.ym = ym; d.yc = yc; d.ys = ys;
        const PureSignalCurves c = hl2PsCurves(d, 1);
        check(c.amplitude.size() == 1 && c.phaseDeg.size() == 1, "empty slots dropped");
    }

    // A full collection is thinned for the window, evenly across the levels.
    {
        const std::size_t n = 4096;
        std::vector<double> x(n), ym(n, 1.0), yc(n, 1.0), ys(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            x[i] = static_cast<double>(i + 1) / static_cast<double>(n);
        Hl2PsDispView d;
        d.x = x; d.ym = ym; d.yc = yc; d.ys = ys;
        const PureSignalCurves c = hl2PsCurves(d, 1);
        check(c.amplitude.size() <= PureSignalCurves::kMaxSamplePoints, "thinned");
        check(c.amplitude.size() >= PureSignalCurves::kMaxSamplePoints / 2, "not over-thinned");
        check(c.amplitude.back().y() > 0.99, "reaches the top bucket");
    }

    // The correction curves pass straight through.
    {
        Hl2PsDispView d;
        const std::vector<double> cx{0.0, 1.0};
        const std::vector<double> gain{1.1, 1.2};
        const std::vector<double> ax{0.0, 1.0};
        const std::vector<double> deg{3.0, 0.0};
        d.correctionX = cx; d.correctionGain = gain;
        d.correctionPhaseX = ax; d.correctionPhaseDeg = deg;
        const PureSignalCurves c = hl2PsCurves(d, 1);
        check(c.correctionGain.size() == 2 && near(c.correctionGain[1].y(), 1.2), "gain");
        check(c.correctionPhaseDeg.size() == 2 && near(c.correctionPhaseDeg[0].y(), 3.0), "phase");
    }

    check(!PureSignalCurves{}.valid(), "nothing calibrated is not valid");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("hl2_ps_curves_test: pass");
    return 0;
}
