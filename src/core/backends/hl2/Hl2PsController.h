#pragma once

#include "core/backends/hl2/Hl2PureSignal.h"

#include <QString>

#include <optional>

namespace AetherSDR::hl2 {

enum class Hl2PsState { Off, Idle, Calibrating, Correcting, FeedbackWeak, FeedbackStrong };

QString hl2PsStateName(Hl2PsState state);

inline constexpr double kHl2AlcTargetDefault = 0.85;
inline constexpr double kHl2AlcTargetWithPureSignal = 0.99;
double hl2PsAlcTarget(Hl2PsState state);
bool hl2PsMaySaveCorrection(const Hl2PsSnapshot& snapshot, bool keyed);

// One tick's worth of instructions. A reset ends correction, ramped. A resume
// starts calibrating again.
struct Hl2PsActions {
    std::optional<int> newGainDb;
    bool reset = false;
    bool resume = false;
};

// Every PureSignal decision, in one place with no WDSP and no radio behind it:
// which state the operator sees, when correction must stop, and the receive gain
// that keeps the feedback in its working range. Hl2PureSignal carries the results
// out.
//
// The level rules are deskHPSDR's auto-attenuate (ps_menu.c), with a wider dead
// band and the sign turned round, because `0x0e` is a gain where it has an attenuator.
class Hl2PsController {
public:
    static constexpr int kMinGainDb = -12;
    static constexpr int kMaxGainDb = 48;

    // `correctionRestored`: a saved curve is already loaded, so hold it rather
    // than ask for a calibration, which would have to turn it off first.
    Hl2PsActions setEnabled(bool on, bool correctionRestored = false);
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

    // The gain for this band. Starts at the minimum: PURE's attenuator is not
    // documented, so an unknown level must not start out driving the converter.
    void setGainDb(int db);
    [[nodiscard]] int gainDb() const noexcept { return m_gainDb; }

    // Something changed that a held curve no longer describes, such as drive.
    void requestRecalibration() noexcept;

    // Every 100 ms. `keyedModulated` is voice, data or two-tone on the air.
    Hl2PsActions tick(const Hl2PsSnapshot& snapshot, bool keyedModulated);

    // How far the feedback sat below the reference when the curve was good. A curve
    // restored from a file brings its own, because it gets no calibrations to check
    // it: this comparison is all that watches it.
    void setSettledGapDb(std::optional<double> gapDb) { m_settledGapDb = gapDb; }
    [[nodiscard]] std::optional<double> settledGapDb() const noexcept { return m_settledGapDb; }

    [[nodiscard]] Hl2PsState state() const noexcept { return m_state; }

private:
    Hl2PsActions fault(Hl2PsState state);

    bool m_enabled = false;
    int m_gainDb = kMinGainDb;
    Hl2PsState m_state = Hl2PsState::Off;
    bool m_wasKeyed = false;
    bool m_faulted = false;
    bool m_pendingResume = false;
    bool m_recalibrate = false;
    std::optional<double> m_settledGapDb;
    int m_lastSuccesses = 0;
    int m_lastAttempts = 0;
    int m_failedFitsInARow = 0;
    int m_attemptsSinceSuccess = 0;
};

}  // namespace AetherSDR::hl2
