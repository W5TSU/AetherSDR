#include "HackRfBackend.h"
#include "core/backends/hackrf/HackRfIq.h"
#include "core/backends/hackrf/HackRfTuning.h"
#include "core/backends/hackrf/HackRfWorker.h"
#include "core/backends/hl2/Hl2FreqCal.h"
#include "core/backends/hl2/Hl2ModeTable.h"
#include "core/RadioSettingsScope.h"

#include <QJsonObject>
#include <QtConcurrent/QtConcurrentRun>
#include <QLoggingCategory>

#include <algorithm>
#include <chrono>
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
    , m_txDsp(std::make_unique<HackRfTxDsp>())
{
    // m_arbiter itself is (re)created fresh in connectRadio() — a stale
    // pending/timed-out state from a previous session must not leak into
    // the next one if this backend object is reused for another connect.
    // Its signals are connected there, once per instance; nothing to wire
    // here. m_worker/m_rxDsp/m_txDsp are never replaced, so their connections
    // live for the whole backend lifetime.
    connect(m_worker.get(), &HackRfWorker::streamStopped, this, &HackRfBackend::onWorkerStreamStopped);

    // THE RECEIVE CHAIN RUNS ON ITS OWN THREAD (HackRfRxDsp). The libhackrf
    // callback hands each transfer straight to its bounded queue (DIRECT: the
    // queue is thread-safe, and a queued hop through any event loop is what
    // let IQ pile up without limit), and only finished frames come back here,
    // where they are re-emitted so this backend's signals stay on its own
    // thread (backend_seam_affinity_test). ~16 blocks is ~260 ms of IQ at
    // 8 MS/s: deeper buys nothing but latency once the DSP is behind.
    m_rxDspThread = new QThread(this);
    m_rxDspThread->setObjectName(QStringLiteral("HackRfRxDsp"));
    m_rxDsp = new HackRfRxDsp(kSpectrumFftSize, kAudioDspBlockSize, /*maxQueuedBlocks=*/16);
    m_rxDsp->moveToThread(m_rxDspThread);
    m_rxDspThread->start();
    HackRfRxDsp* const rxDsp = m_rxDsp;
    connect(m_worker.get(), &HackRfWorker::rxIqReady, m_rxDsp,
            [rxDsp](QVector<std::complex<float>> iq) { rxDsp->enqueueIq(std::move(iq)); },
            Qt::DirectConnection);
    connect(m_rxDsp, &HackRfRxDsp::spectrumFrame, this, [this](const QByteArray& frame) {
        emit spectrumFrameReady(0, frame);
        emit waterfallRowReady(0, frame);
    });
    connect(m_rxDsp, &HackRfRxDsp::audioFrame, this, [this](const QByteArray& pcm) {
        emit audioFrameReady(pcm);
        emit sliceAudioFrameReady(0, pcm);
    });
    connect(m_txDsp.get(), &HackRfTxDsp::iqReady, this, &HackRfBackend::onTxDspIqReady);

    // Polls HackRfTxRxArbiter::tick() for timeout recovery. HackRfWorker's
    // start/stop calls are synchronous (see its own header comment on why
    // there's no dedicated worker thread — libhackrf's start_rx/start_tx
    // don't block, unlike RtlSdrWorker's rtlsdr_read_async), so a pending
    // transition normally resolves within the same call stack that
    // requested it; this timer exists for the case that call genuinely
    // hangs or a confirm never arrives, not for the common path.
    m_arbiterTickTimer.setInterval(50);
    connect(&m_arbiterTickTimer, &QTimer::timeout, this, &HackRfBackend::onArbiterTick);

    // Delivered on this (the GUI) thread: the watcher lives here, and only the
    // build itself runs on the pool.
    connect(&m_rxChannelBuild, &QFutureWatcherBase::finished,
            this, &HackRfBackend::onRxChannelBuilt);

    m_tuneFeedTimer.setInterval(20);
    connect(&m_tuneFeedTimer, &QTimer::timeout, this, &HackRfBackend::feedTuneCarrier);

    // CW: a fine-grained feed keeps the TX queue only ~20 ms ahead, inside the
    // 50 ms edge latency, so every timestamped edge lands in samples not yet
    // rendered. PreciseTimer: the default coarse timer may fire 5% late.
    m_cwFeedTimer.setInterval(5);
    m_cwFeedTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_cwFeedTimer, &QTimer::timeout, this, &HackRfBackend::feedCwCarrier);
    m_cwHangTimer.setSingleShot(true);
    connect(&m_cwHangTimer, &QTimer::timeout, this, [this] {
        if (!m_cwAutoKeyed) return;
        m_cwAutoKeyed = false;
        setKeying(false);
    });
}

