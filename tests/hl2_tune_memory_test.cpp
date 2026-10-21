// The Hermes-Lite 2 remembers TUNE power per band (upstream 4e547347). The radio
// stores no settings, so the backend keeps it beside the per-band drive, with
// the same first-set baseline for bands never tuned. Socket-free: the backend's
// own MetisClient is never started and nothing is keyed on a radio.
//
//   1. Each band keeps its own TUNE power; returning to a band echoes it.
//   2. The capture holds the per-band map and the baseline, and a restored
//      backend echoes the remembered value on entering that band.
//   3. Under a TUNE carrier a band change echoes nothing; the carrier runs at
//      the slider's value. At unkey the new band's own TUNE power applies.

#include "core/RadioStateMemory.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/hl2/Hl2Backend.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QList>

#include <cstdio>

namespace AetherSDR::hl2 {
struct Hl2TuneMemoryTestAccess {
    static void setTuning(Hl2Backend& b, bool on) { b.m_tuning = on; }
};
} // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2TuneMemoryTestAccess;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2Backend backend;
    QList<int> echoed;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
                     [&echoed](const TransmitDelta& d) {
                         if (d.tunePower) echoed << *d.tunePower;
                     });
    backend.setSliceFrequency(0, 7'100'000.0);
    backend.setTunePower(30);
    backend.setSliceFrequency(0, 14'200'000.0);
    check(echoed.isEmpty(),
          "a new band starts on the first-set baseline, which is the value already held");
    backend.setTunePower(55);
    backend.setSliceFrequency(0, 7'150'000.0);
    check(!echoed.isEmpty() && echoed.last() == 30, "back on 40 m, its own TUNE power is echoed");

    const QJsonObject tx = backend.currentOperatingState().extension
                               .value(QStringLiteral("txSetpoints")).toObject();
    const QJsonObject byBand = tx.value(QStringLiteral("tuneByBand")).toObject();
    check(byBand.value(QStringLiteral("40m")).toInt() == 30
              && byBand.value(QStringLiteral("20m")).toInt() == 55,
          "the capture holds TUNE power per band");
    check(tx.value(QStringLiteral("tuneDefaultPercent")).toInt() == 30,
          "the first value set is the baseline for bands never tuned");

    Hl2Backend next;
    QList<int> restored;
    QObject::connect(&next, &IRadioBackend::transmitChanged, &next,
                     [&restored](const TransmitDelta& d) {
                         if (d.tunePower) restored << *d.tunePower;
                     });
    RestoredRadioState state;
    state.extensionSchemaVersion = 1;
    state.extension.insert(QStringLiteral("txSetpoints"), tx);
    next.applyRestoredState(state);
    next.setSliceFrequency(0, 14'250'000.0);
    check(!restored.isEmpty() && restored.last() == 55,
          "a restored radio entering 20 m echoes 20 m's TUNE power");

    // Under a carrier the drive is the slider's value: no band memory moves it.
    restored.clear();
    Access::setTuning(next, true);
    next.setSliceFrequency(0, 7'100'000.0);
    check(restored.isEmpty(), "a band change under a TUNE carrier echoes no other TUNE power");
    next.setTune(false, -1);
    check(!restored.isEmpty() && restored.last() == 30,
          "at unkey the new band's own TUNE power applies");

    if (g_failures) {
        std::printf("hl2_tune_memory_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hl2_tune_memory_test: all checks passed\n");
    return 0;
}
