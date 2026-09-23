#include "HackRfBackend.h"
#include "core/backends/hackrf/HackRfWorker.h"

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
    m_sliceFilterLow = -100'000;
    m_sliceFilterHigh = 100'000;
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

    emitInitialState();
    emit connected();
}

void HackRfBackend::disconnectRadio()
{
    if (!m_connected) return;
    m_arbiterTickTimer.stop();
    m_worker->close();  // also stops any active RX/TX — see HackRfWorker::close()
    m_connected = false;
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

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = m_sliceMode;
    emit sliceChanged(0, delta);
}

void HackRfBackend::setSliceMode(int sliceId, const QString& mode)
{
    Q_UNUSED(sliceId);
    if (!m_connected) return;
    // Stored, not yet applied to any real demodulator — see the class
    // comment (no HackRfDdc/WDSP RXA channel yet).
    m_sliceMode = mode.trimmed().toUpper();

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = m_sliceMode;
    emit sliceChanged(0, delta);
}

void HackRfBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    Q_UNUSED(sliceId);
    if (!m_connected || lowHz >= highHz) return;
    m_sliceFilterLow = lowHz;
    m_sliceFilterHigh = highHz;

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
    Q_UNUSED(mode);
    Q_UNUSED(thresholdDb);
    // AGC is engine-side DSP once a WDSP RXA channel exists (HackRfDdc);
    // nothing to apply it to yet. Mirrors RtlSdrBackend's own Phase 1 note.
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
    // No WDSP RXA channel exists yet to consume HackRfDdc's output (see the
    // class comment) — this just runs the real DDC on real samples so the
    // decimation/tuning math is exercised end to end. decimatedIqReady()
    // is observable via ddc() for verification until a real consumer exists.
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

} // namespace AetherSDR::hackrf
