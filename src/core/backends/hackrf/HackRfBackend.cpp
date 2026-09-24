#include "HackRfBackend.h"
#include "core/backends/hackrf/HackRfWorker.h"
#include "core/backends/hl2/Hl2ModeTable.h"

#include <QJsonObject>
#include <QLoggingCategory>

#include <algorithm>
#include <span>

Q_DECLARE_LOGGING_CATEGORY(lcHackRf)  // defined in HackRfWorker.cpp

namespace AetherSDR::hackrf {

namespace {
constexpr const char* kPanId = "0xh1000000";
// FFT size for the wideband panadapter spectrum — matches RtlSdrDdc's own
// choice for the same "USB SDR wideband capture" category of source; twice
// Hl2RxDsp's default (1024) since HL2's span is narrower to begin with.
constexpr int kSpectrumFftSize = 2048;

// RX audio (WDSP RXA channel) constants — see the class comment.
//
// NOT 48 kHz, despite that being HackRfDdc's own documented default and
// Hl2RxDsp's WDSP input rate: 48 kHz only covers ±24 kHz of IQ bandwidth
// (Nyquist), which is plenty for the narrow modes (SSB/CW/AM/NFM top out
// around ±12.5 kHz) but far short of broadcast WFM's real bandwidth
// (±75 kHz deviation). Feeding WFM through a 48 kHz slice puts the FM
// discriminator's per-sample phase step (2π·Δf/rate) past π on loud peaks —
// a genuine phase-wrap, not just a quality trade-off — measured on real
// hardware as exactly the reported symptom: quiet, badly distorted audio.
// 384 kHz matches SDRangel's own WFM channel rate exactly (its
// WFMDemodSink defaults: 384 kHz channel rate, 80 kHz RF bandwidth i.e.
// ±40 kHz, 75 kHz deviation scaling — the same reference points WDSP's own
// wbfm.c uses), giving the discriminator twice the phase margin 192 kHz
// did. Divides cleanly into the 24 kHz audio-output rate below; the extra
// CPU on the narrow modes is the accepted trade for one shared rate instead
// of a per-mode-adaptive one (out of scope for this increment).
// HackRfBackend calls HackRfDdc::setOutputSampleRateHz(kAudioDdcRateHz) in
// connectRadio() specifically because this no longer matches HackRfDdc's
// own default.
constexpr int kAudioDdcRateHz = 384'000;
// AudioEngine's native RX rate (Hl2RxDsp::Config::audioSampleRateHz's own
// comment) — emitting it directly means no resampling downstream.
constexpr int kAudioOutputRateHz = 24'000;
// WDSP processing block, matching Hl2RxDsp's own default dspBlockSize.
// 1024 * 24000 / 384000 = 64 exactly, satisfying WdspChannel::create()'s
// integer-output-block requirement.
constexpr std::size_t kAudioDspBlockSize = 1024;
}

WdspChannel::Mode HackRfBackend::wdspModeFromString(const QString& mode) noexcept
{
    const QString u = mode.trimmed().toUpper();
    if (u == QLatin1String("FMN")) return WdspChannel::Mode::Fm;
    if (u == QLatin1String("CWR")) return WdspChannel::Mode::Cwl;
    return hl2::modeFromString(mode);
}

int HackRfBackend::wdspAgcModeFromString(const QString& mode) noexcept
{
    const QString m = mode.trimmed().toLower();
    if (m == QLatin1String("off"))  return 0;
    if (m == QLatin1String("slow")) return 2;
    if (m == QLatin1String("fast")) return 4;
    return 3;  // medium: WDSP's own default, and this backend's fallback
}

std::pair<int, int> HackRfBackend::defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.trimmed().toUpper();
    if (u == QLatin1String("FMN")) return hl2::defaultPassbandForMode(QStringLiteral("FM"));
    if (u == QLatin1String("CWR")) return hl2::defaultPassbandForMode(QStringLiteral("CW"));
    return hl2::defaultPassbandForMode(mode);
}

