#pragma once

#include "core/backends/hackrf/HackRfDdc.h"
#include "core/backends/hackrf/HackRfTxRxArbiter.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RestoredRadioState.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

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
//   - RX AUDIO/SPECTRUM ARE STILL NOT WIRED, though HackRfDdc now exists and
//     IS fed real wideband samples (HackRfWorker::rxIqReady -> HackRfDdc::
//     process(), tuned so center == the single slice frequency): there is
//     no WDSP RXA channel yet to turn HackRfDdc's correctly-tuned,
//     correctly-decimated 48 kHz IQ into demodulated audio or a spectrum
//     frame, so decimatedIqReady() currently has no consumer inside this
//     class. ddc() is exposed for exactly the same reason RtlSdrBackend
//     exposes its own ddc() accessor — real-hardware verification without
//     a full WDSP integration to build first.
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
    // RtlSdrBackend::ddc()) — see the class comment on why nothing inside
    // this class consumes decimatedIqReady() yet.
    HackRfDdc* ddc() const { return m_ddc.get(); }

private slots:
    void onArbiterWantRxStart();
    void onArbiterWantRxStop();
    void onArbiterWantTxStart();
    void onArbiterWantTxStop();
    void onArbiterTimedOut(HackRfTxRxArbiter::State pendingState);
    void onArbiterTick();
    void onWorkerStreamStopped(bool wasRx, const QString& reason);
    void onWorkerRxIqReady(QVector<std::complex<float>> iq);

private:
    void emitInitialState();
    qint64 nowMs() const { return m_clock.elapsed(); }

    bool m_connected{false};
    QString m_serial;

    // Slice 0 / pan state — single slice for now, see the class comment.
    double m_sliceFreqHz{100'000'000.0};   // 100.0 MHz FM broadcast — safe RX default
    QString m_sliceMode{QStringLiteral("WFM")};
    int m_sliceFilterLow{-100'000};
    int m_sliceFilterHigh{100'000};
    double m_sampleRateHz{8'000'000.0};
    int m_vgaGainDb{20};
    int m_lnaGainDb{16};
    bool m_ampEnabled{false};

    std::unique_ptr<HackRfWorker> m_worker;
    std::unique_ptr<HackRfDdc> m_ddc;
    std::unique_ptr<HackRfTxRxArbiter> m_arbiter;
    QElapsedTimer m_clock;   // monotonic ms source for the arbiter — started on connect
    QTimer m_arbiterTickTimer;  // polls HackRfTxRxArbiter::tick() for timeout recovery
};

} // namespace AetherSDR::hackrf
