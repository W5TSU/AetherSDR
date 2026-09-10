#include "core/CatSettings.h"

#include "core/AppSettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>

namespace AetherSDR {

namespace {

const QString kRootKey = QStringLiteral("CatServer");

// Historical rigctld default (matches the old CatPort_0 seed).
constexpr quint16 kDefaultRigctldPort = 4532;

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

CatPortSpec specFromJson(const QJsonObject& o)
{
    CatPortSpec spec;
    spec.port = static_cast<quint16>(o.value(QStringLiteral("port")).toInt(0));
    spec.dialect = o.value(QStringLiteral("dialect")).toString(QStringLiteral("Rigctld"));
    spec.enabled = asBool(o.value(QStringLiteral("enabled")), false);
    spec.vfoA = o.value(QStringLiteral("vfoA")).toInt(-1);
    spec.vfoB = o.value(QStringLiteral("vfoB")).toInt(-1);
    return spec;
}

QJsonObject specToJson(const CatPortSpec& spec)
{
    QJsonObject o;
    o[QStringLiteral("port")] = static_cast<int>(spec.port);
    o[QStringLiteral("dialect")] = spec.dialect;
    o[QStringLiteral("enabled")] = spec.enabled;
    o[QStringLiteral("vfoA")] = spec.vfoA;
    o[QStringLiteral("vfoB")] = spec.vfoB;
    return o;
}

} // namespace

QVector<CatPortSpec> CatSettings::defaultPorts()
{
    CatPortSpec spec;
    spec.port = kDefaultRigctldPort;
    spec.dialect = QStringLiteral("Rigctld");
    spec.enabled = false;
    spec.vfoA = 0;
    spec.vfoB = -1;
    return {spec};
}

QJsonObject CatSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kRootKey, QString{}).toString();
    if (!json.isEmpty()) {
        const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
        if (!o.isEmpty()) {
            return o;
        }
    }
    // No stored object (or unparseable): fall back to a view built from the
    // legacy flat keys, so a first read before migrate() still sees them.
    return buildFromLegacy();
}

void CatSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

QJsonObject CatSettings::buildFromLegacy()
{
    auto& s = AppSettings::instance();

    bool sawAny = s.contains(QStringLiteral("CatEnabled"));
    QVector<CatPortSpec> ports;
    for (int i = 0; i < kMaxPorts; ++i) {
        const QString pfx = QStringLiteral("CatPort_%1_").arg(i);
        if (!s.contains(pfx + QStringLiteral("Port"))
            && !s.contains(pfx + QStringLiteral("Enabled"))) {
            continue;
        }
        sawAny = true;
        CatPortSpec spec;
        spec.port = static_cast<quint16>(
            s.value(pfx + QStringLiteral("Port"), QString{}).toInt());
        spec.dialect =
            s.value(pfx + QStringLiteral("Dialect"), QStringLiteral("Rigctld")).toString();
        spec.enabled =
            s.value(pfx + QStringLiteral("Enabled"), QStringLiteral("False")).toString()
            == QLatin1String("True");
        spec.vfoA = s.value(pfx + QStringLiteral("VfoA"), QStringLiteral("0")).toInt();
        spec.vfoB = s.value(pfx + QStringLiteral("VfoB"), QStringLiteral("-1")).toInt();
        ports.append(spec);
    }

    if (!sawAny) {
        return {};
    }

    // Drop trailing placeholder listeners (no port, not enabled).
    while (!ports.isEmpty() && ports.last().port == 0 && !ports.last().enabled) {
        ports.removeLast();
    }

    QJsonObject o;
    o[QStringLiteral("enabled")] =
        s.value(QStringLiteral("CatEnabled"), QStringLiteral("False")).toString()
        == QLatin1String("True");
    QJsonArray arr;
    for (const CatPortSpec& spec : ports) {
        arr.append(specToJson(spec));
    }
    o[QStringLiteral("ports")] = arr;
    return o;
}

bool CatSettings::enabled()
{
    return asBool(readObj().value(QStringLiteral("enabled")), false);
}

void CatSettings::setEnabled(bool on)
{
    QJsonObject o = readObj();
    o[QStringLiteral("enabled")] = on;
    if (!o.contains(QStringLiteral("ports"))) {
        QJsonArray arr;
        for (const CatPortSpec& spec : defaultPorts()) {
            arr.append(specToJson(spec));
        }
        o[QStringLiteral("ports")] = arr;
    }
    write(o);
}

QVector<CatPortSpec> CatSettings::ports()
{
    const QJsonObject o = readObj();
    if (!o.contains(QStringLiteral("ports"))) {
        return defaultPorts();
    }
    QVector<CatPortSpec> result;
    const QJsonArray arr = o.value(QStringLiteral("ports")).toArray();
    for (const QJsonValue& v : arr) {
        result.append(specFromJson(v.toObject()));
        if (result.size() == kMaxPorts) {
            break;
        }
    }
    return result;
}

void CatSettings::setPorts(const QVector<CatPortSpec>& ports)
{
    QJsonObject o = readObj();
    QJsonArray arr;
    const int n = std::min<int>(ports.size(), kMaxPorts);
    for (int i = 0; i < n; ++i) {
        arr.append(specToJson(ports.at(i)));
    }
    o[QStringLiteral("ports")] = arr;
    if (!o.contains(QStringLiteral("enabled"))) {
        o[QStringLiteral("enabled")] = false;
    }
    write(o);
}

int CatSettings::activePortCount()
{
    if (!enabled()) {
        return 0;
    }
    int count = 0;
    for (const CatPortSpec& spec : ports()) {
        if (spec.enabled && spec.port >= 1024) {
            ++count;
        }
    }
    return count;
}

bool CatSettings::migrate()
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