HackRfBackend::HackRfBackend(QObject* parent)
    : IRadioBackend(parent)
    , m_worker(std::make_unique<HackRfWorker>())
    , m_ddc(std::make_unique<HackRfDdc>())
    , m_spectrum(std::make_unique<hl2::Hl2Spectrum>(kSpectrumFftSize))
{
    // m_arbiter itself is (re)created fresh in connectRadio() — a stale
    // pending/timed-out state from a previous session must not leak into
    // the next one if this backend object is reused for another connect.
    // Its signals are connected there, once per instance; nothing to wire
    // here. m_worker/m_ddc are never replaced, so their connections live
    // for the whole backend lifetime.
    connect(m_worker.get(), &HackRfWorker::streamStopped, this, &HackRfBackend::onWorkerStreamStopped);
    connect(m_worker.get(), &HackRfWorker::rxIqReady, this, &HackRfBackend::onWorkerRxIqReady);
    connect(m_ddc.get(), &HackRfDdc::decimatedIqReady, this, &HackRfBackend::onDdcAudioIqReady);

    // Polls HackRfTxRxArbiter::tick() for timeout recovery. HackRfWorker's
    // start/stop calls are synchronous (see its own header comment on why
    // there's no dedicated worker thread — libhackrf's start_rx/start_tx
    // don't block, unlike RtlSdrWorker's rtlsdr_read_async), so a pending
    // transition normally resolves within the same call stack that
    // requested it; this timer exists for the case that call genuinely
    // hangs or a confirm never arrives, not for the common path.
    m_arbiterTickTimer.setInterval(50);
    connect(&m_arbiterTickTimer, &QTimer::timeout, this, &HackRfBackend::onArbiterTick);
}

HackRfBackend::~HackRfBackend()
{
    if (m_connected) disconnectRadio();
}

RadioCapabilities HackRfBackend::capabilities() const
{
    RadioCapabilities c;
    c.family = QStringLiteral("hackrf");
    c.model = QStringLiteral("HackRF");
    c.manufacturer = QStringLiteral("Great Scott Gadgets");

    // TX — full RX+TX (design doc v1), FM/CW only. receiveOnlyModes gates
    // the other demodulatable-but-not-transmittable modes; the engine TX
    // guard (RFC §6) is what actually enforces it.
    c.canTransmit = true;
    c.txPowerMaxWatts = 0.015;  // ~15 dBm typical max, no PA — hackrf.readthedocs.io
    c.hostModulates = true;         // WDSP TXA on this host, once wired — see the class comment
    c.takesTxAudioOverSeam = true;  // audio reaches this backend via submitTxAudio, not DAX/VITA-49
    c.hasRadioPttReadback = false;  // no readback plane, matches HL2 — setKeying's command edge is the only edge
    c.receiveOnlyModes = {QStringLiteral("AM"), QStringLiteral("SAM"), QStringLiteral("WFM"),
                          QStringLiteral("USB"), QStringLiteral("LSB")};
    c.agcModes = {QStringLiteral("off"), QStringLiteral("slow"),
                  QStringLiteral("med"), QStringLiteral("fast")};
    c.alcMeterUnit = QStringLiteral("dBFS");
    c.cwSpeedMinWpm = 5;
    c.cwSpeedMaxWpm = 60;
    c.cwPitchMinHz = 300;
    c.cwPitchMaxHz = 1000;
    c.cwPitchStepHz = 10;

    // Single slice/pan for now — see the class comment on why (no HackRfDdc yet).
    c.canCreateSlices = false;
    c.maxSlices = 1;
    c.maxPanadapters = 1;

    // 1 MHz - 6 GHz per hackrf.readthedocs.io's documented tuning range.
    c.tuningMinHz = 1'000'000;
    c.tuningMaxHz = 6'000'000'000;
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine,
                              1'000'000, 6'000'000'000};

    // hackrf_set_sample_rate accepts 2-20 MHz continuously; advertised as
    // steps because the UI presents a discrete list, not a true continuous
    // control — same reasoning as RtlSdrBackend's own non-contiguous list.
    c.sampleRatesHz = {2'000'000, 4'000'000, 8'000'000, 10'000'000,
                       12'500'000, 16'000'000, 20'000'000};

    c.persistsMemories = false;
    c.hasSupplyVoltageTelemetry = false;
    c.hasAudioPeakingFilter = false;

    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::Tuning
                            | RadioCapabilities::ClientSettingsDomain::Passband
                            | RadioCapabilities::ClientSettingsDomain::SpanRate
                            | RadioCapabilities::ClientSettingsDomain::RfGain
                            | RadioCapabilities::ClientSettingsDomain::Memories;

    c.extensions["hackrf"] = QVariantMap{{"serial", m_serial}};
    c.extensionNamespaces = {"hackrf"};

    return c;
}

