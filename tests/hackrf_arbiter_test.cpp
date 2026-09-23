#include "core/backends/hackrf/HackRfTxRxArbiter.h"

#include <QCoreApplication>
#include <QObject>
#include <cstdio>

using namespace AetherSDR::hackrf;
using State = HackRfTxRxArbiter::State;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

void expectTrue(const char* name, bool ok) { report(name, ok); }

const char* stateName(State s)
{
    switch (s) {
        case State::Idle:        return "Idle";
        case State::RxStreaming: return "RxStreaming";
        case State::TxPending:   return "TxPending";
        case State::TxStreaming: return "TxStreaming";
        case State::RxPending:   return "RxPending";
    }
    return "?";
}

void expectState(const char* name, State got, State want)
{
    if (got == want) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %s, want %s\n", name, stateName(got), stateName(want));
        ++g_failed;
    }
}

// Records every want*()/timeout signal the arbiter fires, in order, so a
// test can assert exactly what happened rather than just the end state.
struct Recorder : public QObject {
    Q_OBJECT
public:
    explicit Recorder(HackRfTxRxArbiter& a)
    {
        connect(&a, &HackRfTxRxArbiter::wantRxStart, this, [this] { log.append("rxStart"); });
        connect(&a, &HackRfTxRxArbiter::wantRxStop,  this, [this] { log.append("rxStop"); });
        connect(&a, &HackRfTxRxArbiter::wantTxStart, this, [this] { log.append("txStart"); });
        connect(&a, &HackRfTxRxArbiter::wantTxStop,  this, [this] { log.append("txStop"); });
        connect(&a, &HackRfTxRxArbiter::arbitrationTimedOut, this,
                [this](State s) { log.append(QString("timeout(%1)").arg(stateName(s))); });
    }
    QStringList log;
};

// ── Basic transitions ───────────────────────────────────────────────────

void testInitialStateIsIdle()
{
    HackRfTxRxArbiter a;
    expectState("starts Idle", a.state(), State::Idle);
}

void testRequestRxFromIdle()
{
    HackRfTxRxArbiter a;
    Recorder rec(a);
    a.requestRx(0);
    expectState("Idle + requestRx -> RxStreaming", a.state(), State::RxStreaming);
    expectTrue("emits only rxStart", rec.log == QStringList{"rxStart"});
}

void testRequestTxFromIdle()
{
    // Nothing to tear down (RX never started) — TX starts directly.
    HackRfTxRxArbiter a;
    Recorder rec(a);
    a.requestTx(0);
    expectState("Idle + requestTx -> TxStreaming directly", a.state(), State::TxStreaming);
    expectTrue("emits only txStart", rec.log == QStringList{"txStart"});
}

void testRxToTxGoesThroughPending()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    Recorder rec(a);  // attach after the initial RX so we only see the TX transition
    a.requestTx(100);
    expectState("RxStreaming + requestTx -> TxPending", a.state(), State::TxPending);
    expectTrue("asks to stop RX, does NOT start TX yet", rec.log == QStringList{"rxStop"});
}

void testConfirmRxStoppedCompletesTheTxTransition()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    a.requestTx(100);
    Recorder rec(a);
    a.confirmRxStopped(150);
    expectState("confirmRxStopped -> TxStreaming", a.state(), State::TxStreaming);
    expectTrue("starts TX", rec.log == QStringList{"txStart"});
}

void testTxToRxGoesThroughPending()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    a.requestTx(100);
    a.confirmRxStopped(150);  // now TxStreaming
    Recorder rec(a);
    a.requestRx(200);
    expectState("TxStreaming + requestRx -> RxPending", a.state(), State::RxPending);
    expectTrue("asks to stop TX, does NOT start RX yet", rec.log == QStringList{"txStop"});
}

void testConfirmTxStoppedCompletesTheRxTransition()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    a.requestTx(100);
    a.confirmRxStopped(150);
    a.requestRx(200);
    Recorder rec(a);
    a.confirmTxStopped(250);
    expectState("confirmTxStopped -> RxStreaming", a.state(), State::RxStreaming);
    expectTrue("starts RX", rec.log == QStringList{"rxStart"});
}

// ── Idempotence ─────────────────────────────────────────────────────────

void testRequestRxWhileAlreadyRxIsNoOp()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    Recorder rec(a);
    a.requestRx(50);
    expectState("still RxStreaming", a.state(), State::RxStreaming);
    expectTrue("no signal emitted", rec.log.isEmpty());
}

void testRequestTxWhileAlreadyInTxPendingDoesNotRestartTheTransition()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    a.requestTx(100);  // now TxPending
    Recorder rec(a);
    a.requestTx(120);  // same desire, still pending
    expectState("still TxPending", a.state(), State::TxPending);
    expectTrue("does not re-issue rxStop", rec.log.isEmpty());
}

// ── Rapid PTT bounce: the latest desire wins, never a stale one ─────────

