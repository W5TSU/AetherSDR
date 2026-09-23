#pragma once

#include <QObject>

namespace AetherSDR::hackrf {

// The RX/TX arbitration state machine (#42 design doc's "Architecture ->
// RX/TX arbitration" section) — the one piece of the HackRF backend with no
// precedent to copy. HL2's hardware streams RX and TX IQ concurrently over
// Ethernet, so Hl2Backend never had to solve transport-level RX/TX
// exclusivity. A single HackRF's USB pipe genuinely cannot run
// hackrf_start_rx and hackrf_start_tx at once: RX must be torn down before
// TX starts, and rebuilt after.
//
// This class is deliberately decoupled from HackRfWorker/libhackrf so it is
// unit-testable without hardware (the design doc calls this out explicitly):
// it never calls a hackrf_* function itself. It emits "want" signals telling
// its owner (HackRfBackend) what to do, and the owner reports back with
// confirm*() once that request completes. tick() drives timeout recovery —
// pass a monotonic millisecond clock rather than owning a QTimer, so tests
// can simulate elapsed time without a running event loop.
//
// Rapid PTT bounce (a short CW dit, or letting go of PTT before an in-flight
// transition confirms) is handled by ALWAYS re-checking what's currently
// wanted at the moment a pending transition resolves, rather than blindly
// completing whatever was originally requested — the same "coalesce to the
// latest intent, not a stale queued one" principle behind the Icom
// command-plane fix for a delayed PTT-OFF reply arriving after a newer
// PTT-ON and cutting transmit audio. A confirmRxStopped() that arrives after
// the operator has already asked for RX again does not start TX; it starts
// RX, because RX is what's wanted NOW.
class HackRfTxRxArbiter : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, RxStreaming, TxPending, TxStreaming, RxPending };
    Q_ENUM(State)

    explicit HackRfTxRxArbiter(QObject* parent = nullptr);

    State state() const { return m_state; }

    // How long a pending transition may wait for its confirm*() before
    // tick() gives up and forces a safe recovery. Exposed (not just a
    // private constant) so a test can pick a deadline shorter than any
    // real timer resolution rather than waiting on wall-clock time.
    void setTimeoutMs(qint64 ms) { m_timeoutMs = ms; }
    qint64 timeoutMs() const { return m_timeoutMs; }

    // Caller wants RX active (initial connect, or PTT released). Idempotent
    // if already RX or already transitioning toward RX.
    void requestRx(qint64 nowMs);
    // Caller wants TX active (PTT pressed / CW key down).
    void requestTx(qint64 nowMs);

    // The owner reports that the RX teardown it was asked for (wantRxStop())
    // has completed. Ignored if not currently in TxPending — a confirmation
    // that arrives after a timeout already forced recovery, or arrives
    // twice, must not re-drive the state machine from an unexpected state.
    void confirmRxStopped(qint64 nowMs);
    // Symmetric: the TX teardown requested by wantTxStop() has completed.
    void confirmTxStopped(qint64 nowMs);

    // Call periodically (e.g. every 50ms, from a QTimer HackRfBackend owns)
    // with the current monotonic time. No-op unless a pending transition has
    // outlived timeoutMs(), in which case it forces the arbiter back to
    // Idle — never to TxStreaming: an unconfirmed teardown is exactly the
    // situation where guessing "probably fine, proceed to TX" is the wrong
    // failure direction. Idle recovers cleanly on the next requestRx()/
    // requestTx(), which starts fresh rather than assuming any pending
    // transition's state.
    void tick(qint64 nowMs);

signals:
    void stateChanged(State state);
    // "Do this now" — the owner calls the matching HackRfWorker method and,
    // once it returns, calls the corresponding confirm*() back.
    void wantRxStart();
    void wantRxStop();
    void wantTxStart();
    void wantTxStop();
    // A pending transition timed out waiting for its confirmation. The
    // arbiter has already forced itself back to Idle by the time this
    // fires — this is notification for logging/UI, not a request to act.
    void arbitrationTimedOut(State pendingState);

private:
    enum class Target { Rx, Tx };

    // Re-evaluates m_state against m_desired and fires the matching want*()
    // signal — the only place transitions actually happen. nowMs sets the
    // deadline exactly when (and only when) entering a pending state —
    // never on a repeated request while already pending, or a re-asserted
    // request that resolves to a no-op, or that would let a caller who
    // keeps re-requesting the same thing indefinitely postpone the
    // deadline and defeat tick()'s timeout recovery entirely.
    void evaluate(qint64 nowMs);
    void setState(State s);

    State m_state{State::Idle};
    Target m_desired{Target::Rx};
    qint64 m_timeoutMs{2000};
    qint64 m_deadlineMs{0};  // valid only while m_state is TxPending/RxPending
};

} // namespace AetherSDR::hackrf