void HackRfBackend::applyRestoredState(const RestoredRadioState& state)
{
    // HackRF has no radio-side memory (Reset first — this backend object
    // may be reused for a different physical device between sessions, and
    // an empty snapshot must not inherit the previous one's settings.
    // Mirrors RtlSdrBackend::applyRestoredState exactly.
    m_sliceFreqHz = 100'000'000.0;
    m_sliceMode = QStringLiteral("WFM");
    // Matches defaultPassbandForMode("WFM") — see its own comment on why
    // ±100 kHz (the value this used to be) is not just wider than needed
    // but actively wrong: WdspChannel applies ONE filter to both the
    // pre-demod IF stage and the post-demod audio stage, and ±100 kHz on
    // the audio side sits at the edge of/beyond this chain's 192 kHz
    // internal rate's Nyquist.
    m_sliceFilterLow = -40'000;
    m_sliceFilterHigh = 40'000;
    m_sampleRateHz = 8'000'000.0;
    m_vgaGainDb = 20;
    m_lnaGainDb = 16;
    m_ampEnabled = false;

    if (state.rfFrequencyHz > 0) {
        m_sliceFreqHz = state.rfFrequencyHz;
    }
    if (!state.mode.isEmpty()) {
        m_sliceMode = state.mode.trimmed().toUpper();
    }
    if (state.filterLowHz < state.filterHighHz) {
        m_sliceFilterLow = static_cast<int>(state.filterLowHz);
        m_sliceFilterHigh = static_cast<int>(state.filterHighHz);
    }
    if (state.sampleRateHz > 0) {
        m_sampleRateHz = state.sampleRateHz;
    }
    const QJsonValue gainVal = state.extension.value(QStringLiteral("rfGain"));
    if (gainVal.isObject()) {
        const QJsonObject obj = gainVal.toObject();
        if (obj.contains(QStringLiteral("vgaGainDb")))
            m_vgaGainDb = obj.value(QStringLiteral("vgaGainDb")).toInt(m_vgaGainDb);
        if (obj.contains(QStringLiteral("lnaGainDb")))
            m_lnaGainDb = obj.value(QStringLiteral("lnaGainDb")).toInt(m_lnaGainDb);
        if (obj.contains(QStringLiteral("ampEnabled")))
            m_ampEnabled = obj.value(QStringLiteral("ampEnabled")).toBool(m_ampEnabled);
    }
}

RestoredRadioState HackRfBackend::currentOperatingState() const
{
    RestoredRadioState state;
    state.rfFrequencyHz = m_sliceFreqHz;
    state.mode = m_sliceMode;
    state.filterLowHz = m_sliceFilterLow;
    state.filterHighHz = m_sliceFilterHigh;
    state.sampleRateHz = static_cast<int>(m_sampleRateHz);
    state.extensionSchemaVersion = 1;
    state.extension[QStringLiteral("rfGain")] = QJsonObject{
        {QStringLiteral("vgaGainDb"), m_vgaGainDb},
        {QStringLiteral("lnaGainDb"), m_lnaGainDb},
        {QStringLiteral("ampEnabled"), m_ampEnabled},
    };
    return state;
}

