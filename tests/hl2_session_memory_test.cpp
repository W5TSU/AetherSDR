// What a Hermes-Lite 2 remembers across a restart beyond the flat operating
// state (upstream 4e547347, #5777): every receiver's own setpoints, the
// receiver set, and the manual notches. The radio stores none of these, so the
// client is its memory.
//
// Socket-free: the receivers are built and configured as connectRadio() leaves
// them, then the link edge is injected, so the replay runs through the
// production linkUp handler and the operator's own add path. RadioStateMemory
// is driven against an isolated settings profile. Nothing is keyed.

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/RadioStateMemory.h"
#include "core/backends/NotchDelta.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2ModeTable.h"
#include "core/backends/hl2/Hl2Receivers.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"

#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

#include <cmath>
#include <cstdio>
#include <tuple>

namespace AetherSDR::hl2 {

struct Hl2SessionMemoryTestAccess {
    // Receivers as connectRadio() builds and seeds them, configured as
    // beginDspSetup() leaves them, then a link edge. No transport.
    static bool bringUp(Hl2Backend& b, int count = 1, int boardMaxRx = Hl2Backend::kAssumedBoardMaxRx)
    {
        b.m_boardMaxRx = boardMaxRx;
        b.buildReceivers(count);
        const double startHz = b.m_haveRestoredState && b.m_restoredState.rfFrequencyHz > 0.0
            ? b.m_restoredState.rfFrequencyHz : 10'000'000.0;
        for (Hl2Backend::Receiver& r : b.m_rx) {
            r.sliceFreqHz = startHz;
            r.ncoHz = startHz;
        }
        b.seedReceiverAgc();
        b.seedOpenReceiversFromMemory();
        for (int ddc = 0; ddc < count; ++ddc) {
            Hl2Backend::Receiver* const r = b.rx(ddc);
            if (!r || !r->dsp) {
                return false;
            }
            Hl2RxDsp::Config config;
            config.inputSampleRateHz = b.m_sampleRateHz;
            config.mode = modeFromString(r->mode);
            std::tie(config.filterLowHz, config.filterHighHz) = b.dspFilterHz(*r);
            bool configured = false;
            int channelId = -1;
            Hl2RxDsp* const dsp = r->dsp;
            QMetaObject::invokeMethod(dsp, [dsp, config, &configured, &channelId] {
                configured = dsp->configure(config);
                channelId = dsp->wdspChannelId();
            }, Qt::BlockingQueuedConnection);
            if (!configured) {
                return false;
            }
            Hl2ReceiverIds* const ids = b.m_ids.mutableByDdc(ddc);
            ids->dspChannel = channelId;
            ids->analyzerId = ids->uiNumber;
        }
        b.publishIoDsps();
        b.m_sessionReplayPending = true;   // what connectRadio() arms
        linkUp(b);
        return b.isConnected();
    }
    static void linkUp(Hl2Backend& b)
    {
        QMetaObject::invokeMethod(b.m_metis, "linkUp", Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static int receivers(const Hl2Backend& b) { return static_cast<int>(b.m_rx.size()); }
    static const Hl2Backend::Receiver* rx(const Hl2Backend& b, int ddc) { return b.rx(ddc); }
    static int txDdc(const Hl2Backend& b) { return b.m_txDdc; }
    static int liveNotches(const Hl2Backend& b) { return static_cast<int>(b.m_notches.size()); }
    static int restoredReceivers(const Hl2Backend& b)
    {
        return static_cast<int>(b.m_restoredReceivers.size());
    }
    static int restoredNotches(const Hl2Backend& b)
    {
        return static_cast<int>(b.m_restoredNotches.size());
    }
    static int restoredTxReceiver(const Hl2Backend& b) { return b.m_restoredTxReceiver; }
    static int unreopened(const Hl2Backend& b) { return static_cast<int>(b.m_replayUnreopened.size()); }
    // connectRadio() arms the replay before the link comes up; a capture in
    // that window must not erase what the replay is about to bring back.
    static void armReplay(Hl2Backend& b) { b.m_sessionReplayPending = true; }
    static double restoredRfHz(const Hl2Backend& b) { return b.m_restoredState.rfFrequencyHz; }
    static QString restoredMode(const Hl2Backend& b) { return b.m_restoredState.mode; }
    static QString panIdOf(const Hl2Backend& b, int ddc)
    {
        const Hl2ReceiverIds* ids = b.m_ids.byDdc(ddc);
        return ids ? ids->panId : QString();
    }
};

}   // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2SessionMemoryTestAccess;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    std::printf("  %s  %s\n", condition ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!condition) {
        ++g_failures;
    }
}