HackRfBackend::~HackRfBackend()
{
    // Also covers a connect still building its channel: that closes the
    // device. The build itself touches nothing of this object, so it may
    // outlive it; its channel is released when the abandoned future is.
    if (m_connected || m_connectPending) disconnectRadio();
    // The device is closed, so nothing feeds the queue any more. Join the DSP
    // thread before its object goes; a WDSP block in flight finishes first.
    m_rxDspThread->quit();
    m_rxDspThread->wait();
    delete m_rxDsp;
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

    // A free-running crystal (+-20 ppm on a stock HackRF One, ~8.7 kHz at
    // 435 MHz) with no calibration register: correcting it is the client's job,
    // so the Calibration page applies. hackrf.freqcal.* implements it.
    c.hostFrequencyCalibration = true;

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
    if (m_connected || m_connectPending) disconnectRadio();

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

    // This device's frequency calibration, keyed by its serial: it describes
    // one physical crystal, so a second HackRF must not inherit the first's.
    // Loaded before the first tune so no session starts uncorrected.
    m_calibrationId = request.serialIdentity.indexLocator ? QString() : m_serial;
    m_freqCalPpb = m_calibrationId.isEmpty()
        ? 0
        : Hl2FreqCal::loadPpb(RadioSettingsScope(QStringLiteral("hackrf"), m_calibrationId));
    if (m_freqCalPpb != 0)
        qCInfo(lcHackRf) << "HackRF: frequency calibration" << m_freqCalPpb << "ppb";

    // A HackRF Pro may only use the sample rates its firmware tunes correctly.
    m_board = m_worker->isPro() ? HackRfBoard::Pro : HackRfBoard::One;

    // A restored rate outside the zoom steps snaps to the nearest one, and each
    // session starts with the panadapter centred on the slice. A restored 2 or
    // 4 MS/s becomes that span shown from the 8 MS/s capture (planForSpan).
    {
        const auto& rates = hardwareRatesHz(m_board);
        const ZoomPlan plan = planForSpan(chooseSampleRate(rates, m_sampleRateHz, m_sampleRateHz));
        m_spanHz = plan.sampleRateHz / plan.decimation;
        m_sampleRateHz = plan.sampleRateHz;
        m_rxDsp->setZoomDecimation(plan.decimation);
    }
    m_panCenterHz = m_sliceFreqHz;
    if (!m_worker->setSampleRateHz(m_sampleRateHz)
        || !tuneHardware(m_panCenterHz)
        || !m_worker->setVgaGainDb(m_vgaGainDb)
        || !m_worker->setLnaGainDb(m_lnaGainDb)
        || !m_worker->setAmpEnable(m_ampEnabled)) {
        emit connectionError(tr("HackRF opened but initial configuration failed"));
        m_worker->close();
        return;
    }

    // The DDC's wideband centre is the pan (where the LO sits); its NCO shifts
    // the slice down from wherever it sits in the span.
    m_rxDsp->ddc().setInputSampleRateHz(m_sampleRateHz);
    m_rxDsp->ddc().setCenterFrequencyHz(m_panCenterHz);
    m_rxDsp->ddc().setSliceFrequencyHz(m_sliceFreqHz);
    // Not HackRfDdc's own 48 kHz default — see kAudioDdcRateHz's comment on
    // why WFM needs a wider slice than WDSP's usual 48 kHz input.
    m_rxDsp->ddc().setOutputSampleRateHz(kAudioDdcRateHz);

    // THE RECEIVE CHAIN IS BUILT OFF THE GUI THREAD, and connected() waits for
    // it. Opening a WDSP channel measures FFTW_PATIENT plans unless the wisdom
    // cache already has them: ~30 s on a cold cache here, "up to a minute" by
    // MainWindow's own dialog copy. This used to run inline, so the whole app
    // froze for that long with "connecting" as the last log line, and an
    // operator who reasonably closed it was back to a cold cache next time:
    // the freeze repeated on every attempt and read as "cannot connect".
    // Hl2Backend learned this first (beginDspSetup); this is the same shape
    // without its multi-receiver and wire-pacing machinery, which HackRF lacks.
    //
    // The hardware is already open and configured, so a failure there is still
    // reported synchronously above. Nothing streams until completeConnect().
    m_connectPending = true;
    const quint64 generation = ++m_connectGeneration;
    const WdspChannel::Config cfg = rxChannelConfig();
    emit dspSetupProgress(tr("Preparing the receive chain…"), 0, 1);
    // Mirrored to the log: dspSetupProgress has one consumer, MainWindow, so a
    // headless run would otherwise show nothing between here and connected().
    qCInfo(lcHackRf) << "HackRF DSP setup: opening the receive chain";
    m_rxChannelBuild.setFuture(QtConcurrent::run([generation, cfg] {
        // ---- POOL THREAD ---- Touches nothing of the backend: only cfg, by
        // value. WdspChannel::create serialises on the process-wide FFTW
        // planner lock itself.
        QElapsedTimer clock;
        clock.start();
        RxChannelBuild build;
        build.generation = generation;
        std::string error;
        build.channel = WdspChannel::create(cfg, &error);
        build.error = QString::fromStdString(error);
        build.elapsedMs = clock.elapsed();
        return build;
    }));
}