void HackRfBackend::connectRadio(const RadioConnectRequest& request)
{
    if (m_connected) disconnectRadio();

    m_serial = request.serial.trimmed();

    // HackRfDiscovery's index-based fallback identity ("hackrf:<index>"),
    // used when a device's USB serial was empty or duplicated — mirrors
    // RtlSdrBackend::connectRadio()'s own "rtl:<index>" parsing for the
    // identical reason. A real serial (the common case) skips this and
    // opens by serial below.
    bool opened = false;
    if (m_serial.startsWith(QLatin1String("hackrf:"))
        && (request.serialIdentity.indexLocator
            || request.serialIdentity.reportedSerial.isEmpty())) {
        bool ok = false;
        const int idx = m_serial.mid(7).toInt(&ok);
        if (!ok || idx < 0 || !m_worker->openByIndex(idx)) {
            emit connectionError(tr("HackRF at index %1 not found — the device list may "
                                     "have changed since it was discovered").arg(idx));
            return;
        }
        opened = true;
    } else {
        opened = m_worker->open(m_serial);
    }
    if (!opened) {
        emit connectionError(tr("Could not open HackRF device"
                                 "%1").arg(m_serial.isEmpty() ? QString() : QStringLiteral(" (serial %1)").arg(m_serial)));
        return;
    }

    if (!m_worker->setSampleRateHz(m_sampleRateHz)
        || !m_worker->setFreqHz(static_cast<std::uint64_t>(m_sliceFreqHz))
        || !m_worker->setVgaGainDb(m_vgaGainDb)
        || !m_worker->setLnaGainDb(m_lnaGainDb)
        || !m_worker->setAmpEnable(m_ampEnabled)) {
        emit connectionError(tr("HackRF opened but initial configuration failed"));
        m_worker->close();
        return;
    }

    // Single-slice: the DDC's "wideband center" and "slice target" are the
    // same frequency for now (HackRfWorker tunes directly to the requested
    // frequency; there's no wider-than-the-slice capture actually being
    // exploited yet). NCO shift is a no-op today — this sets up the shape
    // multi-slice will need (a wideband-tuned center with per-slice
    // offsets) without claiming multi-slice works before HackRfBackend
    // actually owns more than one HackRfDdc instance.
    m_ddc->setInputSampleRateHz(m_sampleRateHz);
    m_ddc->setCenterFrequencyHz(m_sliceFreqHz);
    m_ddc->setSliceFrequencyHz(m_sliceFreqHz);
    // Not HackRfDdc's own 48 kHz default — see kAudioDdcRateHz's comment on
    // why WFM needs a wider slice than WDSP's usual 48 kHz input.
    m_ddc->setOutputSampleRateHz(kAudioDdcRateHz);
    rebuildRxChannel();

    m_connected = true;
    m_clock.start();
    m_arbiterTickTimer.start();
    // A fresh instance, not a reset on the old one — QObject subclasses
    // aren't copy-assignable, and this also means each connectRadio() call
    // gets its own arbiter with no risk of double-connecting signals onto
    // one that's already wired (the old instance, and its connections,
    // are simply destroyed here).
    m_arbiter = std::make_unique<HackRfTxRxArbiter>();
    connect(m_arbiter.get(), &HackRfTxRxArbiter::wantRxStart, this, &HackRfBackend::onArbiterWantRxStart);
    connect(m_arbiter.get(), &HackRfTxRxArbiter::wantRxStop,  this, &HackRfBackend::onArbiterWantRxStop);
    connect(m_arbiter.get(), &HackRfTxRxArbiter::wantTxStart, this, &HackRfBackend::onArbiterWantTxStart);
    connect(m_arbiter.get(), &HackRfTxRxArbiter::wantTxStop,  this, &HackRfBackend::onArbiterWantTxStop);
    connect(m_arbiter.get(), &HackRfTxRxArbiter::arbitrationTimedOut, this, &HackRfBackend::onArbiterTimedOut);
    m_arbiter->requestRx(nowMs());

    // connected() MUST fire before the initial slice/pan state (mirrors
    // RtlSdrBackend::connectRadio()'s own ordering). RadioModel::onConnected()
    // unconditionally stages whatever is currently in m_slices/m_panadapters as
    // "previous session, reclaimable on the next status" — publishing the
    // initial state first meant it got staged the instant connected() fired
    // and then sat orphaned, because HackRF (unlike Flex) never sends a
    // follow-up status broadcast on its own to trigger a reclaim: the operator
    // saw a live-looking VFO that silently stopped updating. See the
    // regression this produced: dig into the reconnect-staging path if this
    // ordering ever needs to change again.
    emit connected();
    emitInitialState();
}

