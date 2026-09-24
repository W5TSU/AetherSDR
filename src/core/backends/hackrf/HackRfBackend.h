#pragma once

#include "core/backends/hackrf/HackRfDdc.h"
#include "core/backends/hackrf/HackRfTxRxArbiter.h"
#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RestoredRadioState.h"
#include "core/dsp/WdspChannel.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <complex>
#include <memory>
#include <utility>
#include <vector>

namespace AetherSDR::hackrf {

class HackRfWorker;

// IRadioBackend implementation for HackRF One / HackRF Pro (#42 design doc).
// Highly experimental — full design at
// docs/superpowers/specs/2026-09-16-hackrf-backend-design.md.
//
// THIS INCREMENT'S SCOPE, stated plainly rather than left to be discovered
// by reading code that doesn't do what the design doc's v1 describes yet:
//
//   - Single slice, single panadapter. The design doc's v1 wants multi-slice
//     receive within HackRF's wide capture, which needs HackRfDdc (a
//     multi-instance DDC against one shared wideband feed — explicitly the
//     one RX-side piece with no precedent in this codebase). HackRfDdc does
//     not exist yet, so neither does multi-slice; this backend is RtlSdrBackend
//     -shaped (one slice) until it does.
//   - The RX/TX arbitration is REAL: setKeying() drives HackRfTxRxArbiter,
//     which genuinely tears down RX before starting TX and rebuilds it after,
//     using HackRfWorker underneath. Keying this backend really does switch
//     the hardware between RX and TX.
//   - TX AUDIO IS NOT WIRED. submitTxAudio()/finishTxAudio() are left at
//     IRadioBackend's no-op defaults — there is no WDSP TXA channel yet to
//     turn mic/CW audio into modulated IQ. Keying transmits SILENCE (the
//     empty-queue path in HackRfWorker::handleTxTransfer, which
//     silence-pads and emits txUnderrun()), not the operator's audio. This
//     is deliberate, not an oversight: building a correct FM/CW modulator
//     is its own scope, tracked as a remaining #42 item, and a backend that
//     silently claimed to transmit audio it doesn't produce would be a
//     worse failure than one that plainly does nothing yet.
//   - RX AUDIO IS WIRED via a single WdspChannel (Direction::Receive) —
//     the same reusable WDSP wrapper Hl2RxDsp uses, not a copy of it: HL2's
//     class carries a great deal that is specific to ITS wire (notch
//     database, ADC-pairing meter, per-block spectrum, HPSDR handedness
//     quirks), none of which applies here, so this backend talks to
//     WdspChannel directly instead of dragging Hl2RxDsp's HL2-specific
//     surface along. HackRfDdc's decimatedIqReady() (48 kHz IQ) feeds a
//     fixed-size buffer that drains into WdspChannel::processIq() exactly
//     WDSP's configured block at a time (Hl2RxDsp::processIqBlock's own
//     buffering technique, generalized) — HackRF's IQ arrives in
//     USB-transfer-sized chunks, not WDSP's block size, so the buffer is
//     what makes the two independent. Output is 24 kHz interleaved float32
//     stereo, matching AudioEngine's native RX rate exactly like Hl2RxDsp's
//     own choice (see its Config::audioSampleRateHz comment) — no
//     resampling needed downstream. ddc() is still exposed for the same
//     real-hardware-verification reason RtlSdrBackend exposes its own.
//   - AGC ceiling (0 dB per 0..100 threshold unit, i.e. *0.6) is the same
//     starting map Hl2Backend::setSliceAgc() uses, chosen there from
//     measurements on HL2's own front end — not yet independently
//     calibrated against HackRF's, so treat it as a starting point.
//   - RX SPECTRUM/WATERFALL ARE WIRED, though: hl2::Hl2Spectrum (a raw-IQ
//     panadapter FFT engine that is completely radio-family-agnostic
//     despite its namespace — see its own header comment, "a raw-IQ
//     backend that streams its own spectra runs it here instead," which is
//     precisely this backend's situation) computes ONE wideband spectrum
//     per backend from HackRfWorker::rxIqReady directly, independent of
//     HackRfDdc's per-slice narrowband decimation. Rate-gated the same way
//     Hl2RxDsp gates it: skip the whole FFT (not just the emit) when a
//     frame isn't due, but keep the accumulator fed so the next due frame
//     completes from contiguous recent samples.
//
// None of the above blocks correctness of what IS implemented: capabilities
// declaration, connect/disconnect lifecycle, frequency/gain control, and the
// RX/TX arbitration are complete and meant to be exercised (short of a real
// on-air transmit test, deliberately not run against this dev machine's
// HackRf Pro without a proper antenna/filter setup — see the design doc's
// own safety framing).
class HackRfBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit HackRfBackend(QObject* parent = nullptr);
    ~HackRfBackend() override;

