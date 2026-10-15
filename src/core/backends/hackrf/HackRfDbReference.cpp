#include "core/backends/hackrf/HackRfDbReference.h"

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"

#include <QJsonObject>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace AetherSDR::hackrf {

namespace {

// The Pro's measured LNA gain over code 0, by code / 8 (codes 0..40).
constexpr double kProLnaGainDb[] = {0.0, 0.0, 18.0, 18.0, 35.5, 35.5};

} // namespace

double HackRfDbReference::gainDb(HackRfBoard board, int lnaDb, int vgaDb, bool ampOn)
{
    const int lnaCode = std::clamp(lnaDb, 0, 40);
    double lna = lnaCode;
    if (board == HackRfBoard::Pro)
        lna = kProLnaGainDb[lnaCode / 8];
    const double amp = ampOn ? (board == HackRfBoard::Pro ? kProAmpGainDb : kOneAmpGainDb) : 0.0;
    return lna + std::clamp(vgaDb, 0, 62) + amp;
}

double HackRfDbReference::offsetDb(HackRfBoard board, int lnaDb, int vgaDb, bool ampOn,
                                   double operatorOffsetDb)
{
    return kFullScaleDbm - gainDb(board, lnaDb, vgaDb, ampOn) + clampOffset(operatorOffsetDb);
}

double HackRfDbReference::clampOffset(double db)
{
    if (!std::isfinite(db))
        return 0.0;
    return std::clamp(db, -kMaxOffsetDb, kMaxOffsetDb);
}

double HackRfDbReference::loadOffsetDb(const RadioSettingsScope& scope)
{
    if (!scope.isValid())
        return 0.0;
    const QJsonValue v = scope.feature(QLatin1String(kFeature)).value(QLatin1String(kFieldOffsetDb));
    return v.isDouble() ? clampOffset(v.toDouble()) : 0.0;
}

void HackRfDbReference::saveOffsetDb(const RadioSettingsScope& scope, double db)
{
    if (!scope.isValid())
        return;
    // Tenths of a dB: finer is noise, and it keeps the stored value readable.
    const double rounded = std::round(clampOffset(db) * 10.0) / 10.0;
    if (rounded == 0.0) {
        // Same rule as Hl2FreqCal::savePpb: a reset removes the document, so the
        // radio is genuinely uncalibrated again.
        if (!scope.removeFeature(QLatin1String(kFeature)))
            qWarning("HackRfDbReference: level calibration reset did not persist");
        AppSettings::instance().save();
        return;
    }
    QJsonObject doc = scope.featureExact(QLatin1String(kFeature));
    doc[QLatin1String(kFieldOffsetDb)] = rounded;
    if (!scope.setFeature(QLatin1String(kFeature), kSchemaVersion, doc))
        qWarning("HackRfDbReference: %.1f dB did not persist", rounded);
    AppSettings::instance().save();
}

} // namespace AetherSDR::hackrf