void HackRfBackend::disconnectRadio()
{
    if (!m_connected) return;
    m_arbiterTickTimer.stop();
    m_worker->close();  // also stops any active RX/TX — see HackRfWorker::close()
    m_connected = false;
    // No RX IQ can reach it once the worker is closed, but drop it anyway
    // rather than leave a channel from the last session sitting idle — the
    // next connectRadio() builds a fresh one regardless.
    m_rxChannel.reset();
    m_audioIqBuffer.clear();
    emit disconnected();
}

void HackRfBackend::setSliceFrequency(int sliceId, double hz)
{
    Q_UNUSED(sliceId);  // single slice
    if (!m_connected) return;
    m_sliceFreqHz = hz;
    m_worker->setFreqHz(static_cast<std::uint64_t>(hz));
    m_ddc->setCenterFrequencyHz(hz);
    m_ddc->setSliceFrequencyHz(hz);
    // Single-slice/single-pan: every VFO retune moves the ENTIRE captured
    // span (there is no independent "slice inside a wider pan window" the
    // way RtlSdrBackend has), so the pan model must move with it every time,
    // unconditionally — matching what setPanCenter() already does below.
    // Missing this left the pan/waterfall's own centerMhz stale at whatever
    // it was last explicitly set to (e.g. the connect-time default), while
    // the hardware — and so the audio, which is genuinely correct — kept
    // following every real retune. Measured on real hardware (#42): audio
    // correctly tuned to a real station while the waterfall's peak appeared
    // at the wrong position relative to the VFO cursor, by an amount that
    // grew with how far the operator had retuned since the last event that
    // happened to touch the pan center — a moving target, not a fixed
    // offset, which is exactly what a stale label produces.
    emit panCenterBandwidthChanged(QString::fromLatin1(kPanId), hz / 1e6,
                                   m_sampleRateHz / 1e6);

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = m_sliceMode;
    emit sliceChanged(0, delta);
}

void HackRfBackend::setSliceMode(int sliceId, const QString& mode)
{
    Q_UNUSED(sliceId);
    if (!m_connected) return;
    const QString previous = m_sliceMode;
    m_sliceMode = mode.trimmed().toUpper();

    // The passband belongs to the mode, adopted on CHANGE only so an
    // operator's own filter edit survives until they change mode again —
    // matches Hl2Backend::setSliceMode's own documented policy (its comment
    // there explains why: nothing else heals a stale filter left over from
    // the previous mode, which reads as noise/distortion rather than "wrong
    // filter", exactly what was observed here going WFM -> FM).
    if (!previous.isEmpty() && previous.compare(m_sliceMode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(m_sliceMode);
        m_sliceFilterLow = lo;
        m_sliceFilterHigh = hi;
    }

    // ORDER IS LOAD-BEARING (Hl2Backend::setSliceMode's own documented
    // finding): WDSP's SetRXAMode rebuilds the NBP filter stage from its own
    // per-mode notion of the passband, discarding whatever setFilter() set
    // before it. The filter must be re-pushed AFTER setMode() on every call,
    // not only when the mode actually changed.
    if (m_rxChannel) {
        m_rxChannel->setMode(wdspModeFromString(m_sliceMode));
        m_rxChannel->setFilter(m_sliceFilterLow, m_sliceFilterHigh);
    }

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = m_sliceMode;
    delta.filterLow = m_sliceFilterLow;
    delta.filterHigh = m_sliceFilterHigh;
    emit sliceChanged(0, delta);
}

void HackRfBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    Q_UNUSED(sliceId);
    if (!m_connected || lowHz >= highHz) return;
    m_sliceFilterLow = lowHz;
    m_sliceFilterHigh = highHz;
    if (m_rxChannel) {
        m_rxChannel->setFilter(lowHz, highHz);
    }

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = m_sliceMode;
    delta.filterLow = lowHz;
    delta.filterHigh = highHz;
    emit sliceChanged(0, delta);
}

void HackRfBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    Q_UNUSED(sliceId);
    if (!m_connected) return;
    m_agcModeIndex = wdspAgcModeFromString(mode);
    // 0..100 -> 0..100 dB ceiling — see the m_agcMaxGainDb header comment on
    // why this is 1:1 rather than Hl2Backend's *0.6 map (measured on real
    // hardware: WBFM's raw discriminator output needs far more headroom than
    // SSB/AM's mixer output does before it clips).
    m_agcMaxGainDb = std::clamp(thresholdDb, 0, 100) * 1.0;
    if (m_rxChannel) {
        m_rxChannel->setAgc(m_agcModeIndex, m_agcMaxGainDb);
    }
}

void HackRfBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(panId);
    Q_UNUSED(intent);  // this pan's window is not slaved to the VFO independently of it (single slice == the pan)
    if (!m_connected) return;
    m_sliceFreqHz = hz;
    m_worker->setFreqHz(static_cast<std::uint64_t>(hz));
    m_ddc->setCenterFrequencyHz(hz);
    m_ddc->setSliceFrequencyHz(hz);
    emit panCenterBandwidthChanged(QString::fromLatin1(kPanId), hz / 1e6, m_sampleRateHz / 1e6);
}

void HackRfBackend::setPanFrameRate(const QString& panId, int fps)
{
    Q_UNUSED(panId);
    // Same formula as Hl2RxDsp::setSpectrumRateFps: 0 = uncapped.
    m_spectrumIntervalMs = fps > 0 ? (1000 / fps) : 0;
}

void HackRfBackend::setPanRfGain(const QString& panId, int gainDb)
{
    Q_UNUSED(panId);
    if (!m_connected) return;
    m_vgaGainDb = std::clamp(gainDb, 0, 62);
    if (m_worker->setVgaGainDb(m_vgaGainDb)) {
        emit panRfGainChanged(QString::fromLatin1(kPanId), m_vgaGainDb);
    }
}

void HackRfBackend::setPanPreamp(const QString& panId, int step)
{
    Q_UNUSED(panId);
    if (!m_connected) return;
    m_ampEnabled = (step != 0);
    if (m_worker->setAmpEnable(m_ampEnabled)) {
        emit panPreampChanged(QString::fromLatin1(kPanId), m_ampEnabled ? 1 : 0);
    }
}

void HackRfBackend::setKeying(bool key)
{
    if (!m_connected) return;
    if (key) {
        m_arbiter->requestTx(nowMs());
    } else {
        m_arbiter->requestRx(nowMs());
    }
}

void HackRfBackend::invokeExtension(const QString& ns, const QString& verb,
                                    quint64 requestId, const QVariant& arg)
{
    if (ns != QLatin1String("hackrf")) {
        emit extensionError(requestId, tr("Unknown namespace %1").arg(ns));
        return;
    }
    if (!m_connected) {
        emit extensionError(requestId, tr("Not connected"));
        return;
    }
    if (verb == QLatin1String("lna.set")) {
        // LNA has no first-class UI slot — see the header comment on the
        // gain-model decision this backend makes (VGA -> panRfGain, AMP ->
        // panPreamp, LNA -> here).
        bool ok = false;
        const int db = arg.toInt(&ok);
        if (!ok) {
            emit extensionError(requestId, tr("lna.set requires an integer dB value"));
            return;
        }
        m_lnaGainDb = db;  // HackRfWorker::setLnaGainDb clamps to the valid 0-40/8dB steps
        if (m_worker->setLnaGainDb(m_lnaGainDb)) {
            emit extensionResult(requestId, m_lnaGainDb);
        } else {
            emit extensionError(requestId, tr("Failed to set LNA gain"));
        }
        return;
    }
    if (verb == QLatin1String("lna.get")) {
        emit extensionResult(requestId, m_lnaGainDb);
        return;
    }
    emit extensionError(requestId, tr("Unknown verb %1.%2").arg(ns, verb));
}

// ── Arbiter <-> HackRfWorker wiring ──────────────────────────────────────
//
// HackRfWorker's start/stop calls are synchronous (see its header comment),
// so each of these confirms back to the arbiter immediately rather than
// waiting for an asynchronous completion — there isn't one to wait for with
// today's libhackrf API. The arbiter's design does not assume otherwise;
// see HackRfTxRxArbiter.h's own comment on why it takes explicit confirm*()
// calls rather than owning the hackrf_* calls itself.

void HackRfBackend::onArbiterWantRxStart()
{
    m_worker->startRx();
    // No separate "confirm" needed on the way INTO Idle/RxStreaming — only
    // the pending (TxPending/RxPending) states wait for a confirmRxStopped/
    // confirmTxStopped, and wantRxStart is never emitted while entering one.
}

