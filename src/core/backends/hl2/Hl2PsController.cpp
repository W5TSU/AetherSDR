#include "core/backends/hl2/Hl2PsController.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::hl2 {
namespace {

constexpr double kTargetLevel = 152.293;
constexpr int kDeadBandLow = 150;
constexpr int kDeadBandHigh = 155;
constexpr int kVeryStrongLevel = 275;
constexpr int kVeryWeakLevel = 25;
constexpr int kLargeStepDb = 15;

constexpr double kClippingDbfs = -1.0;
// Below this there is no signal to compare with: a pause in speech.
constexpr double kReferencePresentDbfs = -30.0;
constexpr double kFeedbackGoneGapDb = 40.0;

constexpr int kFailedFitsAllowedInARow = 3;
constexpr int kAttemptsBeforeFault = 20;

bool fitFailed(const Hl2PsSnapshot& s)
{
    return s.info[0] != 0 || s.info[1] != 0 || s.info[2] != 0 || s.info[3] != 0
        || (s.info[6] & 1) != 0;
}

}  // namespace

QString hl2PsStateName(Hl2PsState state)
{
    switch (state) {
    case Hl2PsState::Off: return QStringLiteral("off");
    case Hl2PsState::Idle: return QStringLiteral("idle");
    case Hl2PsState::Calibrating: return QStringLiteral("calibrating");
    case Hl2PsState::Correcting: return QStringLiteral("correcting");
    case Hl2PsState::FeedbackWeak: return QStringLiteral("feedback too weak");
    case Hl2PsState::FeedbackStrong: return QStringLiteral("feedback too strong");
    }
    return QStringLiteral("off");
}

Hl2PsActions Hl2PsController::setEnabled(bool on, bool correctionRestored)
{
    Hl2PsActions a;
    if (on == m_enabled)
        return a;
    m_enabled = on;
    m_wasKeyed = false;
    m_faulted = false;
    m_pendingResume = false;
    m_recalibrate = false;
    m_failedFitsInARow = 0;
    m_attemptsSinceSuccess = 0;
    if (on) {
        m_state = correctionRestored ? Hl2PsState::Correcting : Hl2PsState::Idle;
        a.resume = !correctionRestored;
    } else {
        m_state = Hl2PsState::Off;
        a.reset = true;
    }
    return a;
}

void Hl2PsController::setGainDb(int db)
{
    m_gainDb = std::clamp(db, kMinGainDb, kMaxGainDb);
}

void Hl2PsController::requestRecalibration() noexcept
{
    if (m_enabled)
        m_recalibrate = true;
}

Hl2PsActions Hl2PsController::fault(Hl2PsState state)
{
    m_state = state;
    m_faulted = true;
    m_pendingResume = false;
    Hl2PsActions a;
    a.reset = true;
    return a;
}

Hl2PsActions Hl2PsController::tick(const Hl2PsSnapshot& s, bool keyedModulated)
{
    Hl2PsActions a;
    if (!m_enabled)
        return a;

    const bool newAttempt = s.info[7] != m_lastAttempts;
    const bool newSuccess = s.info[5] != m_lastSuccesses;
    m_lastAttempts = s.info[7];
    m_lastSuccesses = s.info[5];

    const bool correcting = s.info[14] != 0;

    if (!keyedModulated) {
        m_wasKeyed = false;
        if (!m_faulted)
            m_state = correcting ? Hl2PsState::Correcting : Hl2PsState::Idle;
        return a;
    }

    if (!m_wasKeyed) {
        m_wasKeyed = true;
        m_failedFitsInARow = 0;
        m_attemptsSinceSuccess = 0;
        if (m_faulted) {
            m_faulted = false;
            m_state = Hl2PsState::Calibrating;
            a.resume = true;
            return a;
        }
    }

    if (m_faulted)
        return a;

    if (m_pendingResume) {
        m_pendingResume = false;
        a.resume = true;
        m_state = Hl2PsState::Calibrating;
        return a;
    }

    if (m_recalibrate) {
        m_recalibrate = false;
        a.resume = true;
        m_state = Hl2PsState::Calibrating;
        return a;
    }

    // Too hot for the converter, whatever calcc makes of it.
    if (s.feedbackPeakDbfs && *s.feedbackPeakDbfs > kClippingDbfs) {
        const int next = std::max(kMinGainDb, m_gainDb - kLargeStepDb);
        if (next == m_gainDb)
            return fault(Hl2PsState::FeedbackStrong);
        m_gainDb = next;
        a.newGainDb = next;
        a.reset = true;
        m_pendingResume = true;
        m_state = Hl2PsState::Calibrating;
        return a;
    }

    // The sampler gone: the feedback falls away from the reference at once, long
    // before a calibration cycle could report it.
    if (correcting && s.feedbackPeakDbfs && s.referencePeakDbfs
        && *s.referencePeakDbfs > kReferencePresentDbfs
        && *s.feedbackPeakDbfs < *s.referencePeakDbfs - kFeedbackGoneGapDb) {
        return fault(Hl2PsState::FeedbackWeak);
    }

    if (newSuccess) {
        m_failedFitsInARow = 0;
        m_attemptsSinceSuccess = 0;
    } else if (newAttempt && fitFailed(s)) {
        ++m_failedFitsInARow;
        ++m_attemptsSinceSuccess;
    } else if (newAttempt) {
        ++m_attemptsSinceSuccess;
    }

    if (correcting && m_failedFitsInARow >= kFailedFitsAllowedInARow)
        return fault(Hl2PsState::FeedbackWeak);

    m_state = correcting ? Hl2PsState::Correcting : Hl2PsState::Calibrating;

    if (newAttempt || newSuccess) {
        const int level = s.info[4];
        if (level > kDeadBandHigh || level < kDeadBandLow) {
            int step;
            if (level > kVeryStrongLevel)
                step = -kLargeStepDb;
            else if (level < kVeryWeakLevel)
                step = kLargeStepDb;
            else
                step = -static_cast<int>(std::lround(20.0 * std::log10(level / kTargetLevel)));
            const int next = std::clamp(m_gainDb + step, kMinGainDb, kMaxGainDb);
            if (next != m_gainDb) {
                m_gainDb = next;
                a.newGainDb = next;
                a.reset = true;
                m_pendingResume = true;
                m_state = Hl2PsState::Calibrating;
                m_attemptsSinceSuccess = 0;
                return a;
            }
        }
    }

    if (!correcting && m_attemptsSinceSuccess >= kAttemptsBeforeFault)
        return fault(s.info[4] > kDeadBandHigh ? Hl2PsState::FeedbackStrong
                                               : Hl2PsState::FeedbackWeak);
    return a;
}

}  // namespace AetherSDR::hl2
