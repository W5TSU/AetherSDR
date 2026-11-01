#include "core/backends/hl2/Hl2PsController.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::hl2 {
namespace {

constexpr double kTargetLevel = 152.293;
// Thetis's green band, wider than deskHPSDR's 150..155. On the radio the first
// calibration of each transmission reads a little off the settled level, and each
// gain change costs a reset, so the narrow band reset correction on every key-down.
constexpr int kDeadBandLow = 128;
constexpr int kDeadBandHigh = 181;
constexpr int kVeryStrongLevel = 275;
constexpr int kVeryWeakLevel = 25;
constexpr int kLargeStepDb = 15;

constexpr double kClippingDbfs = -1.0;
// Below this there is no signal to compare with: a pause in speech.
constexpr double kReferencePresentDbfs = -30.0;
// How far the gap may move from the one the curve was measured at. Amplifier
// compression moves it a little; a pulled sampler or an amplifier switched in or
// out moves it by much more.
constexpr double kGapToleranceDb = 10.0;

constexpr int kFailedFitsAllowedInARow = 3;
constexpr int kAttemptsBeforeFault = 20;

bool fitFailed(const Hl2PsSnapshot& s)
{
    return s.info[0] != 0 || s.info[1] != 0 || s.info[2] != 0 || s.info[3] != 0
        || (s.info[6] & 1) != 0;
}

}  // namespace

double hl2PsAlcTarget(Hl2PsState state)
{
    // Calibrating needs the headroom. A fault has nothing to gain from it, and the
    // uncorrected signal must not go out hotter than with PS off.
    return (state == Hl2PsState::Off || state == Hl2PsState::FeedbackWeak
            || state == Hl2PsState::FeedbackStrong)
               ? kHl2AlcTargetDefault
               : kHl2AlcTargetWithPureSignal;
}

bool hl2PsMaySaveCorrection(const Hl2PsSnapshot& snapshot, bool keyed)
{
    // Keyed, a save could hold the I/O thread, and EP2 with it, for up to a second.
    return !keyed && snapshot.info[14] != 0 && snapshot.correctionRunning;
}

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
    m_settledGapDb.reset();
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
    m_settledGapDb.reset();   // the gap is measured at a gain
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
    m_settledGapDb.reset();
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
        m_settledGapDb.reset();
        a.newGainDb = next;
        a.reset = true;
        m_pendingResume = true;
        m_state = Hl2PsState::Calibrating;
        return a;
    }

    // The sampler gone: the feedback falls away from the reference at once, long
    // before a calibration cycle could report it. Judged against the curve's own gap,
    // because the gap at a working gain can be small whatever is on the sampler.
    const bool referencePresent = s.feedbackPeakDbfs && s.referencePeakDbfs
                                  && *s.referencePeakDbfs > kReferencePresentDbfs;
    if (referencePresent && correcting && m_settledGapDb) {
        const double gap = *s.referencePeakDbfs - *s.feedbackPeakDbfs;
        if (gap > *m_settledGapDb + kGapToleranceDb)
            return fault(Hl2PsState::FeedbackWeak);
        if (gap < *m_settledGapDb - kGapToleranceDb) {
            // Stronger than the curve knows, such as the amplifier switched in.
            m_settledGapDb.reset();
            m_pendingResume = true;
            m_state = Hl2PsState::Calibrating;
            a.reset = true;
            return a;
        }
    }
    if (referencePresent && correcting && newSuccess && s.info[4] >= kDeadBandLow
        && s.info[4] <= kDeadBandHigh) {
        m_settledGapDb = *s.referencePeakDbfs - *s.feedbackPeakDbfs;
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
                m_settledGapDb.reset();
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