void HackRfBackend::onRxChannelBuilt()
{
    const RxChannelBuild build = m_rxChannelBuild.result();
    // Superseded or abandoned: a disconnect (or a newer connect) bumped the
    // generation after this build started. Its channel is released with
    // `build`. That disconnect already emitted dspSetupFinished.
    if (!m_connectPending || build.generation != m_connectGeneration) {
        qCInfo(lcHackRf) << "HackRF DSP setup: discarding a receive chain built for"
                         << "an abandoned connect";
        return;
    }
    m_connectPending = false;
    emit dspSetupFinished();

    if (!build.channel) {
        qCWarning(lcHackRf) << "HackRfBackend: failed to create RX WDSP channel:" << build.error;
        m_worker->close();
        emit connectionError(tr("HackRF opened but its receive chain could not be prepared: %1")
                                 .arg(build.error));
        return;
    }
    qCInfo(lcHackRf) << "HackRF DSP setup: receive chain ready in" << build.elapsedMs << "ms";

    // The channel was built from the state at connectRadio(). Re-apply the
    // live values in case anything changed while it was building; each call is
    // a cheap no-op when it did not.
    m_rxDsp->installChannel(build.channel, rxSettings());

    completeConnect();
}

void HackRfBackend::completeConnect()
{
    HackRfTxDsp::Config txCfg;
    txCfg.outputSampleRateHz = m_sampleRateHz;   // TX and RX share HackRF's one clock
    m_txDsp->configure(txCfg);
    // The RF Power slider's drive. RadioModel also pushes it on connect, but
    // the value it last set is applied here so the first key-down never runs
    // at libhackrf's default TX gain.
    m_worker->setTxVgaGainDb(txVgaGainDbForPowerPercent(m_txPowerPercent));
    m_transmitting = false;

    m_connected = true;
    m_clock.start();
    m_arbiterTickTimer.start();
    // A fresh instance, not a reset on the old one — QObject subclasses
    // aren't copy-assignable, and this also means each connect gets its own
    // arbiter with no risk of double-connecting signals onto one that's
    // already wired (the old instance, and its connections, are simply
    // destroyed here).
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
    m_tuneFeedTimer.stop();
    m_tuning = false;
    m_cwFeedTimer.stop();
    m_cwHangTimer.stop();
    m_cwAutoKeyed = false;
    // Every disconnect invalidates a channel build still in flight; it cannot
    // be cancelled (WDSP's OpenChannel does not return early), so it runs to
    // completion and onRxChannelBuilt() discards it.
    ++m_connectGeneration;
    if (m_connectPending) {
        // Abandoned mid-setup: the device is open but never streamed. Close it,
        // end the setup phase the dialog is showing, and report the session
        // over, as Hl2Backend does for a disconnect during its DSP setup.
        m_connectPending = false;
        m_worker->close();
        emit dspSetupFinished();
        emit disconnected();
        return;
    }
    if (!m_connected) return;
    m_arbiterTickTimer.stop();
    m_worker->close();  // also stops any active RX/TX — see HackRfWorker::close()
    m_connected = false;
    // No RX IQ can reach it once the worker is closed, but drop it anyway
    // rather than leave a channel from the last session sitting idle — the
    // next connectRadio() builds a fresh one regardless.
    m_rxDsp->clearChannel();
    emit disconnected();
}