void testBounceDuringTxPendingEndsInRxNotTx()
{
    // Operator keys down then releases before the RX teardown even confirms
    // — a short CW dit is exactly this shape. Must never transmit.
    HackRfTxRxArbiter a;
    a.requestRx(0);
    a.requestTx(100);          // TxPending — asked to stop RX
    a.requestRx(110);          // changed their mind before it confirmed
    Recorder rec(a);
    a.confirmRxStopped(150);   // the ORIGINAL teardown finally confirms
    expectState("ends in RxStreaming, not TxStreaming", a.state(), State::RxStreaming);
    expectTrue("starts RX, never started TX", rec.log == QStringList{"rxStart"});
}

void testBounceDuringRxPendingEndsInTxNotRx()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    a.requestTx(100);
    a.confirmRxStopped(150);   // TxStreaming
    a.requestRx(200);          // RxPending — asked to stop TX
    a.requestTx(210);          // changed their mind back before it confirmed
    Recorder rec(a);
    a.confirmTxStopped(250);
    expectState("ends in TxStreaming, not RxStreaming", a.state(), State::TxStreaming);
    expectTrue("starts TX, never started RX", rec.log == QStringList{"txStart"});
}

// ── Stale / unexpected confirmations are ignored ────────────────────────

void testConfirmTxStoppedIgnoredWhenNotPending()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);
    Recorder rec(a);
    a.confirmTxStopped(50);  // nothing was ever pending
    expectState("state unchanged", a.state(), State::RxStreaming);
    expectTrue("no signal emitted", rec.log.isEmpty());
}

void testConfirmRxStoppedIgnoredWhenNotPending()
{
    HackRfTxRxArbiter a;
    Recorder rec(a);
    a.confirmRxStopped(0);  // arbiter is Idle; nothing pending
    expectState("stays Idle", a.state(), State::Idle);
    expectTrue("no signal emitted", rec.log.isEmpty());
}

// ── Timeout recovery ─────────────────────────────────────────────────────

void testTickBeforeDeadlineDoesNothing()
{
    HackRfTxRxArbiter a;
    a.setTimeoutMs(1000);
    a.requestRx(0);
    a.requestTx(100);  // TxPending, deadline = 1100
    Recorder rec(a);
    a.tick(500);
    expectState("still TxPending, well before deadline", a.state(), State::TxPending);
    expectTrue("no timeout fired", rec.log.isEmpty());
}

void testTickAtDeadlineForcesRecoveryToIdle()
{
    HackRfTxRxArbiter a;
    a.setTimeoutMs(1000);
    a.requestRx(0);
    a.requestTx(100);  // TxPending, deadline = 1100
    Recorder rec(a);
    a.tick(1100);
    expectState("forced back to Idle, never TxStreaming", a.state(), State::Idle);
    expectTrue("fires arbitrationTimedOut(TxPending), starts nothing",
               rec.log == QStringList{"timeout(TxPending)"});
}

void testTimeoutOnRxPendingAlsoRecoversToIdle()
{
    HackRfTxRxArbiter a;
    a.setTimeoutMs(1000);
    a.requestRx(0);
    a.requestTx(100);
    a.confirmRxStopped(150);   // TxStreaming
    a.requestRx(200);          // RxPending, deadline = 1200
    Recorder rec(a);
    a.tick(1200);
    expectState("forced back to Idle", a.state(), State::Idle);
    expectTrue("fires arbitrationTimedOut(RxPending)",
               rec.log == QStringList{"timeout(RxPending)"});
}

void testRecoveryAfterTimeoutStartsCleanly()
{
    HackRfTxRxArbiter a;
    a.setTimeoutMs(1000);
    a.requestRx(0);
    a.requestTx(100);
    a.tick(1100);  // times out -> Idle
    Recorder rec(a);
    a.requestRx(1200);
    expectState("a fresh requestRx after timeout works normally", a.state(), State::RxStreaming);
    expectTrue("starts RX cleanly, no leftover pending state",
               rec.log == QStringList{"rxStart"});
}

void testTickNeverFiresOutsideAPendingState()
{
    HackRfTxRxArbiter a;
    a.requestRx(0);  // RxStreaming — nothing pending
    Recorder rec(a);
    a.tick(1'000'000);  // arbitrarily far in the future
    expectState("still RxStreaming", a.state(), State::RxStreaming);
    expectTrue("no timeout ever fires with nothing pending", rec.log.isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testInitialStateIsIdle();
    testRequestRxFromIdle();
    testRequestTxFromIdle();
    testRxToTxGoesThroughPending();
    testConfirmRxStoppedCompletesTheTxTransition();
    testTxToRxGoesThroughPending();
    testConfirmTxStoppedCompletesTheRxTransition();

    testRequestRxWhileAlreadyRxIsNoOp();
    testRequestTxWhileAlreadyInTxPendingDoesNotRestartTheTransition();

    testBounceDuringTxPendingEndsInRxNotTx();
    testBounceDuringRxPendingEndsInTxNotRx();

    testConfirmTxStoppedIgnoredWhenNotPending();
    testConfirmRxStoppedIgnoredWhenNotPending();

    testTickBeforeDeadlineDoesNothing();
    testTickAtDeadlineForcesRecoveryToIdle();
    testTimeoutOnRxPendingAlsoRecoversToIdle();
    testRecoveryAfterTimeoutStartsCleanly();
    testTickNeverFiresOutsideAPendingState();

    return g_failed == 0 ? 0 : 1;
}

#include "hackrf_arbiter_test.moc"
