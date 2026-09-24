#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

namespace AetherSDR {

/**
 * @brief Discovers HackRF USB devices via libhackrf on a background worker thread.
 *
 * Emits RadioInfo with family="hackrf" so ConnectionPanel's existing
 * onRadioDiscovered/onRadioUpdated/onRadioLost slots consume it unchanged —
 * pattern-matches RtlSdrDiscovery exactly (#42), which is itself modeled on
 * Hl2Discovery, for consistent multi-backend discovery.
 */
class HackRfDiscovery : public QObject
{
    Q_OBJECT

public:
    explicit HackRfDiscovery(QObject* parent = nullptr);
    ~HackRfDiscovery() override;

    // Start/stop periodic USB scans
    void start(int intervalMs = 5000);
    void stop();

    [[nodiscard]] bool isRunning() const noexcept;

    // True when this build includes libhackrf support.
    static bool isAvailable();

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

private slots:
    void onScanTimer();
    void onScanFinished();

private:
    struct Seen {
        RadioInfo info;
        int missedScans = 0;
    };

    QTimer* m_timer = nullptr;
    QFutureWatcher<QVector<RadioInfo>> m_watcher;
    bool m_scanning = false;
    QHash<QString, Seen> m_seen;  // keyed by serial
    static constexpr int kMissedScansBeforeLost = 3;
};

}  // namespace AetherSDR
