// HackRF connect must not freeze the GUI while WDSP plans its FFTs.
//
// REQUIRES A REAL HACKRF, and is opt-in: exits 77 (skipped) unless
// AETHER_HACKRF_HW_TEST=1 is set and a HackRF is attached, so an ordinary ctest
// run on a machine with one plugged in never takes the device.
//
//   AETHER_HACKRF_HW_TEST=1 ./build/hackrf_cold_connect_hw_test
//
// The bug: HackRfBackend::connectRadio opened its RX WDSP channel inline, on the
// GUI thread. With no usable FFTW wisdom (a first run, or a cache written by a
// different FFTW build) that measures FFTW_PATIENT plans for ~30 s, so the app
// froze with "connecting" as the last log line, and an operator who closed it
// never let the cache fill, which made every attempt freeze again. The channel
// is now built on a worker thread and connected() waits for it.
//
// RED-CAPABLE ONLY WITH THE FULL PLANNER. The run uses a fresh, empty wisdom
// directory AND unsets AETHER_WDSP_FFTW_TIMELIMIT, which the suite's isolation
// TU (TestWdspWisdomIsolation.cpp) sets for every test binary, even one run
// directly. Under that bound a cold build takes ~40 ms, so an inline build would
// pass every check here; unbounded it takes ~30 s, so an inline build fails the
// first one. The unbounded plans only ever reach the temporary directory.
#include "core/HackRfDiscovery.h"
#include "core/backends/hackrf/HackRfBackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <optional>

using namespace AetherSDR;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok)
        ++g_failures;
}

constexpr qint64 kMaxConnectCallMs = 500;   // connectRadio() itself
constexpr qint64 kMaxGuiGapMs = 500;        // heartbeat is 50 ms
constexpr int kSetupTimeoutMs = 240'000;    // a slow machine's cold PATIENT build