void HackRfBackend::setSliceFrequency(int sliceId, double hz)
{
    Q_UNUSED(sliceId);  // single slice
    if (!m_connected) return;
    // Inside the span only the DDC's NCO moves: no hardware retune, no
    // waterfall jump. Outside it the pan recentres on the slice.
    bool panMoved = false;
    const PanSlice r = tuneSlice({m_panCenterHz, m_sliceFreqHz}, m_sampleRateHz, hz, panMoved);
    applyPanSlice(r.panHz, r.sliceHz, panMoved, /*sliceMoved=*/true);
}

void HackRfBackend::applyPanSlice(double panHz, double sliceHz, bool panMoved, bool sliceMoved)
{
    m_panCenterHz = panHz;
    m_sliceFreqHz = sliceHz;
    // The hardware sits on the pan for RX and on the slice for TX
    // (onArbiterWantTxStart); retune whichever applies right now.
    if (m_transmitting)
        tuneHardware(m_sliceFreqHz);
    else if (panMoved)
        tuneHardware(m_panCenterHz);
    m_rxDsp->ddc().setCenterFrequencyHz(m_panCenterHz);
    m_rxDsp->ddc().setSliceFrequencyHz(m_sliceFreqHz);
    // THE PAN MODEL MUST FOLLOW EVERY LO MOVE. An earlier version left the
    // waterfall's centre label stale while the hardware followed the VFO, and
    // the peak drifted away from the cursor by however far the operator had
    // retuned (#42). The label is the LO, so it is re-emitted whenever the LO
    // moves -- and, now that the slice moves inside the span, only then.
    if (panMoved) {
        emit panCenterBandwidthChanged(QString::fromLatin1(kPanId), m_panCenterHz / 1e6,
                                       m_spanHz / 1e6);
    }
    if (sliceMoved) {
        SliceDelta delta;
        delta.frequency = m_sliceFreqHz / 1e6;
        delta.mode = m_sliceMode;
        emit sliceChanged(0, delta);
    }
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
    m_rxDsp->applySettings(rxSettings());   // mode, THEN filter, on the DSP thread

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
    m_rxDsp->applySettings(rxSettings());

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
    m_rxDsp->applySettings(rxSettings());
}

void HackRfBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(panId);
    if (!m_connected) return;
    if (intent == PanCenterIntent::Range) {
        // The centre riding along with a zoom: the zoom's anchor (Ctrl+wheel keeps
        // the frequency under the pointer still), never a retune. Followed as
        // far as the slice stays inside the CAPTURE, which on a narrow zoom is
        // far wider than the view, so in practice exactly as the GUI asked.
        const PanSlice r = rangePan({m_panCenterHz, m_sliceFreqHz}, m_sampleRateHz, hz);
        applyPanSlice(r.panHz, r.sliceHz, /*panMoved=*/true, /*sliceMoved=*/false);
        return;
    }
    // A drag: move the view, keep the slice unless it would leave the span.
    bool sliceMoved = false;
    const PanSlice r = dragPan({m_panCenterHz, m_sliceFreqHz}, m_sampleRateHz, hz, sliceMoved);
    applyPanSlice(r.panHz, r.sliceHz, /*panMoved=*/true, sliceMoved);
}

