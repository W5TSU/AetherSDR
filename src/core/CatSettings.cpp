#include "core/CatSettings.h"

#include "core/AppSettings.h"
#include "core/SettingsJsonUtil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>

namespace AetherSDR {

namespace {

const QString kRootKey = QStringLiteral("CatServer");

// Historical rigctld default (matches the old CatPort_0 seed).
constexpr quint16 kDefaultRigctldPort = 4532;

CatPortSpec specFromJson(const QJsonObject& o)
{
    CatPortSpec spec;
    spec.port = static_cast<quint16>(o.value(QStringLiteral("port")).toInt(0));
    spec.dialect = o.value(QStringLiteral("dialect")).toString(QStringLiteral("Rigctld"));
    spec.enabled = jsonBool(o.value(QStringLiteral("enabled")), false);
    spec.vfoA = o.value(QStringLiteral("vfoA")).toInt(0);
    spec.vfoB = o.value(QStringLiteral("vfoB")).toInt(CatPort::kVfoNone);
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

// The stored value of CatServer as a parsed object, or an empty object when the
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

CatDialect catDialectFromToken(const QString& token)
{
    if (token == QLatin1String("FlexCAT")) {
        return CatDialect::FlexCAT;
    }
    if (token == QLatin1String("TS2000")) {
        return CatDialect::TS2000;
    }
    return CatDialect::Rigctld;
}

QString catDialectToken(CatDialect dialect)
{
    switch (dialect) {
    case CatDialect::TS2000:
        return QStringLiteral("TS2000");
    case CatDialect::FlexCAT:
        return QStringLiteral("FlexCAT");
    case CatDialect::Rigctld:
        break;
    }
    return QStringLiteral("Rigctld");
}

QVector<CatPortSpec> CatSettings::defaultPorts()
{
    CatPortSpec spec;
    spec.port = kDefaultRigctldPort;
    spec.dialect = QStringLiteral("Rigctld");
    spec.enabled = false;
    spec.vfoA = 0;
    spec.vfoB = CatPort::kVfoNone;
    return {spec};
}

QJsonObject CatSettings::readObj()
{
    const QJsonObject stored = storedObject();
    if (!stored.isEmpty()) {
        return stored;
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
        spec.vfoB = s.value(pfx + QStringLiteral("VfoB"),
                            QString::number(CatPort::kVfoNone)).toInt();
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
    return jsonBool(readObj().value(QStringLiteral("enabled")), false);
}

void CatSettings::setEnabled(bool on)
{
    QJsonObject o = readObj();
    o[QStringLiteral("enabled")] = on;
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

CatPortSpec CatSettings::portAt(int index)
{
    const QVector<CatPortSpec> p = ports();
    return (index >= 0 && index < p.size()) ? p.at(index) : CatPortSpec{};
}

bool CatSettings::listenerRuns(const CatPortSpec& spec, bool masterEnabled)
{
    return masterEnabled && spec.enabled && spec.port >= 1024;
}

int CatSettings::activePortCount()
{
    const bool master = enabled();
    int count = 0;
    for (const CatPortSpec& spec : ports()) {
        if (listenerRuns(spec, master)) {
            ++count;
        }
    }
    return count;
}

bool CatSettings::migrate()
{
    // A valid, non-empty stored object means this service is already migrated.
    // A corrupt value is treated as absent (readObj() falls back to legacy) and
    // is replaced below.
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
