#include "HackRfTxRxArbiter.h"

namespace AetherSDR::hackrf {

HackRfTxRxArbiter::HackRfTxRxArbiter(QObject* parent)
    : QObject(parent)
{
}

void HackRfTxRxArbiter::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(m_state);
}

void HackRfTxRxArbiter::requestRx(qint64 nowMs)
{
    m_desired = Target::Rx;
    evaluate(nowMs);
}

void HackRfTxRxArbiter::requestTx(qint64 nowMs)
{
    m_desired = Target::Tx;
    evaluate(nowMs);
}

void HackRfTxRxArbiter::confirmRxStopped(qint64 nowMs)
{
    if (m_state != State::TxPending) return;  // stale or unexpected — ignore
    if (m_desired == Target::Tx) {
        setState(State::TxStreaming);
        emit wantTxStart();
    } else {
        // Changed their mind while the teardown was in flight — RX is what's
        // wanted now, and RX is exactly what just finished stopping, so
        // restart it directly rather than proceeding into TX.
        setState(State::RxStreaming);
        emit wantRxStart();
    }
    Q_UNUSED(nowMs);
}

void HackRfTxRxArbiter::confirmTxStopped(qint64 nowMs)
{
    if (m_state != State::RxPending) return;
    if (m_desired == Target::Rx) {
        setState(State::RxStreaming);
        emit wantRxStart();
    } else {
        setState(State::TxStreaming);
        emit wantTxStart();
    }
    Q_UNUSED(nowMs);
}

void HackRfTxRxArbiter::tick(qint64 nowMs)
{
    if (m_state != State::TxPending && m_state != State::RxPending) return;
    if (nowMs < m_deadlineMs) return;

    const State pending = m_state;
    // Force back to Idle, never to the state that was waited on — an
    // unconfirmed teardown must not be assumed to have succeeded, and Idle
    // is the one state every subsequent request*() knows how to start
    // fresh from (see requestRx()/requestTx()'s Idle case in evaluate()).
    setState(State::Idle);
    emit arbitrationTimedOut(pending);
}

void HackRfTxRxArbiter::evaluate(qint64 nowMs)
{
    switch (m_state) {
    case State::Idle:
        if (m_desired == Target::Rx) {
            setState(State::RxStreaming);
            emit wantRxStart();
        } else {
            // Nothing to tear down (RX was never running) — start directly.
            setState(State::TxStreaming);
            emit wantTxStart();
        }
        break;

    case State::RxStreaming:
        if (m_desired == Target::Tx) {
            // STATE BEFORE THE EMIT, everywhere in this file. HackRfBackend
            // answers synchronously: its wantRxStop handler stops RX and calls
            // confirmRxStopped() before this emit returns. Emitting first left
            // that confirm looking at RxStreaming, where it is ignored as stale,
            // and the arbiter then waited in TxPending for a confirm that had
            // already come: keying never started TX (no TX LED, no output).
            m_deadlineMs = nowMs + m_timeoutMs;  // set exactly on entry — see the .h comment
            setState(State::TxPending);
            emit wantRxStop();
        }
        // else: already RX and RX is still wanted — no-op.
        break;

    case State::TxStreaming:
        if (m_desired == Target::Rx) {
            m_deadlineMs = nowMs + m_timeoutMs;
            setState(State::RxPending);
            emit wantTxStop();
        }
        break;

    case State::TxPending:
    case State::RxPending:
        // A transition is already in flight. Do nothing here — when it
        // resolves, confirmRxStopped()/confirmTxStopped() re-checks
        // m_desired at that moment, which is what makes a request that
        // arrives mid-transition (a PTT bounce) safe without restarting or
        // duplicating the in-flight teardown. The deadline set when this
        // pending state was entered is deliberately left untouched — a
        // caller re-requesting the same thing must not be able to postpone
        // tick()'s timeout indefinitely.
        break;
    }
}

} // namespace AetherSDR::hackrf
