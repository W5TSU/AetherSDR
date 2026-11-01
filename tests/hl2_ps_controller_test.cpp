// Hl2PsController: what the operator sees, when correction must stop, and the
// receive gain. Snapshots are built by hand; no WDSP, no radio.

#include "core/backends/hl2/Hl2PsController.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR::hl2;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

// A snapshot after `cals` calibrations, all good, at this feedback level.
Hl2PsSnapshot snap(int level, int cals, bool correcting = true)
{
    Hl2PsSnapshot s;
    s.info[4] = level;
    s.info[5] = cals;
    s.info[7] = cals;
    s.info[14] = correcting ? 1 : 0;
    s.feedbackPeakDbfs = -17.0;
    s.referencePeakDbfs = -12.4;
    return s;
}

Hl2PsController enabledAt(int gain)
{
    Hl2PsController c;
    c.setEnabled(true);
    c.setGainDb(gain);
    c.tick(snap(0, 0, false), true);
    return c;
}

}  // namespace

int main()
{
    std::printf("\n  Hl2PsController\n\n");

    {
        Hl2PsController c;
        check(c.state() == Hl2PsState::Off, "starts off");
        check(c.gainDb() == Hl2PsController::kMinGainDb, "starts at minimum gain");
        check(!c.tick(snap(200, 5), true).newGainDb, "off: a tick does nothing");
        check(hl2PsStateName(Hl2PsState::FeedbackWeak) == QStringLiteral("feedback too weak"),
              "state names read as the operator sees them");
        const Hl2PsActions on = c.setEnabled(true);
        check(on.resume && !on.reset, "enable asks for calibration");
        check(c.state() == Hl2PsState::Idle, "enabled and unkeyed: idle");
    }
    {
        Hl2PsController c;
        const Hl2PsActions on = c.setEnabled(true, true);
        check(!on.resume && !on.reset, "enable over a restored curve: no calibration asked");
    }
    {
        Hl2PsController c = enabledAt(0);
        const Hl2PsActions a = c.tick(snap(200, 1), true);
        const int expected = -static_cast<int>(std::lround(20.0 * std::log10(200.0 / 152.293)));
        check(a.newGainDb == expected && a.reset && !a.resume,
              "level 200 at gain 0: gain falls by the computed step, with a reset");
        check(c.gainDb() == expected, "the controller holds the new gain");
        const Hl2PsActions next = c.tick(snap(200, 1), true);
        check(next.resume && !next.reset && !next.newGainDb, "the next tick resumes");
    }
    {
        // Seen on the radio: the first calibration of each transmission read 143,
        // the next 166. Chasing those reset correction on every key-down.
        Hl2PsController d = enabledAt(0);
        check(!d.tick(snap(143, 1), true).reset, "level 143: in the green band, left alone");
        check(!d.tick(snap(166, 2), true).reset, "level 166: left alone too");
        check(!d.tick(snap(129, 3), true).reset && !d.tick(snap(181, 4), true).reset,
              "the band's edges are left alone");
        check(d.tick(snap(182, 5), true).newGainDb == -2, "182 aims back at 152: down 2 dB");
    }
    {
        Hl2PsController c = enabledAt(0);
        const Hl2PsActions a = c.tick(snap(152, 1), true);
        check(!a.newGainDb && !a.reset && !a.resume, "level 152: nothing changes");
        check(c.state() == Hl2PsState::Correcting, "and it reads correcting");
    }
    {
        Hl2PsController c = enabledAt(0);
        check(c.tick(snap(300, 1), true).newGainDb == -12, "level 300 at 0: steps down 15, clamped");
        Hl2PsController d = enabledAt(40);
        check(d.tick(snap(10, 1), true).newGainDb == 48, "level 10 at 40: steps up 15, clamped");
        Hl2PsController e = enabledAt(48);
        const Hl2PsActions a = e.tick(snap(10, 1, false), true);
        check(!a.newGainDb && !a.reset, "at the limit already: no change and no reset");
    }
    {
        Hl2PsController c = enabledAt(0);
        c.tick(snap(152, 1), true);
        const Hl2PsActions a = c.tick(snap(200, 1), true);
        check(!a.newGainDb, "no new calibration: the gain does not move");
    }
    {
        // The sampler cable pulled while correcting: gone from the peaks at once.
        Hl2PsController c = enabledAt(0);
        c.tick(snap(152, 1), true);
        Hl2PsSnapshot s = snap(152, 1);
        s.feedbackPeakDbfs = -60.0;
        const Hl2PsActions a = c.tick(s, true);
        check(a.reset && !a.resume, "feedback 40 dB under the reference: reset at once");
        check(c.state() == Hl2PsState::FeedbackWeak, "and it reads too weak");
        check(!c.tick(s, true).resume, "and it stays down while keyed");
        check(c.tick(s, false).reset == false, "unkeying changes nothing");
        check(c.state() == Hl2PsState::FeedbackWeak, "the fault still shows after unkey");
        const Hl2PsActions again = c.tick(snap(152, 1, false), true);
        check(again.resume, "the next keying tries again");
        check(c.state() != Hl2PsState::FeedbackWeak, "and the fault clears");
    }
    {
        // A pause in speech: no reference, so a quiet feedback means nothing.
        Hl2PsController c = enabledAt(0);
        c.tick(snap(152, 1), true);
        Hl2PsSnapshot s = snap(152, 1);
        s.feedbackPeakDbfs = -90.0;
        s.referencePeakDbfs = -80.0;
        check(!c.tick(s, true).reset, "both quiet (a pause): no fault");
        s.feedbackPeakDbfs.reset();
        s.referencePeakDbfs.reset();
        check(!c.tick(s, true).reset, "no peaks at all: no fault");
    }
    {
        Hl2PsController c = enabledAt(0);
        c.tick(snap(152, 1), true);
        Hl2PsSnapshot s = snap(152, 2);
        s.info[2] = 1;
        s.info[5] = 1;
        check(!c.tick(s, true).reset, "one failed fit while correcting: no reset");
        s.info[7] = 3;
        check(!c.tick(s, true).reset, "two: no reset");
        s.info[7] = 4;
        const Hl2PsActions a = c.tick(s, true);
        check(a.reset && c.state() == Hl2PsState::FeedbackWeak, "three in a row: reset");
    }
    {
        Hl2PsController c = enabledAt(0);
        c.tick(snap(152, 1), true);
        Hl2PsSnapshot s = snap(152, 1);
        s.feedbackPeakDbfs = -0.5;
        const Hl2PsActions a = c.tick(s, true);
        check(a.reset && a.newGainDb == -12, "clipping: gain steps down 15 and resets");

        Hl2PsController d = enabledAt(-12);
        d.tick(snap(152, 1), true);
        const Hl2PsActions at = d.tick(s, true);
        check(at.reset && !at.newGainDb && d.state() == Hl2PsState::FeedbackStrong,
              "clipping at minimum gain: reset, reads too strong");
    }
    {
        Hl2PsController c = enabledAt(48);
        Hl2PsSnapshot s = snap(10, 0, false);
        for (int i = 1; i <= 20; ++i) {
            s.info[7] = i;
            c.tick(s, true);
        }
        check(c.state() == Hl2PsState::FeedbackWeak, "twenty failed attempts on a weak level: too weak");
        check(c.tick(s, true).resume == false, "correction stays off");

        Hl2PsController d = enabledAt(-12);
        Hl2PsSnapshot t = snap(400, 0, false);
        for (int i = 1; i <= 20; ++i) {
            t.info[7] = i;
            d.tick(t, true);
        }
        check(d.state() == Hl2PsState::FeedbackStrong, "twenty on a strong level: too strong");
    }
    {
        Hl2PsController c = enabledAt(0);
        c.tick(snap(152, 3), true);
        c.tick(snap(152, 3), false);
        check(c.state() == Hl2PsState::Correcting, "unkeyed with a curve loaded: holds correcting");
        check(!c.tick(snap(200, 3), false).newGainDb, "unkeyed: nothing moves");

        c.requestRecalibration();
        check(!c.tick(snap(152, 3), false).resume, "recalibration waits for keying");
        check(c.tick(snap(152, 3), true).resume, "keyed: it resumes");
    }
    {
        Hl2PsController c = enabledAt(0);
        c.tick(snap(152, 1), true);
        const Hl2PsActions off = c.setEnabled(false);
        check(off.reset && !off.resume && c.state() == Hl2PsState::Off, "disable resets, from anywhere");
        check(!c.tick(snap(10, 9), true).reset, "and stays quiet");
    }
    {
        Hl2PsController c;
        c.setGainDb(100);
        check(c.gainDb() == 48, "a stored gain is clamped high");
        c.setGainDb(-100);
        check(c.gainDb() == -12, "and low");
    }

    std::printf("\n  %s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
