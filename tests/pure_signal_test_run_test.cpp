// The PureSignal window's test signal. This decides when a transmitter keys, so
// every way out of a run is here: each one must end unkeyed.

#include "core/PureSignalTestRun.h"

#include <cstdio>

using namespace AetherSDR;
using Run = PureSignalTestRun;
using Command = Run::Command;
using Mode = Run::Mode;
using Phase = Run::Phase;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static Run::Config config(Mode mode, int seconds = 10, const QString& callsign = {})
{
    Run::Config c;
    c.mode = mode;
    c.seconds = seconds;
    c.psOn = true;
    c.callsign = callsign;
    return c;
}

static Run::Inputs quiet(bool keyed = false)
{
    Run::Inputs in;
    in.levelDbm = -100.0f;
    in.keyed = keyed;
    return in;
}

// A run into its transmit phase, at t = kListenMs.
static QVector<Command> throughListen(Run& run, const Run::Config& c)
{
    run.start(c, 0);
    run.tick(100, quiet());
    return run.tick(Run::kListenMs, quiet());
}

int main()
{
    // Listening first, and nothing keys until it's over.
    {
        Run run;
        check(run.start(config(Mode::TwoTone), 0).isEmpty(), "start keys nothing");
        check(run.phase() == Phase::Listening, "starts by listening");
        check(run.tick(Run::kListenMs - 1, quiet()).isEmpty(), "nothing keys while listening");
        const auto keyed = run.tick(Run::kListenMs, quiet());
        check(keyed == QVector<Command>{Command::KeyTwoTone}, "two-tone keys after listening");
        check(run.phase() == Phase::Transmitting, "transmitting");
    }

    // A busy frequency is never keyed, and a peak anywhere in the listen counts.
    {
        Run run;
        run.start(config(Mode::TwoTone), 0);
        Run::Inputs busy = quiet();
        busy.levelDbm = Run::kBusyDbm + 1.0f;
        run.tick(100, busy);
        check(run.tick(Run::kListenMs, quiet()).isEmpty(), "busy: nothing keyed");
        check(run.phase() == Phase::Finished && !run.succeeded(), "busy: refused");
        check(run.result().contains(QStringLiteral("busy")), "busy: says so");
    }

    // No reading is not a clear frequency.
    {
        Run run;
        run.start(config(Mode::TwoTone), 0);
        check(run.tick(Run::kListenMs, Run::Inputs{}).isEmpty(), "no level: nothing keyed");
        check(run.phase() == Phase::Finished && !run.succeeded(), "no level: refused");
    }

    // PS has to be on to calibrate or compare, but not for a plain signal.
    {
        Run::Config c = config(Mode::Calibrate);
        c.psOn = false;
        Run run;
        check(run.start(c, 0).isEmpty(), "PS off: nothing keyed");
        check(run.phase() == Phase::Finished && !run.succeeded(), "calibrate needs PS");
        c.mode = Mode::AB;
        Run ab;
        ab.start(c, 0);
        check(ab.phase() == Phase::Finished, "A/B needs PS");
        c.mode = Mode::TwoTone;
        Run tone;
        tone.start(c, 0);
        check(tone.phase() == Phase::Listening, "two-tone doesn't");
    }

    // Asked to identify with no callsign: refused before anything keys.
    {
        Run::Config c = config(Mode::TwoTone);
        c.identify = true;
        Run run;
        check(run.start(c, 0).isEmpty(), "no callsign: nothing keyed");
        check(run.phase() == Phase::Finished && !run.succeeded(), "no callsign: refused");
        check(run.result().contains(QStringLiteral("callsign")), "no callsign: says so");
    }

    // The time limit holds whatever is asked for.
    {
        Run run;
        const auto c = config(Mode::TwoTone, 100000);
        throughListen(run, c);
        check(run.tick(Run::kListenMs + Run::kMaxSeconds * 1000 - 1, quiet(true)).isEmpty(),
              "keyed up to the limit");
        const auto end = run.tick(Run::kListenMs + Run::kMaxSeconds * 1000, quiet(true));
        check(end == QVector<Command>{Command::Unkey}, "unkeys at the limit");
        check(run.phase() == Phase::Finished && run.succeeded(), "finished");
    }

    // A single tone is a carrier.
    {
        Run run;
        const auto keyed = throughListen(run, config(Mode::SingleTone, 5));
        check(keyed == QVector<Command>{Command::KeyCarrier}, "single tone keys a carrier");
        check(run.tick(Run::kListenMs + 5000, quiet(true)) == QVector<Command>{Command::Unkey},
              "and unkeys");
    }

    // Calibrate ends when PS is correcting and has settled, not at the first success.
    {
        Run run;
        run.start(config(Mode::Calibrate, 20), 0);
        Run::Inputs in = quiet();
        in.calibrations = 5;
        run.tick(Run::kListenMs, in);
        in.keyed = true;
        in.correcting = true;
        in.calibrations = 5 + Run::kCalibrationsWanted - 1;
        check(run.tick(Run::kListenMs + 2000, in).isEmpty(), "not settled yet");
        in.calibrations = 5 + Run::kCalibrationsWanted;
        check(run.tick(Run::kListenMs + 3000, in) == QVector<Command>{Command::Unkey},
              "unkeys once settled");
        check(run.succeeded(), "calibrated");
    }
    {
        Run run;
        throughListen(run, config(Mode::Calibrate, 5));
        check(run.tick(Run::kListenMs + 5000, quiet(true)) == QVector<Command>{Command::Unkey},
              "calibrate unkeys at the limit");
        check(!run.succeeded(), "and reports it didn't calibrate");
    }

    // A/B flips the correction on a steady signal and always ends with it back on.
    {
        Run run;
        const auto keyed = throughListen(run, config(Mode::AB, 7));
        check(keyed == QVector<Command>{Command::KeyTwoTone}, "A/B keys two-tone, corrected");
        const int t0 = Run::kListenMs;
        check(run.tick(t0 + Run::kAbSwitchMs - 1, quiet(true)).isEmpty(), "holds");
        check(run.tick(t0 + Run::kAbSwitchMs, quiet(true))
                  == QVector<Command>{Command::BypassCorrection}, "first flip is off");
        check(run.tick(t0 + 2 * Run::kAbSwitchMs, quiet(true))
                  == QVector<Command>{Command::UseCorrection}, "second flip is on");
        run.tick(t0 + 2 * Run::kAbSwitchMs + 1, quiet(true));
        Run other;
        throughListen(other, config(Mode::AB, 5));
        other.tick(t0 + Run::kAbSwitchMs, quiet(true));
        check(other.tick(t0 + 5000, quiet(true))
                  == (QVector<Command>{Command::UseCorrection, Command::Unkey}),
              "ending bypassed restores the correction");
    }

    // Stop is immediate from anywhere.
    {
        Run run;
        run.start(config(Mode::TwoTone), 0);
        check(run.stop(100).isEmpty(), "stop while listening keys and unkeys nothing");
        check(run.phase() == Phase::Finished, "stopped");
    }
    {
        Run run;
        throughListen(run, config(Mode::TwoTone));
        check(run.stop(Run::kListenMs + 1) == QVector<Command>{Command::Unkey}, "stop unkeys");
        check(run.tick(Run::kListenMs + 2, quiet()).isEmpty(), "and nothing keys afterwards");
    }
    {
        Run run;
        throughListen(run, config(Mode::AB));
        run.tick(Run::kListenMs + Run::kAbSwitchMs, quiet(true));
        check(run.stop(Run::kListenMs + Run::kAbSwitchMs + 1)
                  == (QVector<Command>{Command::UseCorrection, Command::Unkey}),
              "stop restores the correction");
    }

    // Something else unkeyed the radio: the run is over, it doesn't key again.
    {
        Run run;
        throughListen(run, config(Mode::TwoTone));
        check(run.tick(Run::kListenMs + 100, quiet(false)).isEmpty(),
              "keying gets time to take effect");
        check(run.tick(Run::kListenMs + Run::kKeyConfirmMs, quiet(false))
                  == QVector<Command>{Command::Unkey}, "lost key: ends");
        check(run.phase() == Phase::Finished && !run.succeeded(), "lost key: reported");
    }

    // The CW ID follows the signal, as keying the carrier, and stop cuts it short.
    {
        Run run;
        Run::Config c = config(Mode::TwoTone, 5, QStringLiteral("t"));
        c.identify = true;
        throughListen(run, c);
        const int off = Run::kListenMs + 5000;
        check(run.tick(off, quiet(true)) == QVector<Command>{Command::Unkey}, "signal ends first");
        check(run.phase() == Phase::Identifying, "then the ID");
        check(run.tick(off + Run::kIdGapMs - 1, quiet()).isEmpty(), "after a gap");
        const int id = off + Run::kIdGapMs;
        check(run.tick(id, quiet()) == QVector<Command>{Command::KeyCarrier}, "T: key down");
        check(run.tick(id + 3 * Run::kDitMs - 1, quiet(true)).isEmpty(), "a dah long");
        check(run.tick(id + 3 * Run::kDitMs, quiet(true)) == QVector<Command>{Command::Unkey}, "up");
        check(run.phase() == Phase::Finished && run.succeeded(), "done after the last element");
    }
    {
        Run run;
        Run::Config c = config(Mode::TwoTone, 5, QStringLiteral("W5TSU"));
        c.identify = true;
        throughListen(run, c);
        run.tick(Run::kListenMs + 5000, quiet(true));
        run.tick(Run::kListenMs + 5000 + Run::kIdGapMs, quiet());
        check(run.stop(Run::kListenMs + 5000 + Run::kIdGapMs + 1)
                  == QVector<Command>{Command::Unkey}, "stop during the ID unkeys");
        check(run.phase() == Phase::Finished, "and ends");
    }

    // The ID's timing: W is dit dah dah at 12 WPM.
    {
        const auto w = PureSignalTestRun::morseKeying(QStringLiteral("W"), 100);
        const QVector<PureSignalTestRun::KeyEdge> want{{0, true}, {100, false}, {200, true},
                                                       {500, false}, {600, true}, {900, false}};
        check(w.size() == want.size(), "W has six edges");
        bool same = w.size() == want.size();
        for (qsizetype i = 0; same && i < w.size(); ++i)
            same = w[i].atMs == want[i].atMs && w[i].down == want[i].down;
        check(same, "W's timing");
        const auto two = PureSignalTestRun::morseKeying(QStringLiteral("TT"), 100);
        check(two.size() == 4 && two[2].atMs == 300 + 300, "three dits between letters");
        check(PureSignalTestRun::morseKeying(QStringLiteral("?"), 100).isEmpty(),
              "what can't be sent isn't");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("pure_signal_test_run_test: pass");
    return 0;
}
