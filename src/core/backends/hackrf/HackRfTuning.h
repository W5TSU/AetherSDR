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

// The span changed (zoom): keep the slice inside the new one.
PanSlice applySpan(PanSlice current, double newSpanHz, bool& sliceMoved);

// The sample rate (= span) for a zoom request. Steps to the next supported rate
// IN THE DIRECTION of the request: a wheel step asks for a span between two
// supported rates, and plain nearest-rounding would land back on the current
// one, leaving the wheel inert. A current rate that is not in the list (a
// restored value) snaps to the nearest supported one. `rates` ascending.
double chooseSampleRate(const std::vector<double>& rates, double currentHz, double requestedHz);

} // namespace AetherSDR::hackrf
