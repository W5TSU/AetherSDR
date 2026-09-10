#pragma once

#include "core/CatPort.h"   // CatDialect

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

// One configured CAT listener: a TCP socket (and, on non-Windows, a PTY)
// speaking one protocol dialect, bound to a VFO-A / VFO-B slice pair.
// `dialect` is one of the stored tokens "Rigctld" / "TS2000" / "FlexCAT"
// (the Radio Setup combo displays "Rigctld" / "TS-2000" / "Flex"); use
// catDialectFromToken() / catDialectToken() to convert.
// `vfoB == -1` means simplex / no VFO B (CatPort::kVfoNone).
struct CatPortSpec {
    quint16 port = 0;
    QString dialect = QStringLiteral("Rigctld");
    bool enabled = false;
    int vfoA = 0;
    int vfoB = CatPort::kVfoNone;
};

// Stored-token <-> enum conversion, in one place so callers never re-implement
// the "== \"FlexCAT\" ? ... : \"TS2000\" ? ..." cascade.
CatDialect catDialectFromToken(const QString& token);
QString catDialectToken(CatDialect dialect);

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
    // port 4532 (the historical default) so the UI always has a row to show.
    static QVector<CatPortSpec> ports();
    static void setPorts(const QVector<CatPortSpec>& ports);

    // Bounds-checked accessor; returns a default CatPortSpec for an index
    // outside the configured list.
    static CatPortSpec portAt(int index);

    // The single "does this listener actually run" rule: master enabled AND the
    // listener enabled AND its port is non-privileged (>= 1024). Every caller
    // (the GUI apply routine, the applet lock state, activePortCount) is a thin
    // wrapper around this.
    static bool listenerRuns(const CatPortSpec& spec, bool masterEnabled);

    // Count of listeners that would run given the current master enable.
    static int activePortCount();

    // One-way migration from the legacy flat keys. Returns true iff it wrote
    // the nested object (legacy keys present and no valid nested object yet).
    // The legacy keys are NOT removed. Idempotent; a corrupt stored value is
    // treated as absent and replaced.
    static bool migrate();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
    static QJsonObject buildFromLegacy();
    static QVector<CatPortSpec> defaultPorts();
};

} // namespace AetherSDR