void HackRfBackend::setPanBandwidth(const QString& panId, double hz)
{
    Q_UNUSED(panId);
    if (!m_connected) return;
    const double span = chooseSampleRate(zoomSpansHz(m_board), m_spanHz, hz);
    const ZoomPlan plan = planForSpan(span);
    auto reassert = [this] {
        // The pan is re-told the real span so the zoom control snaps back to it.
        emit panCenterBandwidthChanged(QString::fromLatin1(kPanId), m_panCenterHz / 1e6,
                                       m_spanHz / 1e6);
    };
    if (span == m_spanHz) {
        reassert();
        return;
    }
    if (plan.sampleRateHz != m_sampleRateHz) {
        // A hardware rate change: refused mid-transmission or mid-switch (the
        // TX modulator runs at this rate), and done with RX stopped.
        const bool rxStreaming = m_arbiter
            && m_arbiter->state() == HackRfTxRxArbiter::State::RxStreaming;
        if (m_keyed || !rxStreaming) {
            reassert();
            return;
        }
        m_worker->stopRx();
        if (!m_worker->setSampleRateHz(plan.sampleRateHz)) {
            m_worker->setSampleRateHz(m_sampleRateHz);   // back to what worked
            m_worker->startRx();
            reassert();
            return;
        }
        m_sampleRateHz = plan.sampleRateHz;
        m_rxDsp->ddc().setInputSampleRateHz(m_sampleRateHz);
        HackRfTxDsp::Config txCfg = m_txDsp->config();
        txCfg.outputSampleRateHz = m_sampleRateHz;   // TX and RX share HackRF's one clock
        m_txDsp->configure(txCfg);
        m_worker->startRx();
    }
    // Narrow spans only change the spectrum's decimation: no RX restart.
    m_rxDsp->setZoomDecimation(plan.decimation);
    m_spanHz = span;

    // Zooming never retunes the slice: if it would leave the CAPTURE (a rate
    // change narrowed it), the view recentres on it. Off-screen inside the
    // capture is fine -- it still has audio -- so a zoom otherwise goes exactly
    // where the GUI anchored it. The span changed even if the centre did not: always re-report it.
    bool viewMoved = false;
    const PanSlice r = applySpan({m_panCenterHz, m_sliceFreqHz}, m_sampleRateHz, viewMoved);
    applyPanSlice(r.panHz, r.sliceHz, /*panMoved=*/true, /*sliceMoved=*/false);
    qCInfo(lcHackRf) << "HackRF: span" << m_spanHz / 1e3 << "kHz (rate"
                     << m_sampleRateHz / 1e6 << "MS/s, spectrum decimation" << plan.decimation << ")";
}

void HackRfBackend::setPanFrameRate(const QString& panId, int fps)
{
    Q_UNUSED(panId);
    // Same formula as Hl2RxDsp::setSpectrumRateFps: 0 = uncapped.
    m_rxDsp->setSpectrumIntervalMs(fps > 0 ? (1000 / fps) : 0);
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
    // While transmitting the amp stays off (ampEnabledFor); the setting is kept
    // and the next RX start applies it.
    if (m_transmitting || m_worker->setAmpEnable(ampEnabledFor(m_ampEnabled, false))) {
        emit panPreampChanged(QString::fromLatin1(kPanId), m_ampEnabled ? 1 : 0);
    }
}

void HackRfBackend::setPanIfGain(const QString& panId, int gainDb)
{
    Q_UNUSED(panId);
    if (!m_connected) return;
    m_lnaGainDb = clampLnaGainDb(gainDb);
    if (m_worker->setLnaGainDb(m_lnaGainDb)) {
        emit panIfGainChanged(QString::fromLatin1(kPanId), m_lnaGainDb);
    }
}

void HackRfBackend::setTxPower(int percent)
{
    m_txPowerPercent = std::clamp(percent, 0, 100);
    // While tuning the hardware runs at TUNE power; the new RF Power is kept
    // and restored when TUNE ends.
    if (!m_connected || m_tuning) return;   // otherwise applied in completeConnect()
    m_worker->setTxVgaGainDb(txVgaGainDbForPowerPercent(m_txPowerPercent));
}

bool HackRfBackend::isCwMode() const
{
    const QString m = m_sliceMode.trimmed().toUpper();
    return m == QLatin1String("CW") || m == QLatin1String("CWR")
        || m == QLatin1String("CWU") || m == QLatin1String("CWL");
}

