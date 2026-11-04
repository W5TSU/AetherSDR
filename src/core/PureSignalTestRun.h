#pragma once

#include <QString>
#include <QVector>

#include <optional>

namespace AetherSDR {

// One run of the PureSignal window's test signal, as decisions with no radio or
// clock behind them. The caller ticks it and carries out the commands. Everything
// that ends a run ends it unkeyed, and a finished run never keys again.
class PureSignalTestRun {
public:
    enum class Mode {
        Calibrate,   // two-tone until PS has settled, or the time limit
        AB,          // two-tone, correction on and off in turn
        TwoTone,     // two-tone for the time asked
        SingleTone,  // a carrier for the time asked. PS can't calibrate on this.
    };
    enum class Phase { Idle, Listening, Transmitting, Identifying, Finished };
    enum class Command { KeyTwoTone, KeyCarrier, Unkey, BypassCorrection, UseCorrection };

    struct Config {
        Mode mode = Mode::TwoTone;
        int seconds = 10;
        bool psOn = false;
        bool identify = false;
        QString callsign;
    };
    struct Inputs {
        std::optional<float> levelDbm;  // receive level; none means no reading
        bool keyed = false;
        bool correcting = false;
        int calibrations = 0;
    };
    struct KeyEdge {
        int atMs = 0;
        bool down = false;
    };

    static constexpr int kMinSeconds = 5;
    static constexpr int kMaxSeconds = 30;
    static constexpr int kListenMs = 1500;
    static constexpr float kBusyDbm = -80.0f;
    static constexpr int kAbSwitchMs = 3000;
    static constexpr int kKeyConfirmMs = 500;
    static constexpr int kCalibrationsWanted = 3;
    static constexpr int kIdGapMs = 500;     // lets the transmit tail clear
    static constexpr int kDitMs = 100;       // 12 WPM

    QVector<Command> start(const Config& config, qint64 nowMs);
    QVector<Command> tick(qint64 nowMs, const Inputs& inputs);
    QVector<Command> stop(qint64 nowMs);

    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool active() const noexcept
    {
        return m_phase != Phase::Idle && m_phase != Phase::Finished;
    }
    [[nodiscard]] bool succeeded() const noexcept { return m_succeeded; }
    [[nodiscard]] const QString& result() const noexcept { return m_result; }

    // Key edges for `text`, from 0. What Morse has no code for is left out.
    static QVector<KeyEdge> morseKeying(const QString& text, int ditMs);

private:
    QVector<Command> endTransmit(qint64 nowMs, bool succeeded, const QString& result);
    QVector<Command> finish(bool succeeded, const QString& result);

    Config m_config;
    Phase m_phase = Phase::Idle;
    bool m_succeeded = false;
    QString m_result;
    qint64 m_phaseStartMs = 0;
    std::optional<float> m_peakDbm;
    int m_startCalibrations = 0;
    bool m_bypassed = false;
    int m_abFlips = 0;
    QVector<KeyEdge> m_id;
    qsizetype m_idNext = 0;
    qint64 m_idStartMs = 0;
};

} // namespace AetherSDR
