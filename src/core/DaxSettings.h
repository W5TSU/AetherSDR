#pragma once

#include <QJsonObject>
#include <QVector>

namespace AetherSDR {

// Persistence for AetherSDR's DAX virtual-audio and DAX-IQ interfaces. Per
// Constitution Principle V/XIV the setup lives as one nested JSON object under
// a single AppSettings key ("DaxServer"):
//   { "audioEnabled": bool,
//     "iqRatesHz":   [r0, r1, r2, r3],
//     "iqEnabled":   [b0, b1, b2, b3] }
// DAX RX/TX gains stay operational state owned by the DAX tile / AudioEngine.
// Legacy flat keys (AutoStartDAX, DaxIqRate<n>, DaxIqEnabled<n>) are migrated
// once and left in place.
class DaxSettings {
public:
    static constexpr int kIqChannels = 4;

    static bool audioEnabled();
    static void setAudioEnabled(bool on);

    // Per-channel DAX-IQ sample rate in Hz; always length kIqChannels. Values
    // are snapped to the nearest legal rate {24000, 48000, 96000, 192000}.
    static QVector<int> iqChannelRatesHz();
    static void setIqChannelRatesHz(const QVector<int>& ratesHz);

    // Per-channel DAX-IQ enable; always length kIqChannels.
    static QVector<bool> iqChannelEnabled();
    static void setIqChannelEnabled(const QVector<bool>& enabled);

    // One-way migration from AutoStartDAX / DaxIqRate<n> / DaxIqEnabled<n>.
    // Returns true iff it wrote the nested object. Legacy keys are not
    // removed. Idempotent.
    static bool migrate();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
    static QJsonObject buildFromLegacy();
    static int snapRate(int rawHz);
};

} // namespace AetherSDR
