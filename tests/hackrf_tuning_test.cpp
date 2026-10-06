// HackRF's panadapter/slice bookkeeping and zoom-to-sample-rate choice.
//
// The panadapter (hardware LO) and the slice (the DDC's tuned frequency) used
// to be one value, so dragging the spectrum retuned the slice and the wheel
// zoom had no implementation. Pinned here, without hardware:
//   - a drag moves the pan and leaves the slice, unless the slice would leave
//     the span, when it is pulled to the nearest edge;
//   - a tune inside the span moves only the slice; outside, the pan recentres;
//   - a span change keeps the slice inside;
//   - a zoom request steps to the next supported rate IN ITS DIRECTION, never
//     rounding back to the current one (which would make the wheel inert).
#include "core/backends/hackrf/HackRfTuning.h"

#include <cmath>
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
bool near(double a, double b) { return std::fabs(a - b) < 0.5; }
const std::vector<double> kRates = {2e6, 4e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
} // namespace

int main()
{
    const double span = 8e6;           // 8 MS/s: +-4 MHz, margin 0.45 * 8 = 3.6 MHz
    const PanSlice start{145.0e6, 145.0e6};

    // ---- drag ----
    {
        bool sliceMoved = true;
        const PanSlice r = dragPan(start, span, 146.0e6, sliceMoved);
        check(near(r.panHz, 146.0e6), "drag: the pan moves to the dragged centre");
        check(near(r.sliceHz, 145.0e6) && !sliceMoved, "drag: the slice stays where it was tuned");
    }
    {
        bool sliceMoved = false;
        const PanSlice r = dragPan(start, span, 150.0e6, sliceMoved);   // slice would be 5 MHz off
        check(near(r.sliceHz, 150.0e6 - 3.6e6) && sliceMoved,
              "drag: a slice that would leave the span is pulled to the near edge");
    }

    // ---- tune ----
    {
        bool panMoved = true;
        const PanSlice r = tuneSlice(start, span, 146.5e6, panMoved);
        check(near(r.panHz, 145.0e6) && !panMoved, "tune inside the span: the pan stays");
        check(near(r.sliceHz, 146.5e6), "tune inside the span: the slice moves");
    }
    {
        bool panMoved = false;
        const PanSlice r = tuneSlice(start, span, 149.0e6, panMoved);
        check(panMoved && near(r.panHz, 149.0e6) && near(r.sliceHz, 149.0e6),
              "tune outside the span: the pan recentres on the slice");
    }

    // ---- span change: ZOOMING NEVER RETUNES THE SLICE ----
    // Reported: "it does cause the frequency to change as I zoom". The slice was
    // pulled to the edge of the narrower view; the view must move instead.
    {
        bool panMoved = false;
        const PanSlice r = applySpan(PanSlice{145.0e6, 148.0e6}, 2e6, panMoved);   // margin 0.9 MHz
        check(near(r.sliceHz, 148.0e6), "zooming in never moves the slice");
        check(panMoved && near(r.panHz, 148.0e6), "...the view moves onto it instead");
    }
    {
        bool panMoved = true;
        const PanSlice r = applySpan(PanSlice{145.0e6, 146.0e6}, 20e6, panMoved);
        check(!panMoved && near(r.sliceHz, 146.0e6) && near(r.panHz, 145.0e6),
              "zooming out leaves an inside slice and the view alone");
    }

    // ---- the centre that rides along with a zoom (intent Range) ----
    {
        // The zoom anchor asks for a centre 5 MHz from the slice: follow it only
        // as far as keeps the slice in view, and never move the slice.
        const PanSlice r = rangePan(start, span, 150.0e6);
        check(near(r.sliceHz, 145.0e6), "a zoom's centre never moves the slice");
        check(near(r.panHz, 145.0e6 + 3.6e6), "...the centre stops where the slice is still in view");
        const PanSlice r2 = rangePan(start, span, 146.0e6);
        check(near(r2.panHz, 146.0e6) && near(r2.sliceHz, 145.0e6),
              "a zoom centre that keeps the slice in view is taken as asked");
    }

    // ---- zoom -> sample rate ----
    check(chooseSampleRate(kRates, 8e6, 6.4e6) == 4e6, "zoom in from 8 MHz by 0.8 steps down to 4 MHz");
    check(chooseSampleRate(kRates, 8e6, 9.0e6) == 10e6, "zoom out from 8 MHz by a little steps up to 10 MHz");
    check(chooseSampleRate(kRates, 10e6, 12.5e6) == 12.5e6, "an exact supported request is taken");
    check(chooseSampleRate(kRates, 2e6, 1.0e6) == 2e6, "below the minimum stays at 2 MHz");
    check(chooseSampleRate(kRates, 20e6, 40e6) == 20e6, "above the maximum stays at 20 MHz");
    check(chooseSampleRate(kRates, 8e6, 8e6) == 8e6, "the current span is kept");
    check(chooseSampleRate(kRates, 8e6, 2.5e6) == 2e6, "a big zoom in lands on the largest rate not above it");
    check(chooseSampleRate(kRates, 7.0e6, 7.0e6) == 8e6,
          "an unsupported current rate (a restored value) snaps to a supported one");

    // ---- narrow zoom: spans below 2 MHz decimate the 2 MS/s capture ----
    {
        const auto& spans = zoomSpansHz();
        check(spans.front() == 62'500.0 && spans.back() == 20e6, "zoom spans run 62.5 kHz .. 20 MHz");
        check(chooseSampleRate(spans, 2e6, 1.6e6) == 1e6, "zoom in from 2 MHz steps to 1 MHz");
        check(chooseSampleRate(spans, 125e3, 100e3) == 62'500.0, "zoom in to the narrowest 62.5 kHz");
        check(chooseSampleRate(spans, 62'500.0, 50e3) == 62'500.0, "below 62.5 kHz stays there");
        check(chooseSampleRate(spans, 1e6, 1.25e6) == 2e6, "zoom out from 1 MHz steps to 2 MHz");

        const ZoomPlan wide = planForSpan(8e6);
        check(wide.sampleRateHz == 8e6 && wide.decimation == 1, "8 MHz: rate 8 MS/s, no decimation");
        const ZoomPlan two = planForSpan(2e6);
        check(two.sampleRateHz == 2e6 && two.decimation == 1, "2 MHz: rate 2 MS/s, no decimation");
        const ZoomPlan narrow = planForSpan(250e3);
        check(narrow.sampleRateHz == 2e6 && narrow.decimation == 8, "250 kHz: 2 MS/s decimated by 8");
        const ZoomPlan narrowest = planForSpan(62'500.0);
        check(narrowest.sampleRateHz == 2e6 && narrowest.decimation == 32,
              "62.5 kHz: 2 MS/s decimated by 32");
    }

    std::printf("%s\n", g_failed == 0 ? "hackrf_tuning_test: OK" : "hackrf_tuning_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