void HackRfBackend::onArbiterWantRxStop()
{
    m_worker->stopRx();
    m_arbiter->confirmRxStopped(nowMs());
}

void HackRfBackend::onArbiterWantTxStart()
{
    m_worker->startTx();
}

void HackRfBackend::onArbiterWantTxStop()
{
    m_worker->stopTx();
    m_arbiter->confirmTxStopped(nowMs());
}

void HackRfBackend::onArbiterTimedOut(HackRfTxRxArbiter::State pendingState)
{
    qCWarning(lcHackRf) << "HackRfBackend: RX/TX arbitration timed out waiting on"
                        << (pendingState == HackRfTxRxArbiter::State::TxPending
                                ? "RX teardown before TX" : "TX teardown before RX")
                        << "— forced back to a safe (non-transmitting) state";
}

void HackRfBackend::onArbiterTick()
{
    m_arbiter->tick(nowMs());
}

void HackRfBackend::onWorkerStreamStopped(bool wasRx, const QString& reason)
{
    qCWarning(lcHackRf) << "HackRfBackend:" << (wasRx ? "RX" : "TX")
                        << "stream stopped unexpectedly:" << reason;
    // The arbiter believes a direction is still streaming that genuinely
    // isn't (device error / unplug). There is no clean way to resynchronize
    // its state from here without risking a spurious TX start on a half-
    // reconnected device, so treat this the same as a hard disconnect
    // rather than guess.
    disconnectRadio();
    emit connectionError(tr("HackRF stream stopped unexpectedly: %1").arg(reason));
}

void HackRfBackend::onWorkerRxIqReady(QVector<std::complex<float>> iq)
{
    // Feeds HackRfDdc's decimator, which emits decimatedIqReady() — caught by
    // onDdcAudioIqReady() below (connected in the constructor) and turned into
    // demodulated audio via m_rxChannel. See the class comment.
    m_ddc->process(iq);

    // Wideband panadapter spectrum — computed once per backend from the raw
    // capture, independent of the per-slice DDC above (see the class
    // comment on why these are separate). QVector's storage is contiguous
    // for a trivial element type, so this is a view, not a copy.
    const std::span<const std::complex<float>> wideband(iq.constData(),
                                                         static_cast<std::size_t>(iq.size()));
    if (spectrumFrameDue()) {
        if (m_spectrum->process(wideband, m_specBins) > 0) {
            QByteArray frame(reinterpret_cast<const char*>(m_specBins.data()),
                             static_cast<int>(m_specBins.size() * sizeof(float)));
            emit spectrumFrameReady(0, frame);
            emit waterfallRowReady(0, frame);
            m_lastSpectrumMs = nowMs();
        }
    } else {
        // Keep the window fed without paying for a transform — see
        // Hl2RxDsp's identical reasoning in its own class comment.
        m_spectrum->accumulate(wideband);
    }
}

void HackRfBackend::emitInitialState()
{
    RadioDelta rDelta;
    rDelta.model = QStringLiteral("HackRF");
    rDelta.nickname = m_serial;
    emit radioChanged(rDelta);

    const QString panId = QString::fromLatin1(kPanId);
    emit panCenterBandwidthChanged(panId, m_sliceFreqHz / 1e6, m_sampleRateHz / 1e6);
    emit panBandwidthLimitsChanged(panId, 1.0, 6000.0);

    emit panRfGainInfoChanged(panId, 0, 62, 2);
    emit panRfGainChanged(panId, m_vgaGainDb);
    emit panPreampInfoChanged(panId, {QStringLiteral("OFF"), QStringLiteral("ON")});
    emit panPreampChanged(panId, m_ampEnabled ? 1 : 0);

    SliceDelta sDelta;
    sDelta.frequency = m_sliceFreqHz / 1e6;
    sDelta.mode = m_sliceMode;
    sDelta.filterLow = m_sliceFilterLow;
    sDelta.filterHigh = m_sliceFilterHigh;
    sDelta.modeList = QStringList{QStringLiteral("AM"), QStringLiteral("SAM"),
                                  QStringLiteral("FM"), QStringLiteral("FMN"),
                                  QStringLiteral("WFM"), QStringLiteral("USB"),
                                  QStringLiteral("LSB"), QStringLiteral("CW"),
                                  QStringLiteral("CWR")};
    sDelta.active = true;
    sDelta.panId = panId;
    emit sliceChanged(0, sDelta);
}

