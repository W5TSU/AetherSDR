#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

// One configured CAT listener: a TCP socket (and, on non-Windows, a PTY)
// speaking one protocol dialect, bound to a VFO-A / VFO-B slice pair.
// `dialect` is one of the stored tokens "Rigctld" / "TS2000" / "FlexCAT"
// (the Radio Setup combo displays "Rigctld" / "TS-2000" / "Flex").
// `vfoB == -1` means simplex / no VFO B (CatPort::kVfoNone).
struct CatPortSpec {
    quint16 port = 0;
    QString dialect = QStringLiteral("Rigctld");
    bool enabled = false;
    int vfoA = -1;
    int vfoB = -1;
};

// Persistence for AetherSDR's CAT server. Per Constitution Principle V/XIV the
// configuration lives as one nested JSON object under a single AppSettings key
// ("CatServer"):
//   { "enabled": bool, "ports": [ { port, dialect, enabled, vfoA, vfoB }, ... ] }
// Legacy flat keys (CatEnabled, CatPort_<n>_*) are migrated once and then left
// in place for downgrade safety; nothing new is ever written to them.
class CatSettings {
public:
    static constexpr int kMaxPorts = 8;

    // Master "Enable CAT server" — starts/stops the listeners now and is
    // re-applied on radio connect.
    static bool enabled();
    static void setEnabled(bool on);

    // The configured listeners, in order. At most kMaxPorts; a shorter list is
    // normal. On a fresh store this returns one disabled rigctld listener on
    // port 4532 (the historical default).
    static QVector<CatPortSpec> ports();
    static void setPorts(const QVector<CatPortSpec>& ports);

    // Count of listeners that would actually run: master enabled AND the
    // listener enabled AND its port is non-privileged (>= 1024). The GUI's
    // apply routine is a thin caller of this.
    static int activePortCount();

    // One-way migration from the legacy flat keys. Returns true iff it wrote
    // the nested object (legacy keys present and no nested object yet). The
    // legacy keys are NOT removed. Idempotent.
    static bool migrate();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
    static QJsonObject buildFromLegacy();
    static QVector<CatPortSpec> defaultPorts();
};

} // namespace AetherSDR