    // ---- IRadioBackend ----
    RadioCapabilities capabilities() const override;
    bool ownsRxAudio() const override { return true; }  // host-demodulated once HackRfDdc exists; no VITA-49 either way

    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override { return m_connected; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanFrameRate(const QString& panId, int fps) override;

    // VGA (baseband) gain, 0-62dB/2dB steps — the continuous slider. LNA and
    // the front-end AMP map to setPanPreamp() below; there is no third
    // continuous-gain slot in the seam today (the design doc's "open
    // question" this backend resolves: two of HackRF's three gain stages
    // get first-class UI via the existing panRfGain/panPreamp mechanisms;
    // the third (LNA) is reachable via invokeExtension("hackrf","lna.set",…)
    // rather than inventing a new seam capability for one backend).
    void setPanRfGain(const QString& panId, int gainDb) override;
    // The front-end RF amp near the antenna port: OFF/ON, ~11dB when on
    // (hackrf.h's own documented figure) — a genuinely two-position control,
    // which is exactly what panPreamp is for (see IRadioBackend.h's own
    // comment on why Icom's preamp uses this instead of a continuous slider).
    void setPanPreamp(const QString& panId, int step) override;

    void setKeying(bool key) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    static QString familyName() { return QStringLiteral("hackrf"); }

    // Single-slice DDC, exposed for real-hardware verification (mirrors
    // RtlSdrBackend::ddc()).
    HackRfDdc* ddc() const { return m_ddc.get(); }

    // Operator mode string -> WDSP demod mode. Delegates to
    // hl2::modeFromString for every name the two backends share (it is
    // already unit-tested via hl2_mode_table_test and is purely a string
    // table — nothing HL2-specific about the mapping itself), adding only
    // the two spellings unique to this backend's own declared mode list
    // (FMN, CWR) that table does not recognize. Public and static so it is
    // directly unit-testable without a connected backend (hackrf_backend_test).
    static WdspChannel::Mode wdspModeFromString(const QString& mode) noexcept;

    // Operator AGC mode string -> WDSP RXA AGC mode index. Same small map as
    // Hl2Backend's own (private, so not reusable directly) wdspAgcMode():
    // off=0, slow=2, fast=4, anything else (including "med" and unknown
    // strings) falls back to WDSP's own medium default of 3.
    static int wdspAgcModeFromString(const QString& mode) noexcept;

    // Mode-appropriate default RX passband, Hz relative to carrier.
    // Delegates to hl2::defaultPassbandForMode for every name the two
    // backends share, adding only this backend's two unique spellings
    // (FMN, CWR) by aliasing them onto the shared table's FM/CW buckets.
    // Public and static for the same testability reason as the two maps
    // above.
    static std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept;

private slots:
    void onArbiterWantRxStart();
    void onArbiterWantRxStop();
    void onArbiterWantTxStart();
    void onArbiterWantTxStop();
    void onArbiterTimedOut(HackRfTxRxArbiter::State pendingState);
    void onArbiterTick();
    void onWorkerStreamStopped(bool wasRx, const QString& reason);
    void onWorkerRxIqReady(QVector<std::complex<float>> iq);
    void onDdcAudioIqReady(QVector<std::complex<float>> iq);

private:
    void emitInitialState();
    // (Re)builds m_rxChannel from the current mode/filter/AGC state. Called
    // on connect and on any mode change — WdspChannel::setMode() handles a
    // mode change at runtime, so this full rebuild is for the one time a
    // fresh channel must exist at all (connect), not the ongoing path.
    void rebuildRxChannel();
    qint64 nowMs() const { return m_clock.elapsed(); }
    // Gates the wideband spectrum FFT the same way Hl2RxDsp::spectrumFrameDue()
    // does — see the class comment.
    bool spectrumFrameDue() const
    {
        return m_spectrumIntervalMs <= 0
            || (nowMs() - m_lastSpectrumMs) >= m_spectrumIntervalMs;
    }

    bool m_connected{false};
    QString m_serial;

    // Slice 0 / pan state — single slice for now, see the class comment.
    double m_sliceFreqHz{100'000'000.0};   // 100.0 MHz FM broadcast — safe RX default
    QString m_sliceMode{QStringLiteral("WFM")};
    int m_sliceFilterLow{-40'000};   // matches defaultPassbandForMode("WFM")
    int m_sliceFilterHigh{40'000};
    double m_sampleRateHz{8'000'000.0};
    int m_vgaGainDb{20};
    int m_lnaGainDb{16};
    bool m_ampEnabled{false};

    std::unique_ptr<HackRfWorker> m_worker;
    std::unique_ptr<HackRfDdc> m_ddc;
    std::unique_ptr<hl2::Hl2Spectrum> m_spectrum;
    std::vector<float> m_specBins;   // reused output buffer for m_spectrum->process()
    int m_spectrumIntervalMs{33};    // ~30 fps default, matches RtlSdrDdc's own default
    qint64 m_lastSpectrumMs{0};

    // RX audio (WDSP RXA channel) — see the class comment.
    std::unique_ptr<WdspChannel> m_rxChannel;
    int m_agcModeIndex{3};       // WDSP AGC mode; 3 = medium, WDSP's own default
    // 0..100 threshold * 1.0 dB, at the slice default of 65 -- NOT Hl2Backend's
    // 0.6 map. Measured on real hardware (#42): the WBFM discriminator's own
    // raw output sits far quieter than SSB/AM's mixer output (~-36 dBFS peak
    // pre-AGC on a solid broadcast signal, captured via automation's
    // capture_audio raw tap), so the *0.6 map that avoids clipping on HL2's
    // much hotter SSB signal leaves WFM at 39 dB max gain -- nowhere near
    // enough headroom to reach a normal listening level. 1:1 puts the default
    // of 65 at 65 dB, closer to WDSP's own 120 dB ceiling; recalibrate this
    // per-mode (narrow modes may want HL2's original map back) once a WDSP
    // RXA channel exists for more than WFM to compare against.
    double m_agcMaxGainDb{65.0};
    std::vector<std::complex<float>> m_audioIqBuffer;  // awaiting a full WDSP block
    std::vector<float> m_audioI, m_audioQ;             // deinterleaved input scratch
    std::vector<float> m_audioLeft, m_audioRight;      // WdspChannel output scratch
    std::unique_ptr<HackRfTxRxArbiter> m_arbiter;
    QElapsedTimer m_clock;   // monotonic ms source for the arbiter AND the spectrum gate — started on connect
    QTimer m_arbiterTickTimer;  // polls HackRfTxRxArbiter::tick() for timeout recovery
};

} // namespace AetherSDR::hackrf