void HackRfBackend::setCwKeying(bool down, bool breakIn, int breakInDelayMs)
{
    if (!m_connected) return;
    if (down && !isCwMode()) {
        qCWarning(lcHackRf) << "HackRF CW key ignored outside CW mode:" << m_sliceMode;
        return;
    }
    m_cwHangTimer.stop();
    if (down && !m_keyed) {
        // Nothing to key inside without a PTT: break-in raises one, otherwise
        // the edge has no transmission to belong to.
        if (!breakIn) return;
        setKeying(true);
        m_cwAutoKeyed = true;
    }
    if (!m_keyed) return;   // a key-up after the PTT already dropped
    const double nowS = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_cwTx.keyEdge(down, nowS);
    if (!down && m_cwAutoKeyed) {
        // The last element plays out 50 ms (latency) + 5 ms (ramp) after its
        // key-up, behind up to 20 ms of queue: a shorter hang would cut it off
        // when the unkey clears the TX queue.
        constexpr int kMinHangMs = 100;
        m_cwHangTimer.start(std::max(kMinHangMs, std::clamp(breakInDelayMs, 0, 2000)));
    }
}

// Keeps ~20 ms of CW ahead in the TX queue: inside the 50 ms edge latency, so
// every timestamped edge lands in samples not yet rendered.
void HackRfBackend::feedCwCarrier()
{
    if (!m_keyed || m_tuning || !isCwMode()) return;
    const int block = std::max(1, static_cast<int>(m_sampleRateHz / 500.0));   // 2 ms
    for (int guard = 0; guard < 20
         && shouldFeedTuneCarrier(m_worker->txQueueDepth(), m_sampleRateHz, 20); ++guard) {
        QVector<std::complex<float>> iq(block);
        m_cwTx.render(iq.data(), static_cast<std::size_t>(block));
        m_worker->submitTxIq(iq);
    }
}

void HackRfBackend::setTune(bool on, int tunePowerPercent)
{
    if (!m_connected) return;
    if (on) {
        m_tuning = true;
        const int percent = tunePowerPercent >= 0 ? tunePowerPercent : m_txPowerPercent;
        m_worker->setTxVgaGainDb(txVgaGainDbForPowerPercent(percent));
        setKeying(true);
        feedTuneCarrier();          // prime the queue before the first timer tick
        m_tuneFeedTimer.start();
    } else {
        m_tuneFeedTimer.stop();
        m_tuning = false;
        setKeying(false);
        m_worker->setTxVgaGainDb(txVgaGainDbForPowerPercent(m_txPowerPercent));
    }
}

// Keeps ~100 ms of carrier queued: enough to ride out a late tick, short
// enough that an unkey stops the carrier promptly.
void HackRfBackend::feedTuneCarrier()
{
    if (!m_tuning || !m_keyed) return;
    const int rate = m_txDsp->config().audioSampleRateHz;
    const std::vector<float> silence(static_cast<std::size_t>(std::max(1, rate / 50)), 0.0f);  // 20 ms
    for (int guard = 0; guard < 10
         && shouldFeedTuneCarrier(m_worker->txQueueDepth(), m_sampleRateHz, 100); ++guard) {
        m_txDsp->processAudioBlock(silence);
    }
}

bool HackRfBackend::tuneHardware(double trueHz)
{
    return m_worker->setFreqHz(correctedTuneHz(trueHz, m_freqCalPpb));
}

// Same shape as Hl2Backend::applyFreqCalPpb: clamp, persist per device unless
// this is a live trim, then re-tune so the change is heard at once instead of
// at the next dial movement.
void HackRfBackend::applyFreqCalPpb(int ppb, bool persist)
{
    const int clamped = Hl2FreqCal::clampPpb(ppb);
    if (persist) {
        if (m_calibrationId.isEmpty()) {
            qCWarning(lcHackRf) << "HackRF: not persisting frequency calibration —"
                                << "no stable device serial; applying for this session only";
        } else {
            Hl2FreqCal::savePpb(RadioSettingsScope(QStringLiteral("hackrf"), m_calibrationId),
                                clamped);
        }
    }
    if (clamped == m_freqCalPpb)
        return;
    m_freqCalPpb = clamped;
    qCInfo(lcHackRf) << "HackRF: frequency calibration" << clamped << "ppb";
    if (m_connected)
        tuneHardware(m_transmitting ? m_sliceFreqHz : m_panCenterHz);
}

