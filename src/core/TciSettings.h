#pragma once

#include <QJsonObject>
#include <QtGlobal>

namespace AetherSDR {

// Persistence for AetherSDR's TCI (ExpertSDR) WebSocket server. Per
// Constitution Principle V/XIV the setup lives as one nested JSON object under
// a single AppSettings key ("TciServer"):
//   { "enabled": bool, "port": int }
// RX/TX gains and the TX overflow mode are NOT here — they are operational
// state owned by TciServer and adjusted from the TCI status tile. Legacy flat
// keys (AutoStartTCI, TciPort) are migrated once and left in place.
class TciSettings {
public:
    static constexpr quint16 kDefaultPort = 50001;

    static bool enabled();
    static void setEnabled(bool on);

    // Listen port. A value outside [1024, 65535] falls back to kDefaultPort.
    static quint16 port();
    static void setPort(quint16 port);

    // One-way migration from AutoStartTCI / TciPort. Returns true iff it wrote
    // the nested object. Legacy keys are not removed. Idempotent.
    static bool migrate();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
    static QJsonObject buildFromLegacy();
    static quint16 sanitizePort(int raw);
};

} // namespace AetherSDR
