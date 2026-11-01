#include "core/backends/hl2/Hl2MiscOptionsSettings.h"

#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QDebug>
#include <QJsonObject>

namespace AetherSDR {

namespace {
constexpr const char* kFieldAdcDither = "adcDither";
constexpr const char* kFieldPureSignalGain = "pureSignalGainDbByBand";
constexpr const char* kFieldAdcRandom = "adcRandom";
constexpr const char* kFieldResetOnDisconnect = "resetOnDisconnect";
constexpr const char* kFieldTxLatency = "txLatency";
constexpr const char* kFieldPttHang = "pttHang";
constexpr const char* kFieldSwapAudioChannels = "swapAudioChannels";

// Reference implementation's own initial values (Thetis
// ChannelMaster/netInterface.c, create_rnet()).
constexpr int kDefaultTxLatency = 20;
constexpr int kDefaultPttHang = 12;

// kTxLatencyMax/kPttHangMax are re-declared on Hl2MiscOptionsSettings itself
// (see its own comment) so RadioSetupDialog.cpp doesn't need to import
// MetisProtocol.h just for two range limits. Asserted equal here, in the one
// file allowed to know both, so they can never silently drift.
static_assert(Hl2MiscOptionsSettings::kTxLatencyMax == hl2::kTxLatencyMax);
static_assert(Hl2MiscOptionsSettings::kPttHangMax == hl2::kPttHangMax);

// Read-modify-write against the EXACT row (never the family-wide fallback) —
// a writer must judge the row it is about to replace, not a composed default
// (AGENTS.md "Radio-Scoped Feature Documents").
void writeField(const RadioSettingsScope& scope, const char* field, const QJsonValue& value)
{
    QJsonObject doc = scope.featureExact(Hl2MiscOptionsSettings::kFeature);
    doc[QLatin1String(field)] = value;
    if (!scope.setFeature(Hl2MiscOptionsSettings::kFeature,
                          Hl2MiscOptionsSettings::kSchemaVersion, doc)) {
        qWarning() << "Hl2MiscOptionsSettings: write did not persist —"
                   << "no radio identity yet, or the store refused it";
    }
}
}  // namespace

std::optional<int> Hl2MiscOptionsSettings::pureSignalGainDb(const RadioSettingsScope& scope,
                                                            const QString& band)
{
    const QJsonValue v = scope.feature(kFeature)
                             .value(QLatin1String(kFieldPureSignalGain)).toObject().value(band);
    return v.isDouble() ? std::optional<int>(v.toInt()) : std::nullopt;
}

void Hl2MiscOptionsSettings::setPureSignalGainDb(const RadioSettingsScope& scope,
                                                 const QString& band, int db)
{
    QJsonObject gains = scope.featureExact(kFeature)
                            .value(QLatin1String(kFieldPureSignalGain)).toObject();
    gains[band] = db;
    writeField(scope, kFieldPureSignalGain, gains);
}

std::optional<Hl2MiscOptionsSettings::PureSignalCurve> Hl2MiscOptionsSettings::pureSignalCurve(
    const RadioSettingsScope& scope, const QString& band)
{
    const QJsonValue v = scope.feature(kFeature)
                             .value(QLatin1String("pureSignalCurveByBand")).toObject().value(band);
    if (!v.isObject())
        return std::nullopt;
    const QJsonObject o = v.toObject();
    if (!o.value(QLatin1String("gapDb")).isDouble() || !o.value(QLatin1String("drive")).isDouble())
        return std::nullopt;
    return PureSignalCurve{o.value(QLatin1String("gapDb")).toDouble(),
                           o.value(QLatin1String("drive")).toInt()};
}

void Hl2MiscOptionsSettings::setPureSignalCurve(const RadioSettingsScope& scope, const QString& band,
                                                double gapDb, int drivePercent)
{
    QJsonObject curves = scope.featureExact(kFeature)
                             .value(QLatin1String("pureSignalCurveByBand")).toObject();
    curves[band] = QJsonObject{{QLatin1String("gapDb"), gapDb},
                               {QLatin1String("drive"), drivePercent}};
    writeField(scope, "pureSignalCurveByBand", curves);
}

bool Hl2MiscOptionsSettings::adcDither(const RadioSettingsScope& scope)
{
    return scope.feature(kFeature).value(QLatin1String(kFieldAdcDither)).toBool(false);
}

void Hl2MiscOptionsSettings::setAdcDither(const RadioSettingsScope& scope, bool enabled)
{
    writeField(scope, kFieldAdcDither, enabled);
}

bool Hl2MiscOptionsSettings::adcRandom(const RadioSettingsScope& scope)
{
    return scope.feature(kFeature).value(QLatin1String(kFieldAdcRandom)).toBool(false);
}

void Hl2MiscOptionsSettings::setAdcRandom(const RadioSettingsScope& scope, bool enabled)
{
    writeField(scope, kFieldAdcRandom, enabled);
}

bool Hl2MiscOptionsSettings::resetOnDisconnect(const RadioSettingsScope& scope)
{
    return scope.feature(kFeature).value(QLatin1String(kFieldResetOnDisconnect)).toBool(false);
}

void Hl2MiscOptionsSettings::setResetOnDisconnect(const RadioSettingsScope& scope, bool enabled)
{
    writeField(scope, kFieldResetOnDisconnect, enabled);
}

int Hl2MiscOptionsSettings::txLatency(const RadioSettingsScope& scope)
{
    // Validated against the wire field's own range rather than trusted, so a
    // hand-edited or truncated document cannot command a nonsense register
    // value (Principle VII).
    const int v = scope.feature(kFeature).value(QLatin1String(kFieldTxLatency)).toInt(kDefaultTxLatency);
    return (v >= 0 && v <= hl2::kTxLatencyMax) ? v : kDefaultTxLatency;
}

void Hl2MiscOptionsSettings::setTxLatency(const RadioSettingsScope& scope, int value)
{
    if (value < 0 || value > hl2::kTxLatencyMax)
        return;
    writeField(scope, kFieldTxLatency, value);
}

int Hl2MiscOptionsSettings::pttHang(const RadioSettingsScope& scope)
{
    const int v = scope.feature(kFeature).value(QLatin1String(kFieldPttHang)).toInt(kDefaultPttHang);
    return (v >= 0 && v <= hl2::kPttHangMax) ? v : kDefaultPttHang;
}

void Hl2MiscOptionsSettings::setPttHang(const RadioSettingsScope& scope, int value)
{
    if (value < 0 || value > hl2::kPttHangMax)
        return;
    writeField(scope, kFieldPttHang, value);
}

bool Hl2MiscOptionsSettings::swapAudioChannels(const RadioSettingsScope& scope)
{
    return scope.feature(kFeature).value(QLatin1String(kFieldSwapAudioChannels)).toBool(false);
}

void Hl2MiscOptionsSettings::setSwapAudioChannels(const RadioSettingsScope& scope, bool enabled)
{
    writeField(scope, kFieldSwapAudioChannels, enabled);
}

}  // namespace AetherSDR