void HackRfBackend::setKeying(bool key)
{
    if (!m_connected) return;
    const bool wasKeyed = m_keyed;
    m_keyed = key;
    if (key) {
        // A new transmission in CW: fresh CW timeline, and the carrier feed
        // runs for the whole PTT (silent until the first paddle edge).
        if (!wasKeyed && isCwMode()) {
            HackRfCwTx::Config cw;
            cw.sampleRateHz = m_sampleRateHz;
            cw.amplitude = 1.0f;   // the FM modulator's unit magnitude: same power per RF Power
            m_cwTx.configure(cw);
            m_cwFeedTimer.start();
        }
        m_arbiter->requestTx(nowMs());
    } else {
        m_cwFeedTimer.stop();
        m_cwHangTimer.stop();
        m_cwAutoKeyed = false;
        m_cwTx.reset();
        m_arbiter->requestRx(nowMs());
        // Drop accumulated phase/resample state so the NEXT transmission's
        // carrier starts clean rather than carrying this one's tail —
        // mirrors Hl2TxDsp::reset()'s own reasoning for the same edge.
        m_txDsp->reset();
    }
}

void HackRfBackend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                                  bool clientLeveled)
{
    Q_UNUSED(clientLeveled);   // no ALC/makeup gain in this modulator to gate — see HackRfTxDsp.h
    // Only actually modulate while genuinely keyed — see the header comment
    // on why this guard exists (mirrors Hl2Backend::submitTxAudio's own).
    // While tuning, the carrier is the only signal: microphone audio would
    // modulate it.
    // In CW the keyed carrier is the signal; microphone audio would FM it.
    if (!m_connected || !m_keyed || m_tuning || isCwMode() || int16Stereo.isEmpty()) return;

    if (sampleRateHz != m_txDsp->config().audioSampleRateHz) {
        // Stated rather than silently resampled — a mismatch would
        // transmit at the wrong pitch, exactly like Hl2Backend's own
        // refusal for the identical situation.
        HackRfTxDsp::Config cfg = m_txDsp->config();
        cfg.audioSampleRateHz = sampleRateHz;
        m_txDsp->configure(cfg);
    }

    // Interleaved stereo to mono, matching Hl2Backend::submitTxAudio's own
    // conversion exactly: AudioEngine duplicates the mic across both
    // channels, so averaging is right for that and still sane if they differ.
    const auto* pcm = reinterpret_cast<const qint16*>(int16Stereo.constData());
    const int frames = static_cast<int>(int16Stereo.size() / sizeof(qint16)) / 2;
    std::vector<float> mono(static_cast<std::size_t>(frames));
    for (int n = 0; n < frames; ++n) {
        const float l = static_cast<float>(pcm[2 * n]) / 32768.0f;
        const float r = static_cast<float>(pcm[2 * n + 1]) / 32768.0f;
        mono[static_cast<std::size_t>(n)] = 0.5f * (l + r);
    }
    m_txDsp->processAudioBlock(mono);
}

