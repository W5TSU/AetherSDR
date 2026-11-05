#include "core/PureSignalTestRun.h"

#include <QHash>
#include <QObject>

#include <algorithm>

namespace AetherSDR {

namespace {
const char* morseFor(QChar c)
{
    static const QHash<QChar, const char*> table{
        {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},
        {'F', "..-."},  {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},
        {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},    {'O', "---"},
        {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
        {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},  {'Y', "-.--"},
        {'Z', "--.."},  {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
        {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."},
        {'9', "----."}, {'/', "-..-."},
    };
    return table.value(c.toUpper(), nullptr);
}
}

QVector<PureSignalTestRun::KeyEdge> PureSignalTestRun::morseKeying(const QString& text, int ditMs)
{
    QVector<KeyEdge> edges;
    int at = 0;
    for (const QChar c : text) {
        const char* code = morseFor(c);
        if (!code)
            continue;
        for (const char* element = code; *element; ++element) {
            edges.append({at, true});
            at += (*element == '-' ? 3 : 1) * ditMs;
            edges.append({at, false});
            at += ditMs;
        }
        at += 2 * ditMs;  // with the one above, three between letters
    }
    return edges;
}

QVector<PureSignalTestRun::Command> PureSignalTestRun::finish(bool succeeded, const QString& result)
{
    m_phase = Phase::Finished;
    m_succeeded = succeeded;
    m_result = result;
    return {};
}

QVector<PureSignalTestRun::Command> PureSignalTestRun::start(const Config& config, qint64 nowMs)
{
    *this = PureSignalTestRun{};
    m_config = config;
    m_config.seconds = std::clamp(config.seconds, kMinSeconds, kMaxSeconds);
    if (m_config.mode == Mode::Calibrate || m_config.mode == Mode::AB) {
        if (!m_config.psOn)
            return finish(false, QObject::tr("Turn PS on first"));
    }
    if (m_config.identify) {
        m_id = morseKeying(m_config.callsign, kDitMs);
        if (m_id.isEmpty())
            return finish(false, QObject::tr("No callsign set to identify with"));
    }
    m_phase = Phase::Listening;
    m_phaseStartMs = nowMs;
    return {};
}

QVector<PureSignalTestRun::Command> PureSignalTestRun::endTransmit(qint64 nowMs, bool succeeded,
                                                                   const QString& result)
{
    QVector<Command> commands;
    if (m_bypassed)
        commands.append(Command::UseCorrection);
    m_bypassed = false;
    commands.append(Command::Unkey);
    finish(succeeded, result);
    if (m_config.identify) {
        m_phase = Phase::Identifying;
        m_idNext = 0;
        m_idStartMs = nowMs + kIdGapMs;
    }
    return commands;
}

QVector<PureSignalTestRun::Command> PureSignalTestRun::tick(qint64 nowMs, const Inputs& inputs)
{
    const qint64 elapsed = nowMs - m_phaseStartMs;
    switch (m_phase) {
    case Phase::Idle:
    case Phase::Finished:
        return {};
    case Phase::Listening:
        if (inputs.levelDbm)
            m_peakDbm = std::max(m_peakDbm.value_or(*inputs.levelDbm), *inputs.levelDbm);
        if (elapsed < kListenMs)
            return {};
        if (!m_peakDbm)
            return finish(false, QObject::tr("No receive level to check the frequency with"));
        if (*m_peakDbm > kBusyDbm) {
            return finish(false, QObject::tr("Frequency busy (%1 dBm), nothing sent")
                                     .arg(static_cast<double>(*m_peakDbm), 0, 'f', 0));
        }
        m_phase = Phase::Transmitting;
        m_phaseStartMs = nowMs;
        m_startCalibrations = inputs.calibrations;
        return {m_config.mode == Mode::SingleTone ? Command::KeyCarrier : Command::KeyTwoTone};
    case Phase::Transmitting: {
        if (elapsed >= kKeyConfirmMs && !inputs.keyed) {
            const QVector<Command> commands = endTransmit(nowMs, false, QObject::tr("Keying stopped"));
            finish(false, m_result);
            return commands;
        }
        if (m_config.mode == Mode::Calibrate && inputs.correcting
            && inputs.calibrations >= m_startCalibrations + kCalibrationsWanted) {
            return endTransmit(nowMs, true, QObject::tr("Calibrated"));
        }
        if (elapsed >= m_config.seconds * 1000LL) {
            if (m_config.mode == Mode::Calibrate)
                return endTransmit(nowMs, false, QObject::tr("Time limit: PS hadn't settled"));
            return endTransmit(nowMs, true, QObject::tr("Finished"));
        }
        if (m_config.mode == Mode::AB) {
            const int flips = static_cast<int>(elapsed / kAbSwitchMs);
            if (flips > m_abFlips) {
                m_abFlips = flips;
                m_bypassed = !m_bypassed;
                return {m_bypassed ? Command::BypassCorrection : Command::UseCorrection};
            }
        }
        return {};
    }
    case Phase::Identifying: {
        QVector<Command> commands;
        while (m_idNext < m_id.size() && nowMs >= m_idStartMs + m_id[m_idNext].atMs) {
            commands.append(m_id[m_idNext].down ? Command::KeyCarrier : Command::Unkey);
            ++m_idNext;
        }
        if (m_idNext >= m_id.size())
            finish(m_succeeded, m_result);
        return commands;
    }
    }
    return {};
}

QVector<PureSignalTestRun::Command> PureSignalTestRun::stop(qint64 nowMs)
{
    Q_UNUSED(nowMs);
    QVector<Command> commands;
    if (m_phase == Phase::Transmitting || m_phase == Phase::Identifying) {
        if (m_bypassed)
            commands.append(Command::UseCorrection);
        commands.append(Command::Unkey);
    }
    m_bypassed = false;
    if (active())
        finish(false, QObject::tr("Stopped"));
    return commands;
}

} // namespace AetherSDR