void HackRfBackend::rebuildRxChannel()
{
    WdspChannel::Config cfg;
    cfg.direction = WdspChannel::Direction::Receive;
    cfg.inputBlockSize = kAudioDspBlockSize;
    cfg.dspBlockSize = kAudioDspBlockSize;
    cfg.inputSampleRate = kAudioDdcRateHz;
    cfg.dspSampleRate = kAudioDdcRateHz;
    cfg.outputSampleRate = kAudioOutputRateHz;
    cfg.mode = wdspModeFromString(m_sliceMode);
    cfg.filterLowHz = m_sliceFilterLow;
    cfg.filterHighHz = m_sliceFilterHigh;
    cfg.agcMode = m_agcModeIndex;
    cfg.maximumAgcGainDb = m_agcMaxGainDb;
    // NOT the default false. WdspChannel::Config::blockForOutput's own
    // comment: false assumes a steady one-block-per-tick feed and WDSP's
    // async worker paces itself against real time; true "waits for each
    // output block (deterministic for a burst/offline feed)". HackRF
    // delivers IQ in USB-transfer-sized bursts, not a steady tick — one
    // onDdcAudioIqReady() call typically drains several WDSP blocks back to
    // back — so false is the wrong mode entirely, not just a quality trade.
    // Measured on real hardware (#42): with it false, the raw output PCM had
    // an exact-period glitch every 128 samples (2x this config's own output
    // block size) — a synchronization artifact, not real audio — audible as
    // the reported "over modulated"/distorted sound at an otherwise healthy
    // signal level and clean spectrum shape.
    cfg.blockForOutput = true;

    std::string error;
    auto channel = WdspChannel::create(cfg, &error);
    if (!channel) {
        qCWarning(lcHackRf) << "HackRfBackend: failed to create RX WDSP channel:"
                            << QString::fromStdString(error);
        m_rxChannel.reset();
        return;
    }
    m_rxChannel = std::move(channel);
    m_audioIqBuffer.clear();
}

void HackRfBackend::onDdcAudioIqReady(QVector<std::complex<float>> iq)
{
    if (!m_rxChannel) return;

    m_audioIqBuffer.insert(m_audioIqBuffer.end(), iq.begin(), iq.end());

    const std::size_t block = kAudioDspBlockSize;
    const std::size_t outN = m_rxChannel->outputBlockSize();
    if (m_audioI.size() != block) {
        m_audioI.assign(block, 0.0f);
        m_audioQ.assign(block, 0.0f);
    }
    if (m_audioLeft.size() != outN) {
        m_audioLeft.assign(outN, 0.0f);
        m_audioRight.assign(outN, 0.0f);
    }

    QByteArray pcm;
    std::size_t consumed = 0;
    while (m_audioIqBuffer.size() - consumed >= block) {
        for (std::size_t n = 0; n < block; ++n) {
            m_audioI[n] = m_audioIqBuffer[consumed + n].real();
            m_audioQ[n] = m_audioIqBuffer[consumed + n].imag();
        }
        consumed += block;

        const WdspChannel::ProcessResult res =
            m_rxChannel->processIq(m_audioI, m_audioQ, m_audioLeft, m_audioRight);
        if (res != WdspChannel::ProcessResult::Ok) {
            continue;  // underrun while the pipeline fills, etc. — no output yet
        }

        const int before = pcm.size();
        pcm.resize(before + static_cast<int>(outN * 2 * sizeof(float)));
        auto* out = reinterpret_cast<float*>(pcm.data() + before);
        for (std::size_t k = 0; k < outN; ++k) {
            out[2 * k] = m_audioLeft[k];
            out[2 * k + 1] = m_audioRight[k];
        }
    }
    if (consumed > 0) {
        m_audioIqBuffer.erase(m_audioIqBuffer.begin(),
                              m_audioIqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    }

    if (!pcm.isEmpty()) {
        emit audioFrameReady(pcm);
        emit sliceAudioFrameReady(0, pcm);
    }
}

} // namespace AetherSDR::hackrf