int HackRfBackend::finishTxAudio()
{
    if (!m_connected || m_sampleRateHz <= 0.0) return 0;
    const std::size_t queued = m_worker->txQueueDepth();
    return static_cast<int>(static_cast<double>(queued) * 1000.0 / m_sampleRateHz);
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
        // Clamped HERE so the reply and the slider report what the hardware took.
        m_lnaGainDb = clampLnaGainDb(db);
        if (m_worker->setLnaGainDb(m_lnaGainDb)) {
            emit panIfGainChanged(QString::fromLatin1(kPanId), m_lnaGainDb);
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
    // Frequency calibration, the same verbs and ppb units Hl2Backend serves, so
    // the Calibration page and the `freqcal` bridge verb drive either family.
    if (verb == QLatin1String("freqcal.set") || verb == QLatin1String("freqcal.set_live")) {
        applyFreqCalPpb(arg.toInt(), /*persist=*/verb == QLatin1String("freqcal.set"));
        if (requestId != 0)
            emit extensionResult(requestId, QVariant(m_freqCalPpb));
        return;
    }
    if (verb == QLatin1String("freqcal.get")) {
        if (requestId != 0)
            emit extensionResult(requestId, QVariantMap{{QStringLiteral("ppb"), m_freqCalPpb}});
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
    m_transmitting = false;
    tuneHardware(m_panCenterHz);   // RX: the LO back to the pan centre
    m_worker->setAmpEnable(ampEnabledFor(m_ampEnabled, /*transmitting=*/false));
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
    // Amp OFF before the first TX sample: it is one switch for both
    // directions, and the operator's RX Preamp must not add ~14 dB to TX.
    m_transmitting = true;
    // TX: the carrier is at DC, so the LO goes to the slice frequency (it sits
    // at the pan centre for RX, possibly MHz away). A PLL retune of a few ms,
    // inside the RX->TX switch the arbiter is already making.
    tuneHardware(m_sliceFreqHz);
    m_worker->setAmpEnable(ampEnabledFor(m_ampEnabled, /*transmitting=*/true));
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

void HackRfBackend::emitInitialState()
{
    RadioDelta rDelta;
    rDelta.model = QStringLiteral("HackRF");
    rDelta.nickname = m_serial;
    emit radioChanged(rDelta);

    const QString panId = QString::fromLatin1(kPanId);
    emit panCenterBandwidthChanged(panId, m_panCenterHz / 1e6, m_spanHz / 1e6);
    // The zoom range, in MHz of span: 62.5 kHz (decimated) to 20 MHz (the top
    // sample rate). (It said 1 to 6000, the TUNING range, which no zoom could
    // honour.)
    emit panBandwidthLimitsChanged(panId, zoomSpansHz(m_board).front() / 1e6,
                                   zoomSpansHz(m_board).back() / 1e6);

    emit panRfGainInfoChanged(panId, 0, 62, 2);
    emit panRfGainChanged(panId, m_vgaGainDb);
    emit panPreampInfoChanged(panId, {QStringLiteral("OFF"), QStringLiteral("ON")});
    emit panPreampChanged(panId, m_ampEnabled ? 1 : 0);
    emit panIfGainInfoChanged(panId, 0, 40, 8, QStringLiteral("LNA"));
    emit panIfGainChanged(panId, m_lnaGainDb);

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
    // HackRF's one slice is also its transmit slice. Without this RadioModel
    // had no TX slice at all, so every key request (PTT, MOX, TUNE) was refused
    // with "No transmit slice is assigned." before reaching the backend.
    sDelta.txSlice = true;
    sDelta.panId = panId;
    emit sliceChanged(0, sDelta);
}

HackRfRxDsp::RxSettings HackRfBackend::rxSettings() const
{
    HackRfRxDsp::RxSettings s;
    s.mode = wdspModeFromString(m_sliceMode);
    s.filterLowHz = m_sliceFilterLow;
    s.filterHighHz = m_sliceFilterHigh;
    s.agcMode = m_agcModeIndex;
    s.agcMaxGainDb = m_agcMaxGainDb;
    return s;
}

WdspChannel::Config HackRfBackend::rxChannelConfig() const
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
    // HackRfRxDsp::onDecimated() call typically drains several WDSP blocks back to
    // back — so false is the wrong mode entirely, not just a quality trade.
    // Measured on real hardware (#42): with it false, the raw output PCM had
    // an exact-period glitch every 128 samples (2x this config's own output
    // block size) — a synchronization artifact, not real audio — audible as
    // the reported "over modulated"/distorted sound at an otherwise healthy
    // signal level and clean spectrum shape.
    cfg.blockForOutput = true;
    return cfg;
}

void HackRfBackend::onTxDspIqReady(QVector<std::complex<float>> iq)
{
    // No arbiter-state gate here: HackRfWorker's own TX queue drains only
    // while actually streaming, and stopTx() clears it on unkey (see its
    // own comment) — a brief RxPending/TxPending window just means this IQ
    // sits queued a little longer, not that it transmits early.
    m_worker->submitTxIq(iq);
}

} // namespace AetherSDR::hackrf
