#pragma once

#include <vector>

namespace AetherSDR::hackrf {

// HackRF's two frequencies, kept apart: the PANADAPTER centre is where the
// hardware LO sits (the middle of the captured span) and the SLICE is what the
// DDC is tuned to inside that span. They used to be one value, so dragging the
// spectrum retuned the slice along with it. Pure; unit-tested
// (hackrf_tuning_test).
struct PanSlice {
    double panHz = 0.0;
    double sliceHz = 0.0;
};

// How far from the pan centre the slice may sit: 45% of the span, leaving the
// passband clear of the band edges where HackRF's baseband filter rolls off.
// RtlSdrBackend uses the same fraction.
inline double sliceMarginHz(double spanHz) { return 0.45 * spanHz; }

// The operator dragged the spectrum to `newPanHz`: the view moves, the slice
// stays, unless that would leave it outside the span, when it is pulled to the
// nearest allowed edge so there is still audio.
PanSlice dragPan(PanSlice current, double spanHz, double newPanHz, bool& sliceMoved);

// The operator tuned the slice to `newSliceHz` (click, VFO, band change): inside
// the span only the slice moves (no hardware retune, no waterfall jump);
// outside it the pan recentres on the slice.
PanSlice tuneSlice(PanSlice current, double spanHz, double newSliceHz, bool& panMoved);

// The span changed (zoom). ZOOMING NEVER RETUNES THE SLICE: if it would not fit
// the new span, the VIEW recentres on it. (It used to pull the slice to the
// edge, so zooming in changed the operator's frequency.)
PanSlice applySpan(PanSlice current, double newSpanHz, bool& panMoved);

// The centre that rides along with a zoom (PanCenterIntent::Range): the zoom's
// anchor, not a retune. Taken as asked as far as the slice stays in view, and
// stopped there; the slice never moves.
PanSlice rangePan(PanSlice current, double spanHz, double newPanHz);

// The sample rate (= span) for a zoom request. Steps to the next supported rate
// IN THE DIRECTION of the request: a wheel step asks for a span between two
// supported rates, and plain nearest-rounding would land back on the current
// one, leaving the wheel inert. A current rate that is not in the list (a
// restored value) snaps to the nearest supported one. `rates` ascending.
double chooseSampleRate(const std::vector<double>& rates, double currentHz, double requestedHz);

// The displayed spans the wheel zoom walks: 62.5 kHz .. 1 MHz are the 2 MS/s
// capture decimated for the spectrum (HackRfZoomDecimator), 2 .. 20 MHz are
// hardware sample rates. Ascending.
const std::vector<double>& zoomSpansHz();

// How to show a span: the hardware sample rate, and the spectrum's decimation
// of it (1 for the hardware spans).
struct ZoomPlan {
    double sampleRateHz = 8'000'000.0;
    int decimation = 1;
};
ZoomPlan planForSpan(double spanHz);

} // namespace AetherSDR::hackrf
