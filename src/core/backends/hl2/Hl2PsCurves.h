#pragma once

#include "core/backends/PureSignalCurves.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

namespace AetherSDR::hl2 {

// WDSP's GetPSDisp, as it comes. Each sample is filed by its feedback level: `x` is
// the output, `ym` the ratio of drive to output, `yc` and `ys` the cosine and sine of
// the phase between them.
struct Hl2PsDispView {
    std::span<const double> x, ym, yc, ys;
    std::span<const double> correctionX, correctionGain;
    std::span<const double> correctionPhaseX, correctionPhaseDeg;
    double phaseReferenceDeg = 0.0;  // WDSP subtracts this from the correction's phase
};

inline PureSignalCurves hl2PsCurves(const Hl2PsDispView& d, int calibration)
{
    PureSignalCurves c;
    c.calibration = calibration;

    const std::size_t n = std::min({d.x.size(), d.ym.size(), d.yc.size(), d.ys.size()});
    const std::size_t stride =
        std::max<std::size_t>(1, (n + PureSignalCurves::kMaxSamplePoints - 1)
                                     / PureSignalCurves::kMaxSamplePoints);
    c.amplitude.reserve(static_cast<qsizetype>(n / stride + 1));
    c.phaseDeg.reserve(static_cast<qsizetype>(n / stride + 1));
    constexpr double kDeg = 180.0 / std::numbers::pi;
    for (std::size_t i = 0; i < n; i += stride) {
        if (d.x[i] <= 0.0)
            continue;  // a slot with no sample in it
        c.amplitude.append({d.x[i] * d.ym[i], d.x[i]});
        const double deg = std::atan2(d.ys[i], d.yc[i]) * kDeg - d.phaseReferenceDeg;
        c.phaseDeg.append({d.x[i], std::remainder(deg, 360.0)});
    }

    const auto line = [](std::span<const double> xs, std::span<const double> ys) {
        QVector<QPointF> out;
        const std::size_t count = std::min(xs.size(), ys.size());
        out.reserve(static_cast<qsizetype>(count));
        for (std::size_t i = 0; i < count; ++i)
            out.append({xs[i], ys[i]});
        return out;
    };
    c.correctionGain = line(d.correctionX, d.correctionGain);
    c.correctionPhaseDeg = line(d.correctionPhaseX, d.correctionPhaseDeg);
    return c;
}

} // namespace AetherSDR::hl2
