#include "core/DaxSettings.h"

#include "core/AppSettings.h"
#include "core/SettingsJsonUtil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <array>

namespace AetherSDR {

namespace {

const QString kRootKey = QStringLiteral("DaxServer");

constexpr int kDefaultRateHz = 48000;
constexpr std::array<int, 4> kLegalRatesHz = {24000, 48000, 96000, 192000};

// The stored value of DaxServer as a parsed object, or an empty object when the
// key is absent or unparseable.
QJsonObject storedObject()
{
    const QString raw = AppSettings::instance().value(kRootKey, QString{}).toString();
    if (raw.isEmpty()) {
        return {};
    }
    return QJsonDocument::fromJson(raw.toUtf8()).object();
}

} // namespace

int DaxSettings::snapRate(int rawHz)
{
    int best = kLegalRatesHz.front();
    int bestDelta = std::abs(rawHz - best);
    for (int legal : kLegalRatesHz) {
        const int delta = std::abs(rawHz - legal);
        if (delta < bestDelta) {
            best = legal;
            bestDelta = delta;
        }
    }
    return best;
}

QJsonObject DaxSettings::readObj()
{
    const QJsonObject stored = storedObject();
    if (!stored.isEmpty()) {
        return stored;
    }
    return buildFromLegacy();
}

void DaxSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

QJsonObject DaxSettings::buildFromLegacy()
{
    auto& s = AppSettings::instance();
    bool sawAny = s.contains(QStringLiteral("AutoStartDAX"));
    QJsonArray rates;
    QJsonArray enabled;
    for (int i = 1; i <= kIqChannels; ++i) {
        const QString rateKey = QStringLiteral("DaxIqRate%1").arg(i);
        const QString enKey = QStringLiteral("DaxIqEnabled%1").arg(i);
        if (s.contains(rateKey) || s.contains(enKey)) {
            sawAny = true;
        }
        rates.append(snapRate(
            s.value(rateKey, QString::number(kDefaultRateHz)).toInt()));
        enabled.append(
            s.value(enKey, QStringLiteral("False")).toString() == QLatin1String("True"));
    }
    if (!sawAny) {
        return {};
    }
    QJsonObject o;
    o[QStringLiteral("audioEnabled")] =
        s.value(QStringLiteral("AutoStartDAX"), QStringLiteral("False")).toString()
        == QLatin1String("True");
    o[QStringLiteral("iqRatesHz")] = rates;
    o[QStringLiteral("iqEnabled")] = enabled;
    return o;
}

bool DaxSettings::audioEnabled()
{
    return jsonBool(readObj().value(QStringLiteral("audioEnabled")), false);
}

void DaxSettings::setAudioEnabled(bool on)
{
    QJsonObject o = readObj();
    o[QStringLiteral("audioEnabled")] = on;
    write(o);
}

QVector<int> DaxSettings::iqChannelRatesHz()
{
    const QJsonArray arr = readObj().value(QStringLiteral("iqRatesHz")).toArray();
    QVector<int> result;
    result.reserve(kIqChannels);
    for (int i = 0; i < kIqChannels; ++i) {
        if (i < arr.size()) {
            result.append(snapRate(arr.at(i).toInt(kDefaultRateHz)));
        } else {
            result.append(kDefaultRateHz);
        }
    }
    return result;
}

void DaxSettings::setIqChannelRatesHz(const QVector<int>& ratesHz)
{
    QJsonObject o = readObj();
    QJsonArray arr;
    for (int i = 0; i < kIqChannels; ++i) {
        const int raw = (i < ratesHz.size()) ? ratesHz.at(i) : kDefaultRateHz;
        arr.append(snapRate(raw));
    }
    o[QStringLiteral("iqRatesHz")] = arr;
    write(o);
}

QVector<bool> DaxSettings::iqChannelEnabled()
{
    const QJsonArray arr = readObj().value(QStringLiteral("iqEnabled")).toArray();
    QVector<bool> result;
    result.reserve(kIqChannels);
    for (int i = 0; i < kIqChannels; ++i) {
        result.append(i < arr.size() ? jsonBool(arr.at(i), false) : false);
    }
    return result;
}

void DaxSettings::setIqChannelEnabled(const QVector<bool>& enabled)
{
    QJsonObject o = readObj();
    QJsonArray arr;
    for (int i = 0; i < kIqChannels; ++i) {
        arr.append(i < enabled.size() ? enabled.at(i) : false);
    }
    o[QStringLiteral("iqEnabled")] = arr;
    write(o);
}

bool DaxSettings::migrate()
{
    if (!storedObject().isEmpty()) {
        return false;
    }
    const QJsonObject legacy = buildFromLegacy();
    if (legacy.isEmpty()) {
        return false;
    }
    write(legacy);
    return true;
}

} // namespace AetherSDR
