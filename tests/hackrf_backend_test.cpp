// Unit test for HackRfBackend (#42). Never calls connectRadio() — no
// hardware needed, mirroring rtl_backend_test's own approach — so this
// exercises capabilities() declaration and the restore-state contract only.

#include "core/backends/hackrf/HackRfBackend.h"
#include "core/backends/hackrf/HackRfWorker.h"
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QJsonObject>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

using namespace AetherSDR;

#ifdef AETHER_BACKEND_HACKRF
namespace AetherSDR::hackrf {
// A connected session without a device: enough for the stream-loss path,
// which only flips state and reports (the worker's close() is a no-op while
// nothing is open).
struct HackRfBackendTestAccess {
    static void markConnected(HackRfBackend& b) { b.m_connected = true; }
    static HackRfWorker* worker(HackRfBackend& b) { return b.m_worker.get(); }
};
}  // namespace AetherSDR::hackrf
#endif

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

#ifdef AETHER_BACKEND_HACKRF
    auto backend = std::make_unique<hackrf::HackRfBackend>();
    check(backend != nullptr, "HackRfBackend instantiation");
    check(!backend->isConnected(), "starts disconnected");

    // ── capabilities() ────────────────────────────────────────────────
    const auto caps = backend->capabilities();
    check(caps.family == "hackrf", "capabilities.family is hackrf");
    check(caps.canTransmit, "HackRF can transmit (unlike RTL-SDR)");
    check(caps.hostModulates, "TX audio is modulated on this host (WDSP TXA, once wired)");
    check(caps.takesTxAudioOverSeam, "TX audio reaches this backend via submitTxAudio");
    check(!caps.hasRadioPttReadback, "no readback plane, matches HL2");
    check(caps.maxSlices == 1, "single slice for now (no HackRfDdc yet)");
    check(caps.maxPanadapters == 1, "single panadapter for now");
    check(!caps.canCreateSlices, "cannot create additional slices yet");
    check(caps.tuningMinHz == 1'000'000, "tuning floor is 1 MHz");
    check(caps.tuningMaxHz == 6'000'000'000, "tuning ceiling is 6 GHz");
    check(!caps.sampleRatesHz.isEmpty(), "advertises a sample-rate list");
    check(caps.persistsMemories == false, "HackRF has no radio-side memory");

    // The gain-model resolution this backend makes: VGA -> continuous
    // panRfGain, AMP -> discrete panPreamp, LNA -> the "hackrf" extension
    // namespace. Nothing in RadioCapabilities itself encodes this split (no
    // dedicated capability flag exists for it — see IRadioBackend.h's own
    // panPreampInfoChanged comment), so what's actually checkable here is
    // that the extension namespace is declared for the LNA path.
    check(caps.extensionNamespaces.contains(QStringLiteral("hackrf")),
          "declares the hackrf extension namespace (for LNA gain)");

    // Every mode transmits but broadcast WFM. (v1 was FM/CW only, with SSB
    // and AM receive-only; hackrf_tx_modes_test pins each modulation.)
    for (const char* m : {"FM", "FMN", "DFM", "CW", "CWR", "USB", "LSB", "DIGU", "DIGL",
                          "RTTY", "AM", "SAM", "DSB"})
        check(!caps.receiveOnlyModes.contains(QString::fromLatin1(m)),
              (std::string(m) + " is transmittable").c_str());
    check(caps.receiveOnlyModes == QStringList{QStringLiteral("WFM")}, "WFM is the only receive-only mode");

    // ── applyRestoredState / currentOperatingState round trip ──────────
    RestoredRadioState restoredState;
    restoredState.rfFrequencyHz = 146'520'000.0;  // 2m FM calling frequency
    restoredState.mode = QStringLiteral("fm");    // lowercase on the wire — must canonicalize
    restoredState.filterLowHz = -8'000;
    restoredState.filterHighHz = 8'000;
    restoredState.sampleRateHz = 10'000'000;
    restoredState.agcMode = QStringLiteral("slow");
    restoredState.agcThreshold = 40;
    restoredState.extension[QStringLiteral("rfGain")] = QJsonObject{
        {QStringLiteral("vgaGainDb"), 30},
        {QStringLiteral("lnaGainDb"), 24},
        {QStringLiteral("ampEnabled"), true},
    };
    restoredState.extension[QStringLiteral("panDisplay")] = QJsonObject{
        {QStringLiteral("spanHz"), 2'000'000.0},
        {QStringLiteral("minDbm"), -100.0},
        {QStringLiteral("maxDbm"), -20.0},
        {QStringLiteral("dbmScale"), 1},
    };
    backend->applyRestoredState(restoredState);

    const auto op = backend->currentOperatingState();
    check(op.rfFrequencyHz == 146'520'000.0, "restored frequency round-trips");
    check(op.mode == "FM", "restored mode is canonicalized to uppercase");
    check(op.filterLowHz == -8'000 && op.filterHighHz == 8'000, "restored filter round-trips");
    check(op.sampleRateHz == 8'000'000,
          "the saved hardware rate no longer picks the span (8 MS/s runs every narrow zoom)");
    // AGC runs in WDSP on this host, so the client is the only place the
    // operator's mode and threshold can live. It was never saved: every
    // session started on medium/65 whatever had been chosen.
    check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::Agc),
          "declares the AGC settings domain");
    check(op.agcMode == QStringLiteral("slow"), "restored AGC mode round-trips");
    check(op.agcThreshold == 40, "restored AGC threshold round-trips");
    const auto displayObj = op.extension.value(QStringLiteral("panDisplay")).toObject();
    check(displayObj.value(QStringLiteral("spanHz")).toDouble() == 2'000'000.0,
          "restored span round-trips");
    check(displayObj.value(QStringLiteral("minDbm")).toDouble() == -100.0
              && displayObj.value(QStringLiteral("maxDbm")).toDouble() == -20.0,
          "restored dBm scale round-trips");
    const auto gainObj = op.extension.value(QStringLiteral("rfGain")).toObject();
    check(gainObj.value(QStringLiteral("vgaGainDb")).toInt() == 30, "restored VGA gain round-trips");
    check(gainObj.value(QStringLiteral("lnaGainDb")).toInt() == 24, "restored LNA gain round-trips");
    check(gainObj.value(QStringLiteral("ampEnabled")).toBool() == true, "restored AMP state round-trips");

    // A second applyRestoredState with an EMPTY snapshot must not inherit
    // the previous call's values — this backend object could be reused for
    // a different physical device between sessions (mirrors
    // RtlSdrBackend::applyRestoredState's own documented reset-first rule).
    RestoredRadioState empty;
    backend->applyRestoredState(empty);
    const auto opAfterEmpty = backend->currentOperatingState();
    // A new session shows ~400 kHz (the 500 kHz zoom step) at -40 to -140 dBm.
    {
        check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::PanDisplay),
              "declares the pan display settings domain");
        const auto display = opAfterEmpty.extension.value(QStringLiteral("panDisplay")).toObject();
        check(display.value(QStringLiteral("spanHz")).toDouble() == 500'000.0,
              "the default span is 500 kHz");
        check(display.value(QStringLiteral("minDbm")).toDouble() == -140.0
                  && display.value(QStringLiteral("maxDbm")).toDouble() == -40.0,
              "the default dBm scale is -40 to -140 (calibrated dBm)");
        check(display.value(QStringLiteral("dbmScale")).toInt() == 1,
              "the saved dBm scale is marked as calibrated dBm");
    }

    // A dBm range saved before calibration was raw dBFS: it is not restored.
    {
        RestoredRadioState old;
        old.extension[QStringLiteral("panDisplay")] = QJsonObject{
            {QStringLiteral("spanHz"), 1'000'000.0},
            {QStringLiteral("minDbm"), -80.0},
            {QStringLiteral("maxDbm"), 0.0},
        };
        backend->applyRestoredState(old);
        const auto display = backend->currentOperatingState().extension
                                 .value(QStringLiteral("panDisplay")).toObject();
        check(display.value(QStringLiteral("minDbm")).toDouble() == -140.0,
              "an unmarked (dBFS) dBm range falls back to the calibrated default");
        check(display.value(QStringLiteral("spanHz")).toDouble() == 1'000'000.0,
              "...while its span is still restored");
        backend->applyRestoredState(empty);
    }
    check(opAfterEmpty.rfFrequencyHz == 100'000'000.0,
          "an empty restore resets to the 100.0 MHz FM-broadcast default, not the prior session's value");
    check(opAfterEmpty.mode == "WFM", "an empty restore resets mode to the default");
    check(opAfterEmpty.agcMode == QStringLiteral("med") && opAfterEmpty.agcThreshold == 65,
          "an empty restore resets AGC to medium / 65");

    // An unknown AGC mode string is ignored, not turned into some other mode.
    RestoredRadioState oddAgc;
    oddAgc.agcMode = QStringLiteral("turbo");
    backend->applyRestoredState(oddAgc);
    check(backend->currentOperatingState().agcMode == QStringLiteral("med"),
          "an unknown restored AGC mode keeps the default");

    // The app pushes the saved slice's AGC BEFORE connecting; it was dropped.
    backend->setSliceAgc(0, QStringLiteral("fast"), 80);
    check(backend->currentOperatingState().agcMode == QStringLiteral("fast")
              && backend->currentOperatingState().agcThreshold == 80,
          "an AGC change before connect is kept");
    backend->setSliceAgc(0, QStringLiteral("off"), 150);
    check(backend->currentOperatingState().agcMode == QStringLiteral("off")
              && backend->currentOperatingState().agcThreshold == 100,
          "the AGC threshold is clamped to 0..100");
    backend->applyRestoredState(empty);

    // ── The dBm scale is kept, saved and echoed ──────────────────────
    {
        QSignalSpy range(backend.get(), &IRadioBackend::panRangeChanged);
        QSignalSpy saved(backend.get(), &IRadioBackend::operatingStateChanged);
        backend->setPanDbmRange(QStringLiteral("0xh1000000"), -110.0, -30.0);
        auto display = backend->currentOperatingState().extension.value(QStringLiteral("panDisplay")).toObject();
        check(display.value(QStringLiteral("minDbm")).toDouble() == -110.0
                  && display.value(QStringLiteral("maxDbm")).toDouble() == -30.0,
              "a dBm scale change is kept for the next save");
        check(saved.count() == 1, "a dBm scale change schedules a save");
        check(range.count() == 1 && range.at(0).at(1).toDouble() == -110.0
                  && range.at(0).at(2).toDouble() == -30.0,
              "a dBm scale change is echoed on panRangeChanged");

        // An inverted range is refused, and the stored one echoed back.
        backend->setPanDbmRange(QStringLiteral("0xh1000000"), -20.0, -60.0);
        display = backend->currentOperatingState().extension.value(QStringLiteral("panDisplay")).toObject();
        check(display.value(QStringLiteral("minDbm")).toDouble() == -110.0,
              "an inverted dBm range is refused");
        check(saved.count() == 1, "a refused dBm range does not save");
        check(range.count() == 2 && range.at(1).at(1).toDouble() == -110.0,
              "a refused dBm range echoes the stored one");
        backend->applyRestoredState(empty);
    }

    // ── Level calibration verbs ──────────────────────────────────────
    // Like every hackrf.* verb they need a connected device (the correction is
    // stored against its serial); the connected path is checked on hardware.
    {
        check(caps.hostLevelCalibration, "declares host level calibration");
        QSignalSpy error(backend.get(), &IRadioBackend::extensionError);
        for (const char* verb : {"levelcal.set", "levelcal.get", "levelcal.calibrate"})
            backend->invokeExtension(QStringLiteral("hackrf"), QString::fromLatin1(verb), 7, -73.0);
        check(error.count() == 3, "the levelcal verbs refuse before connect");
    }

    // ── Setters before connect are safe no-ops, never crash ────────────
    backend->setSliceFrequency(0, 14'074'000.0);
    backend->setKeying(true);
    backend->setKeying(false);
    check(!backend->isConnected(), "still disconnected — setters before connect do nothing harmful");

    // ── RX audio mode/AGC mapping (#42: WDSP RXA channel) ───────────────
    // wdspModeFromString delegates to hl2::modeFromString (already tested via
    // hl2_mode_table_test) for the vocabulary the two backends share, plus the
    // two spellings unique to this backend's own declared mode list (FMN, CWR)
    // that hl2::modeFromString does not recognize.
    check(hackrf::HackRfBackend::wdspModeFromString("USB") == WdspChannel::Mode::Usb,
          "USB maps straight through the shared table");
    check(hackrf::HackRfBackend::wdspModeFromString("LSB") == WdspChannel::Mode::Lsb,
          "LSB maps straight through the shared table");
    check(hackrf::HackRfBackend::wdspModeFromString("AM") == WdspChannel::Mode::Am,
          "AM maps straight through the shared table");
    check(hackrf::HackRfBackend::wdspModeFromString("SAM") == WdspChannel::Mode::Sam,
          "SAM maps straight through the shared table");
    check(hackrf::HackRfBackend::wdspModeFromString("WFM") == WdspChannel::Mode::Wbfm,
          "WFM maps straight through the shared table");
    check(hackrf::HackRfBackend::wdspModeFromString("CW") == WdspChannel::Mode::Cwu,
          "CW maps straight through the shared table (upper-sideband CW)");
    check(hackrf::HackRfBackend::wdspModeFromString("FMN") == WdspChannel::Mode::Fm,
          "FMN (this backend's narrow-FM spelling) maps to WDSP's single Fm mode");
    check(hackrf::HackRfBackend::wdspModeFromString("CWR") == WdspChannel::Mode::Cwl,
          "CWR (reversed-sideband CW) maps to WDSP's Cwl, not the USB fallback");
    check(hackrf::HackRfBackend::wdspModeFromString("bogus") == WdspChannel::Mode::Usb,
          "unknown mode falls back to USB, matching hl2::modeFromString's own fallback");

    check(hackrf::HackRfBackend::wdspAgcModeFromString("off") == 0, "AGC off -> WDSP mode 0");
    check(hackrf::HackRfBackend::wdspAgcModeFromString("slow") == 2, "AGC slow -> WDSP mode 2");
    check(hackrf::HackRfBackend::wdspAgcModeFromString("med") == 3, "AGC med -> WDSP mode 3");
    check(hackrf::HackRfBackend::wdspAgcModeFromString("fast") == 4, "AGC fast -> WDSP mode 4");
    check(hackrf::HackRfBackend::wdspAgcModeFromString("bogus") == 3,
          "unknown AGC mode falls back to medium, matching Hl2Backend's own wdspAgcMode()");

    // ── Mode-appropriate default passband (#42: filter must not survive a
    // mode change, and WFM specifically must not reuse the flat ±100 kHz
    // that used to double as both the pre-demod IF filter and the post-demod
    // audio filter — see defaultPassbandForMode's own comment). ──────────
    check(hackrf::HackRfBackend::defaultPassbandForMode("USB") == std::make_pair(100, 2900),
          "USB passband matches the shared table");
    check(hackrf::HackRfBackend::defaultPassbandForMode("CW") == std::make_pair(-250, 250),
          "CW passband matches the shared table");
    check(hackrf::HackRfBackend::defaultPassbandForMode("WFM") == std::make_pair(-40000, 40000),
          "WFM passband matches the shared table's WBFM entry, not this backend's old ±100 kHz");
    check(hackrf::HackRfBackend::defaultPassbandForMode("FMN") == std::make_pair(-8000, 8000),
          "FMN (narrow FM) aliases onto the shared table's FM entry");
    check(hackrf::HackRfBackend::defaultPassbandForMode("CWR") == std::make_pair(-250, 250),
          "CWR (reversed-sideband CW) aliases onto the shared table's CW entry");

    // ── A stream that dies under the app (#42: unplug mid-session) ─────────
    // The worker's health rule, pure: libhackrf saying the stream is over, or
    // no transfer for longer than the stall limit, is a fault; anything else
    // is a healthy stream.
    using hackrf::HackRfWorker;
    check(HackRfWorker::streamFault(HACKRF_TRUE, 0).isEmpty(), "a streaming, fed stream is healthy");
    check(HackRfWorker::streamFault(HACKRF_TRUE, HackRfWorker::kStreamStallMs).isEmpty(),
          "a gap at the stall limit is still healthy");
    check(!HackRfWorker::streamFault(HACKRF_TRUE, HackRfWorker::kStreamStallMs + 1).isEmpty(),
          "no transfer for longer than the stall limit is a fault");
    check(!HackRfWorker::streamFault(HACKRF_ERROR_STREAMING_STOPPED, 0).isEmpty(),
          "libhackrf ending the stream on its own is a fault");
    check(!HackRfWorker::streamFault(HACKRF_ERROR_STREAMING_THREAD_ERR, 0).isEmpty(),
          "a transfer thread that is gone is a fault");
    check(!HackRfWorker::streamFault(HACKRF_ERROR_STREAMING_EXIT_CALLED, 0).isEmpty(),
          "a transfer thread told to exit is a fault");

    // The backend's reaction: report the error, THEN disconnect, and emit
    // nothing after disconnected() (IRadioBackend rule 6).
    {
        hackrf::HackRfBackend lost;
        QStringList order;
        QObject::connect(&lost, &IRadioBackend::connectionError, [&](const QString&) { order << "error"; });
        QObject::connect(&lost, &IRadioBackend::disconnected, [&] { order << "disconnected"; });
        hackrf::HackRfBackendTestAccess::markConnected(lost);
        emit hackrf::HackRfBackendTestAccess::worker(lost)->streamStopped(true, QStringLiteral("unplugged"));
        check(order == QStringList({"error", "disconnected"}),
              "a lost stream reports the error, then disconnects, then nothing");
        check(!lost.isConnected(), "a lost stream leaves the backend disconnected");
        order.clear();
        emit hackrf::HackRfBackendTestAccess::worker(lost)->streamStopped(true, QStringLiteral("late"));
        check(order.isEmpty(), "a stream report after the session ended is ignored");
    }

    if (g_failures == 0) {
        std::printf("hackrf_backend_test: OK\n");
    } else {
        std::fprintf(stderr, "hackrf_backend_test: %d failure(s)\n", g_failures);
    }
#else
    std::printf("hackrf_backend_test: SKIPPED (AETHER_BACKEND_HACKRF not defined)\n");
#endif

    return g_failures == 0 ? 0 : 1;
}