bool near(double a, double b) { return std::abs(a - b) < 0.5; }

QJsonObject receiverEntry(double freqHz, const QString& mode)
{
    return QJsonObject{{QStringLiteral("freqHz"), freqHz},
                       {QStringLiteral("ncoHz"), freqHz},
                       {QStringLiteral("mode"), mode}};
}

QJsonObject notchEntry(double centerHz, double widthHz, bool active)
{
    return QJsonObject{{QStringLiteral("centerHz"), centerHz},
                       {QStringLiteral("widthHz"), widthHz},
                       {QStringLiteral("active"), active}};
}

// Two good receivers (A on 40 m FT8 with NB and RIT, B on 20 m holding
// transmit with its own pan centre), then a bad entry that ends the list, then
// one that must not move up into its slot. Two good notches and two bad.
RestoredRadioState rememberedSession()
{
    QJsonObject a = receiverEntry(7'074'000.0, QStringLiteral("DIGU"));
    a.insert(QStringLiteral("nbOn"), true);
    a.insert(QStringLiteral("nbLevel"), 70);
    a.insert(QStringLiteral("ritOn"), true);
    a.insert(QStringLiteral("ritHz"), 120);
    QJsonObject b = receiverEntry(14'074'000.0, QStringLiteral("usb"));
    // 6 kHz off the dial: inside a 48 kHz span's usable window.
    b.insert(QStringLiteral("ncoHz"), 14'080'000.0);
    b.insert(QStringLiteral("filterLowHz"), 200);
    b.insert(QStringLiteral("filterHighHz"), 2800);
    b.insert(QStringLiteral("transmit"), true);
    b.insert(QStringLiteral("nbLevel"), 150);     // out of range: drops to default
    b.insert(QStringLiteral("ritHz"), 20'000);    // past the clamp: drops to 0
    b.insert(QStringLiteral("xitOn"), true);
    b.insert(QStringLiteral("xitHz"), -300);
    const QJsonObject bad = receiverEntry(0.0, QStringLiteral("USB"));
    const QJsonObject after = receiverEntry(21'074'000.0, QStringLiteral("USB"));

    RestoredRadioState state;
    state.rfFrequencyHz = 14'074'000.0;   // the flat fields follow transmit (B)
    state.mode = QStringLiteral("USB");
    state.extension = QJsonObject{
        {QStringLiteral("receivers"), QJsonArray{a, b, bad, after}},
        {QStringLiteral("notches"),
         QJsonArray{notchEntry(7'075'500.0, 100.0, true),
                    notchEntry(14'076'000.0, 250.0, false),
                    notchEntry(7'076'000.0, 0.0, true),          // no width
                    notchEntry(50'000'000.0, 100.0, true)}}};    // off this radio
    state.extensionSchemaVersion = 1;
    return state;
}

QJsonArray capturedArray(const Hl2Backend& backend, const char* key)
{
    return backend.currentOperatingState().extension.value(QLatin1String(key)).toArray();
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-session-memory-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();
    AppSettings::instance().load();
    std::printf("\n  HL2 session memory: receivers and notches (upstream 4e547347)\n\n");

    {
        Hl2Backend backend;
        const auto domains = backend.capabilities().clientSettingsDomains;
        check(domains.testFlag(RadioCapabilities::ClientSettingsDomain::Receivers)
                  && domains.testFlag(RadioCapabilities::ClientSettingsDomain::Notches),
              "the HL2 declares the Receivers and Notches domains");
    }

    // ── Validation at the restore boundary ─────────────────────────────────
    {
        Hl2Backend backend;
        backend.applyRestoredState(rememberedSession());
        check(Access::restoredReceivers(backend) == 2,
              "a bad receiver entry ends the list, so the one after it does not move up");
        check(Access::restoredTxReceiver(backend) == 1, "transmit is remembered on receiver B");
        check(near(Access::restoredRfHz(backend), 7'074'000.0)
                  && Access::restoredMode(backend) == QStringLiteral("DIGU"),
              "receiver A's entry, not the transmit-following flat fields, starts the session");
        check(Access::restoredNotches(backend) == 2,
              "a notch with no width or off the tuning range is dropped alone");

        Access::armReplay(backend);
        const QJsonArray receivers = capturedArray(backend, "receivers");
        const QJsonArray notches = capturedArray(backend, "notches");
        check(receivers.size() == 2 && notches.size() == 2,
              "a capture before the replay keeps the remembered receivers and notches");
        check(receivers.size() == 2
                  && receivers.at(1).toObject().value(QStringLiteral("transmit")).toBool()
                  && receivers.at(1).toObject().value(QStringLiteral("mode")).toString()
                         == QStringLiteral("USB")
                  && receivers.at(1).toObject().value(QStringLiteral("nbLevel")).toInt() == 50
                  && receivers.at(1).toObject().value(QStringLiteral("ritHz")).toInt() == 0
                  && receivers.at(1).toObject().value(QStringLiteral("xitHz")).toInt() == -300,
              "receiver B round-trips with its bad fields dropped to defaults, alone");

        backend.applyRestoredState({});
        check(Access::restoredReceivers(backend) == 0 && Access::restoredNotches(backend) == 0,
              "a radio with no memory carries none of the last radio's receivers or notches");
    }

    // ── The replay, through the production link edge ───────────────────────
    {
        Hl2Backend backend;
        int notchReports = 0;
        int inactiveNotches = 0;
        QList<SliceDelta> sliceA;
        QObject::connect(&backend, &IRadioBackend::notchChanged, &backend,
                         [&](int, const NotchDelta& d) {
                             ++notchReports;
                             if (d.active && !*d.active) {
                                 ++inactiveNotches;
                             }
                         });
        QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                         [&](int sliceId, const SliceDelta& d) {
                             if (sliceId == 0) {
                                 sliceA << d;
                             }
                         });
        backend.applyRestoredState(rememberedSession());
        check(Access::bringUp(backend), "the HL2 comes up with receiver A");
        check(Access::receivers(backend) == 2, "receiver B is reopened on connect");
        const auto* a = Access::rx(backend, 0);
        const auto* b = Access::rx(backend, 1);
        check(a && near(a->sliceFreqHz, 7'074'000.0) && a->mode == QStringLiteral("DIGU")
                  && a->nbOn && a->nbLevel == 70 && a->ritOn && a->ritHz == 120,
              "receiver A comes back on 40 m DIGU with its blanker and RIT");
        check(b && near(b->sliceFreqHz, 14'074'000.0) && near(b->ncoHz, 14'080'000.0)
                  && b->mode == QStringLiteral("USB") && b->filterLowHz == 200
                  && b->filterHighHz == 2800 && b->xitOn && b->xitHz == -300,
              "receiver B comes back with its frequency, pan centre, passband and XIT");
        check(Access::txDdc(backend) == 1, "transmit returns to receiver B (ownership only)");
        check(Access::liveNotches(backend) == 2 && notchReports >= 2 && inactiveNotches == 1,
              "both notches are re-created and reported, one still inactive");
        int nbEchoes = 0;
        for (const SliceDelta& d : sliceA) {
            if (d.nb && *d.nb && d.nbLevel && *d.nbLevel == 70) {
                ++nbEchoes;
            }
        }
        check(nbEchoes == 1, "receiver A's restored blanker is published exactly once");

        Access::linkUp(backend);
        check(Access::receivers(backend) == 2 && Access::liveNotches(backend) == 2,
              "a second link edge in the same session replays nothing");

        const QJsonArray receivers = capturedArray(backend, "receivers");
        const QJsonArray notches = capturedArray(backend, "notches");
        check(receivers.size() == 2
                  && near(receivers.at(1).toObject().value(QStringLiteral("freqHz")).toDouble(),
                          14'074'000.0)
                  && receivers.at(1).toObject().value(QStringLiteral("transmit")).toBool()
                  && !receivers.at(0).toObject().value(QStringLiteral("transmit")).toBool(),
              "the live capture records both receivers, transmit on B");
        check(notches.size() == 2 && !notches.at(0).toObject().contains(QStringLiteral("id")),
              "the live capture records the notches without their session ids");

        check(backend.removePanadapter(Access::panIdOf(backend, 1)),
              "the operator closes receiver B");
        check(capturedArray(backend, "receivers").size() == 1,
              "the capture after closing it remembers one receiver");
        backend.disconnectRadio();
    }

    // ── Fewer receivers than remembered ────────────────────────────────────
    {
        RestoredRadioState state;
        state.extension = QJsonObject{{QStringLiteral("receivers"),
            QJsonArray{receiverEntry(7'074'000.0, QStringLiteral("USB")),
                       receiverEntry(10'136'000.0, QStringLiteral("USB")),
                       receiverEntry(14'074'000.0, QStringLiteral("USB")),
                       receiverEntry(18'100'000.0, QStringLiteral("USB"))}}};
        state.extensionSchemaVersion = 1;
        Hl2Backend backend;
        backend.applyRestoredState(state);
        check(Access::bringUp(backend, 1, /*boardMaxRx=*/2), "a two-receiver board comes up");
        check(Access::receivers(backend) == 2 && Access::unreopened(backend) == 2,
              "the board's ceiling decides how many come back; the rest are kept");
        check(capturedArray(backend, "receivers").size() == 4,
              "the capture keeps the two it could not reopen for the next connect");
        check(backend.removePanadapter(Access::panIdOf(backend, 1))
                  && capturedArray(backend, "receivers").size() == 1,
              "once the operator reshapes the set, the kept tail is theirs to rebuild");
        backend.disconnectRadio();
    }

    // ── The document gate ──────────────────────────────────────────────────
    {
        const RadioSettingsScope scope(QStringLiteral("hl2"), QStringLiteral("AA:BB:CC:DD:EE:61"));
        Hl2Backend backend;
        const RadioCapabilities caps = backend.capabilities();
        backend.applyRestoredState(rememberedSession());
        Access::armReplay(backend);
        check(RadioStateMemory::store(scope, caps, backend.currentOperatingState()),
              "the session is stored for this radio");
        const RestoredRadioState reloaded = RadioStateMemory::load(scope, caps);
        check(reloaded.extension.value(QStringLiteral("receivers")).toArray().size() == 2
                  && reloaded.extension.value(QStringLiteral("notches")).toArray().size() == 2,
              "the receivers and notches come back from the store");
        RadioCapabilities narrowed = caps;
        narrowed.clientSettingsDomains &= ~RadioCapabilities::ClientSettingsDomains(
            RadioCapabilities::ClientSettingsDomain::Receivers
            | RadioCapabilities::ClientSettingsDomain::Notches);
        const RestoredRadioState gated = RadioStateMemory::load(scope, narrowed);
        check(!gated.extension.contains(QStringLiteral("receivers"))
                  && !gated.extension.contains(QStringLiteral("notches")),
              "a backend that does not declare the domains is handed neither");
    }

    std::printf("\n  %s\n", g_failures == 0 ? "hl2_session_memory_test: all checks passed"
                                           : "hl2_session_memory_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
