// Unit test for HackRfBackend (#42). Never calls connectRadio() — no
// hardware needed, mirroring rtl_backend_test's own approach — so this
// exercises capabilities() declaration and the restore-state contract only.

#include "core/backends/hackrf/HackRfBackend.h"
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

using namespace AetherSDR;

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
    backend->applyRestoredState(restoredState);

    const auto op = backend->currentOperatingState();
    check(op.rfFrequencyHz == 146'520'000.0, "restored frequency round-trips");
    check(op.mode == "FM", "restored mode is canonicalized to uppercase");
    check(op.filterLowHz == -8'000 && op.filterHighHz == 8'000, "restored filter round-trips");
    check(op.sampleRateHz == 10'000'000, "restored sample rate round-trips");
    // AGC runs in WDSP on this host, so the client is the only place the
    // operator's mode and threshold can live. It was never saved: every
    // session started on medium/65 whatever had been chosen.
    check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::Agc),
          "declares the AGC settings domain");
    check(op.agcMode == QStringLiteral("slow"), "restored AGC mode round-trips");
    check(op.agcThreshold == 40, "restored AGC threshold round-trips");
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