// Spins the event loop until `done` or the timeout. Returns false on timeout.
template <typename Pred>
bool spinUntil(Pred done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

std::optional<RadioInfo> discoverFirst()
{
    HackRfDiscovery disc;
    std::optional<RadioInfo> found;
    auto take = [&](const RadioInfo& info) { if (!found) found = info; };
    QObject::connect(&disc, &HackRfDiscovery::radioDiscovered, take);
    QObject::connect(&disc, &HackRfDiscovery::radioUpdated, take);
    disc.start(250);
    spinUntil([&] { return found.has_value(); }, 5000);
    disc.stop();
    // Let a scan already in flight finish before `disc` goes away.
    spinUntil([] { return false; }, 600);
    return found;
}

struct Events {
    int progress = 0, finished = 0, connected = 0, disconnected = 0, errors = 0;
};

void watch(hackrf::HackRfBackend& backend, Events& ev)
{
    QObject::connect(&backend, &hackrf::HackRfBackend::dspSetupProgress,
                     [&](const QString&, int, int) { ++ev.progress; });
    QObject::connect(&backend, &hackrf::HackRfBackend::dspSetupFinished, [&] { ++ev.finished; });
    QObject::connect(&backend, &IRadioBackend::connected, [&] { ++ev.connected; });
    QObject::connect(&backend, &IRadioBackend::disconnected, [&] { ++ev.disconnected; });
    QObject::connect(&backend, &IRadioBackend::connectionError, [&](const QString& r) {
        ++ev.errors;
        std::printf("connectionError: %s\n", qPrintable(r));
    });
}

RadioConnectRequest requestFor(const RadioInfo& info)
{
    RadioConnectRequest req;
    req.host = info.address.toString();
    req.port = info.port;
    req.serial = info.serial;
    req.serialIdentity = info.serialIdentity;
    return req;
}

// A fresh empty wisdom directory, so the next channel open is cold.
void useColdWisdomCache(QTemporaryDir& dir)
{
    qputenv("AETHER_WDSP_WISDOM_DIR", dir.path().toUtf8());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (qgetenv("AETHER_HACKRF_HW_TEST") != "1") {
        std::printf("SKIP: set AETHER_HACKRF_HW_TEST=1 with a HackRF attached\n");
        return 77;
    }
    const std::optional<RadioInfo> info = discoverFirst();
    if (!info) {
        std::printf("SKIP: no HackRF found\n");
        return 77;
    }
    std::printf("HackRF %s (%s)\n", qPrintable(info->serial), qPrintable(info->model));

    // WDSP reads the wisdom directory once per process, so one cold cache for
    // the whole run: scenario 1 is the cold build; scenario 2 abandons a build
    // that, by then, reads the plans scenario 1 wrote, which is fine: it
    // checks the abandon path, not the planning time.
    QTemporaryDir cold;
    useColdWisdomCache(cold);
    // Before the first channel open: WdspChannel reads the bound once.
    qunsetenv("AETHER_WDSP_FFTW_TIMELIMIT");

    QElapsedTimer clock;
    clock.start();
    qint64 lastBeat = 0, maxGap = 0;
    QTimer beat;
    beat.setInterval(50);
    QObject::connect(&beat, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        if (lastBeat)
            maxGap = std::max(maxGap, now - lastBeat);
        lastBeat = now;
    });
    beat.start();

    // ---- 1. a cold connect keeps the GUI live and connects once ----
    {
        hackrf::HackRfBackend backend;
        Events ev;
        watch(backend, ev);

        // Baseline BEFORE the call, so a stall inside connectRadio() itself, the
        // original bug, counts as a gap too.
        lastBeat = clock.elapsed();
        maxGap = 0;
        const qint64 t0 = clock.elapsed();
        backend.connectRadio(requestFor(*info));
        const qint64 callMs = clock.elapsed() - t0;
        std::printf("connectRadio() returned in %lld ms\n", static_cast<long long>(callMs));
        check(callMs < kMaxConnectCallMs, "connectRadio() returns without building the channel inline");
        check(ev.progress == 1, "dspSetupProgress announced the setup phase");
        check(ev.connected == 0, "connected() waits for the receive chain");

        const bool connected = spinUntil([&] { return ev.connected > 0 || ev.errors > 0; }, kSetupTimeoutMs);
        std::printf("connected after %lld ms, longest GUI stall %lld ms\n",
                    static_cast<long long>(clock.elapsed() - t0), static_cast<long long>(maxGap));
        check(connected && ev.errors == 0, "the connect completes");
        check(clock.elapsed() - t0 > 2000,
              "the channel build was really cold (else this run proves nothing)");
        check(maxGap < kMaxGuiGapMs, "the GUI thread never stalls while the channel builds");
        check(ev.finished == 1, "dspSetupFinished ends the phase exactly once");
        check(ev.connected == 1, "connected() fires exactly once");
        check(backend.isConnected(), "isConnected() after setup");

        backend.disconnectRadio();
        check(ev.disconnected == 1, "disconnect after a completed connect reports disconnected()");
    }

    // ---- 2. a disconnect during setup abandons the build ----
    {
        hackrf::HackRfBackend backend;
        Events ev;
        watch(backend, ev);

        backend.connectRadio(requestFor(*info));
        backend.disconnectRadio();
        check(ev.finished == 1, "a disconnect during setup ends the setup phase");
        check(ev.disconnected == 1, "a disconnect during setup reports disconnected()");
        check(!backend.isConnected(), "not connected after an abandoned setup");

        // Long enough for the abandoned build to finish and be discarded.
        spinUntil([] { return false; }, 3000);
        check(ev.connected == 0, "the abandoned build never connects");

        backend.connectRadio(requestFor(*info));
        const bool connected = spinUntil([&] { return ev.connected > 0 || ev.errors > 0; }, kSetupTimeoutMs);
        check(connected && ev.connected == 1, "a reconnect after an abandoned setup connects once");
        backend.disconnectRadio();
    }

    std::printf("%s\n", g_failures == 0 ? "hackrf_cold_connect_hw_test: OK" : "hackrf_cold_connect_hw_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
