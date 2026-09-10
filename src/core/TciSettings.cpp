#include "core/TciSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonValue>

namespace AetherSDR {

namespace {

const QString kRootKey = QStringLiteral("TciServer");

bool asBool(const QJsonValue& v, bool fallback)
{
    if (v.isBool()) {
        return v.toBool();
    }
    if (v.isString()) {
        return v.toString() == QLatin1String("True");
    }
    return fallback;
}

} // namespace

quint16 TciSettings::sanitizePort(int raw)
{
    if (raw < 1024 || raw > 65535) {
        return kDefaultPort;
    }
    return static_cast<quint16>(raw);
}

QJsonObject TciSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kRootKey, QString{}).toString();
    if (!json.isEmpty()) {
        const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
        if (!o.isEmpty()) {
            return o;
        }
    }
    return buildFromLegacy();
}

void TciSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

QJsonObject TciSettings::buildFromLegacy()
{
    auto& s = AppSettings::instance();
    const bool hasEnable = s.contains(QStringLiteral("AutoStartTCI"));
    const bool hasPort = s.contains(QStringLiteral("TciPort"));
    if (!hasEnable && !hasPort) {
        return {};
    }
    QJsonObject o;
    o[QStringLiteral("enabled")] =
        s.value(QStringLiteral("AutoStartTCI"), QStringLiteral("False")).toString()
        == QLatin1String("True");
    o[QStringLiteral("port")] = static_cast<int>(sanitizePort(
        s.value(QStringLiteral("TciPort"), QString::number(kDefaultPort)).toInt()));
    return o;
}

bool TciSettings::enabled()
{
    return asBool(readObj().value(QStringLiteral("enabled")), false);
}

void TciSettings::setEnabled(bool on)
{
    QJsonObject o = readObj();
    o[QStringLiteral("enabled")] = on;
    if (!o.contains(QStringLiteral("port"))) {
        o[QStringLiteral("port")] = static_cast<int>(kDefaultPort);
    }
    write(o);
}

quint16 TciSettings::port()
{
    const QJsonObject o = readObj();
    if (!o.contains(QStringLiteral("port"))) {
        return kDefaultPort;
    }
    return sanitizePort(o.value(QStringLiteral("port")).toInt(kDefaultPort));
}

void TciSettings::setPort(quint16 port)
{
    QJsonObject o = readObj();
    o[QStringLiteral("port")] = static_cast<int>(sanitizePort(port));
    if (!o.contains(QStringLiteral("enabled"))) {
        o[QStringLiteral("enabled")] = false;
    }
    write(o);
}

bool TciSettings::migrate()
{
    auto& s = AppSettings::instance();
    if (!s.value(kRootKey, QString{}).toString().isEmpty()) {
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
