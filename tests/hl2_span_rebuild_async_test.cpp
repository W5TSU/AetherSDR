// Zooming the HL2 panadapter across a decimation-rate boundary re-spans the
// radio and rebuilds every receiver's WDSP channel. That rebuild used to run on
// the GUI thread through a blocking cross-thread call — the "chunky zoom"
// freeze (docs/HERMES.md §22.4, ADR 0001 cost 2). It now hands off to the I/O
// thread the same way connectRadio() does, and a completion handler resumes on
// the GUI thread.
//
// NO RADIO IS NEEDED — the connect comes up on conservative defaults against a
// dead port, which is enough to have a receiver with a real WDSP channel to
// re-span. Everything here is observed through resamplingChanged() and
// panCenterBandwidthChanged().

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/IRadioBackend.h"

#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;

namespace {

int failures = 0;
void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition)
        ++failures;
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

RadioConnectRequest request()
{
    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = 1024;   // nothing is here; discovery times out, defaults win
    return req;
}

const QString kPan = QStringLiteral("hl2-0");

// Tracks resamplingChanged() edges and the last span reported.
struct Observer {
    int trueCount = 0;
    int falseCount = 0;
    bool active = false;
    double lastSpanMhz = 0.0;

    explicit Observer(Hl2Backend& b)
    {
        QObject::connect(&b, &IRadioBackend::resamplingChanged, &b,
                         [this](bool on) { on ? ++trueCount : ++falseCount; active = on; });
        QObject::connect(&b, &IRadioBackend::panCenterBandwidthChanged, &b,
                         [this](const QString&, double, double spanMhz) { lastSpanMhz = spanMhz; });
    }
};

bool spanIs(double mhz, int rateHz)
{
    return std::fabs(mhz - static_cast<double>(rateHz) / 1.0e6) < 1e-6;
}

void connectAndSettle(Hl2Backend& backend)
{
    QSignalSpy finished(&backend, &Hl2Backend::dspSetupFinished);
    backend.connectRadio(request());
    test::awaitDspBuild("hl2_span_rebuild_async_test",
                        [&] { return finished.count() >= 1; });
    spin(50);
}

// A zoom that snaps to the rate the radio is already on is free display scaling.
// It must NOT rebuild anything.
void withinRateZoomDoesNotResample()
{
    TestSettingsProfile profile(QStringLiteral("hl2-span-async-within"));
    Hl2Backend backend;
    Observer obs(backend);
    connectAndSettle(backend);

    // The defaults connect comes up at 48 kHz. 50 kHz snaps back to 48 kHz.
    backend.setPanBandwidth(kPan, 50'000.0);
    spin(200);

    check(obs.trueCount == 0 && obs.falseCount == 0,
          "a within-rate zoom emits no resamplingChanged");
    check(spanIs(obs.lastSpanMhz, 48'000),
          "a within-rate zoom still republishes the granted span");
}

// The headline: the rebuild no longer blocks the caller. setPanBandwidth()
// returns while the rebuild is still in flight — resamplingChanged(true) has
// fired, resamplingChanged(false) has not.
void theRebuildDoesNotBlockTheCaller()
{
    TestSettingsProfile profile(QStringLiteral("hl2-span-async-nonblock"));
    Hl2Backend backend;
    Observer obs(backend);
    connectAndSettle(backend);

    backend.setPanBandwidth(kPan, 200'000.0);   // -> 192 kHz, crosses a boundary
    const bool returnedMidRebuild = (obs.trueCount == 1 && obs.falseCount == 0 && obs.active);
    check(returnedMidRebuild,
          "setPanBandwidth returns while the rebuild is still in flight");

    const bool settled = test::spinUntil([&] { return obs.falseCount >= 1; });
    check(settled, "the rebuild completes");
    check(spanIs(obs.lastSpanMhz, 192'000), "and the panadapter lands on the new span");
}

// A second span request landing while a rebuild is in flight must not be lost:
// the radio ends on the LAST span asked for, not an intermediate one.
void theLastSpanWins()
{
    TestSettingsProfile profile(QStringLiteral("hl2-span-async-latest"));
    Hl2Backend backend;
    Observer obs(backend);
    connectAndSettle(backend);

    backend.setPanBandwidth(kPan, 200'000.0);   // -> 192 kHz
    spin(200);                                                  // clear the 150 ms sweep throttle
    backend.setPanBandwidth(kPan, 400'000.0);   // -> 384 kHz, while 192 may still be building

    const bool settled = test::spinUntil(
        [&] { return obs.falseCount >= 1 && spanIs(obs.lastSpanMhz, 384'000); });
    check(settled, "the radio settles on the last requested span");
    check(!spanIs(obs.lastSpanMhz, 192'000),
          "it does not come to rest on the superseded span");
    // The two requests produce at most two rebuilds (one per distinct target
    // rate — the 384 kHz one is queued behind the 192 kHz one, not served as a
    // third concurrent task), and every rebuild's true edge has its false edge.
    spin(200);
    check(obs.trueCount == obs.falseCount && obs.trueCount <= 2,
          "rebuilds are serialised and edge-balanced, not stacked");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    withinRateZoomDoesNotResample();
    theRebuildDoesNotBlockTheCaller();
    theLastSpanWins();

    if (failures) {
        std::fprintf(stderr, "hl2_span_rebuild_async_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stderr, "hl2_span_rebuild_async_test: all checks passed\n");
    return 0;
}
