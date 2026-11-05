#pragma once

#include <QPointF>
#include <QVector>

namespace AetherSDR {

// What a backend's PureSignal measured and is applying, for the PureSignal window
// and `ps curves`. Levels are 0..1 of the top of the curve.
struct PureSignalCurves {
    // A full collection is 4096 samples; the window has no use for more than this.
    static constexpr int kMaxSamplePoints = 1024;

    int calibration = 0;                 // successful calibrations when this was read
    QVector<QPointF> amplitude;          // drive (x), output (y); straight is linear
    QVector<QPointF> phaseDeg;           // output (x), phase shift (y)
    QVector<QPointF> correctionGain;     // output (x), the gain applied (y)
    QVector<QPointF> correctionPhaseDeg; // output (x), the phase applied (y)

    [[nodiscard]] bool valid() const noexcept { return calibration > 0; }
};

} // namespace AetherSDR
