// TUNE power applied live while TUNE is keyed, and the transmit tail
// (upstream fe1fec51). Socket-free: the backend's own MetisClient is never
// started, so nothing reaches a radio. Reads the drive register the backend
// last handed to MetisClient and the tail state, at named instants.
//
//   1. setTunePower() while a TUNE carrier is up moves the drive register to
//      the new tune level and leaves the operator's RF power alone.
//   2. setTunePower() with no carrier changes nothing: the next setTune()
//      carries the value.
//   3. The unkey that ends TUNE arms the tail. Until it drains the drive stays
//      at the tune level (the radio is still radiating), and an RF Power change
//      meanwhile is deferred, not written under the tail.
//   4. The tail drains to the operator's RF power, once.
//   5. A re-key inside the tail cancels it: the carrier continues.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <cstdio>

namespace AetherSDR::hl2 {
struct Hl2TuneTailTestAccess {
    static int drive(const Hl2Backend& b) { return b.m_txDriveRegister; }
    static bool tailPending(const Hl2Backend& b) { return b.m_txTailPending; }
    static bool driveOwed(const Hl2Backend& b) { return b.m_rfDriveOwed; }
    static void parkTail(Hl2Backend& b) { b.m_txTailHoldMs = 60 * 60 * 1000; }
    static void expireTail(Hl2Backend& b) { b.finishTxTail(); }
    static void allowTx(Hl2Backend& b) { b.m_txAllowed = true; }
};
} // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2TuneTailTestAccess;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static int reg(int percent) { return percent * hl2::kTxDriveMax / 100; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {
        Hl2Backend b;
        Access::allowTx(b);
        Access::parkTail(b);
        check(b.capabilities().transmitDriveControl
                  && b.capabilities().transmitDriveControl->tunePowerAppliesLive,
              "HL2 declares tunePowerAppliesLive");
        b.setTxPower(80);
        check(Access::drive(b) == reg(80), "RF power 80 % reaches the drive register");

        b.setTunePower(30);
        check(Access::drive(b) == reg(80), "no carrier: setTunePower changes nothing");

        b.setTune(true, 10);
        check(Access::drive(b) == reg(10), "TUNE keys at its own 10 %, not RF power");
        b.setTunePower(25);
        check(Access::drive(b) == reg(25), "live: TUNE power 25 % reaches the carrier");

        b.setTune(false, -1);
        check(Access::tailPending(b), "the unkey that ends TUNE arms the tail");
        check(Access::drive(b) == reg(25), "under the tail the drive stays at the tune level");
        b.setTxPower(60);
        check(Access::drive(b) == reg(25) && Access::driveOwed(b),
              "RF power moved under the tail is owed, not written");

        Access::expireTail(b);
        check(!Access::tailPending(b) && !Access::driveOwed(b),
              "the tail drains and the drive is no longer owed");
        check(Access::drive(b) == reg(60), "after the tail the drive is the operator's 60 %");

        b.setTxPower(70);
        check(Access::drive(b) == reg(70), "after the tail RF power applies at once");
    }

    {
        Hl2Backend b;
        Access::allowTx(b);
        Access::parkTail(b);
        b.setTxPower(50);
        b.setTune(true, 10);
        b.setTune(false, -1);
        check(Access::tailPending(b), "re-key: tail armed");
        b.setKeying(true);
        check(!Access::tailPending(b), "re-key inside the tail cancels it");
        check(Access::drive(b) == reg(10), "the drive is untouched by the cancelled tail");
        b.setKeying(false);
        Access::expireTail(b);
        check(Access::drive(b) == reg(50), "the next unkey's tail still restores RF power");
    }

    if (g_failures == 0)
        std::printf("hl2_tune_tail_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
