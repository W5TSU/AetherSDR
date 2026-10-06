#include "HackRfTuning.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::hackrf {

namespace {
double clampSlice(double panHz, double spanHz, double sliceHz)
{
    const double margin = sliceMarginHz(spanHz);
    return std::clamp(sliceHz, panHz - margin, panHz + margin);
}
} // namespace

PanSlice dragPan(PanSlice current, double spanHz, double newPanHz, bool& sliceMoved)
{
    PanSlice r{newPanHz, clampSlice(newPanHz, spanHz, current.sliceHz)};
    sliceMoved = r.sliceHz != current.sliceHz;
    return r;
}

PanSlice tuneSlice(PanSlice current, double spanHz, double newSliceHz, bool& panMoved)
{
    if (std::fabs(newSliceHz - current.panHz) <= sliceMarginHz(spanHz)) {
        panMoved = false;
        return PanSlice{current.panHz, newSliceHz};
    }
    panMoved = true;
    return PanSlice{newSliceHz, newSliceHz};
}

PanSlice applySpan(PanSlice current, double newSpanHz, bool& sliceMoved)
{
    PanSlice r{current.panHz, clampSlice(current.panHz, newSpanHz, current.sliceHz)};
    sliceMoved = r.sliceHz != current.sliceHz;
    return r;
}

double chooseSampleRate(const std::vector<double>& rates, double currentHz, double requestedHz)
{
    if (rates.empty())
        return currentHz;
    // A current rate outside the list (restored from settings) first snaps to
    // the nearest supported one; that is also the answer to "keep the span".
    double current = rates.front();
    for (double r : rates) {
        if (std::fabs(r - currentHz) < std::fabs(current - currentHz))
            current = r;
    }
    // "Keep the current span" (the request IS the current value, possibly an
    // unsupported restored one): the snapped rate, not a step away from it.
    if (std::fabs(requestedHz - currentHz) < 1.0)
        return current;
    if (requestedHz < current) {
        // Narrower: the largest supported rate not above the request; if the
        // request is below every rate, the minimum.
        double pick = rates.front();
        for (double r : rates) {
            if (r <= requestedHz)
                pick = r;
        }
        return std::min(pick, current);
    }
    if (requestedHz > current) {
        // Wider: the smallest supported rate not below the request; if the
        // request is above every rate, the maximum.
        for (double r : rates) {
            if (r >= requestedHz)
                return r;
        }
        return rates.back();
    }
    return current;
}

const std::vector<double>& zoomSpansHz()
{
    static const std::vector<double> spans = {
        62'500.0, 125'000.0, 250'000.0, 500'000.0, 1'000'000.0,             // decimated
        2'000'000.0, 4'000'000.0, 8'000'000.0, 10'000'000.0, 12'500'000.0,   // hardware
        16'000'000.0, 20'000'000.0};
    return spans;
}

ZoomPlan planForSpan(double spanHz)
{
    constexpr double kMinRate = 2'000'000.0;
    if (spanHz >= kMinRate)
        return ZoomPlan{spanHz, 1};
    int d = 1;
    while (d < 32 && kMinRate / (d * 2) >= spanHz - 1.0)
        d *= 2;
    return ZoomPlan{kMinRate, d};
}

} // namespace AetherSDR::hackrf
