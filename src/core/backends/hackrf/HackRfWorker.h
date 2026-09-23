#pragma once

#include "core/backends/hackrf/HackRfIq.h"

// hackrf_transfer can't be forward-declared like hackrf_device below: it's
// `typedef struct { ... } hackrf_transfer;` in hackrf.h — an ANONYMOUS
// struct, not a named tag — so it has no elaborated-type-specifier form
// (`struct hackrf_transfer;`) compatible with the real typedef. The static
// callbacks below must also match hackrf_sample_block_cb_fn's exact
// signature (int(*)(hackrf_transfer*)) to be passed directly to
// hackrf_start_rx/hackrf_start_tx as C function pointers, so the real type
// has to be visible here, not just in the .cpp.
#include <hackrf.h>

#include <QObject>
#include <QMutex>
#include <QString>
#include <QVector>

#include <atomic>
#include <complex>
#include <cstdint>
#include <deque>

namespace AetherSDR::hackrf {

// Owns one hackrf_device* handle — the only class permitted to touch it,
// mirroring RtlSdrWorker's ownership rule for rtlsdr_dev (#42 design doc).
//
// Unlike RtlSdrWorker, this is NOT a QThread::run() blocking-call host, and
// (unlike an earlier draft of this comment claimed) it doesn't need a
// dedicated worker thread at all: rtlsdr_read_async() blocks the calling
// thread until cancelled, which is why RtlSdrWorker's whole run() loop
// lives inside that call and its control calls need a cancel/retune/
// restart dance to reach the device around it. HackRF's API has no
// equivalent hazard — hackrf_start_rx()/hackrf_start_tx() return
// immediately and hand the USB transfer loop to libhackrf/libusb's own
// internal thread, and every control call here (open/close/set_freq/gain)
// is a plain, fast USB control transfer safe to call from whatever thread
// owns this object (HackRfBackend calls it directly, from the same thread
// IRadioBackend itself runs on — see HackRfBackend.cpp).
//
// RX and TX are mutually exclusive on one HackRF's single USB pipe (the
// design doc's RX/TX arbitration section) — this class does NOT enforce
// that itself. It is a dumb transport: HackRfBackend's arbitration state
// machine (not yet built — #42) decides when to call startRx()/stopRx() vs
// startTx()/stopTx(). Calling hackrf_start_rx while TX is active (or vice
// versa) is a caller bug this class surfaces as a failed bool return and a
// hackrf_error logged, not something it silently corrects.
class HackRfWorker : public QObject {
    Q_OBJECT

public:
    explicit HackRfWorker(QObject* parent = nullptr);
    ~HackRfWorker() override;

    // Opens the first HackRF found, or the one matching serial if it's
    // non-empty. Calls the process-wide hackrf_init() first — safe to do
    // per-instance because exactly one HackRfWorker is expected to exist at
    // a time (one physical device per backend instance, same assumption
    // RtlSdrBackend makes); a second concurrent instance would need
    // reference-counted init/exit, which is out of scope until multi-device
    // support is ever designed.
    bool open(const QString& serial = QString());
    void close();
    bool isOpen() const { return m_device != nullptr; }

    // Frequency/sample-rate/gain may be set before OR during streaming —
    // HackRF's firmware supports live retuning, unlike the RTL2832U tuner
    // RtlSdrWorker has to cancel/restart around. hackrf_start_tx's own doc
    // still recommends setting these before the first start, since a
    // never-set value keeps an unknown prior state.
    bool setFreqHz(std::uint64_t hz);
    bool setSampleRateHz(double hz);
    bool setLnaGainDb(int requestedDb);    // clamped via clampLnaGainDb()
    bool setVgaGainDb(int requestedDb);    // clamped via clampVgaGainDb()
    bool setTxVgaGainDb(int requestedDb);  // clamped via clampTxVgaGainDb()
    bool setAmpEnable(bool on);

    bool startRx();
    bool stopRx();
    bool isRxStreaming() const { return m_rxStreaming.load(std::memory_order_relaxed); }

    bool startTx();
    bool stopTx();
    bool isTxStreaming() const { return m_txStreaming.load(std::memory_order_relaxed); }

    // Producer side of the TX sample queue — called from whatever thread
    // owns the WDSP TXA channel output, once HackRfBackend exists (the
    // design doc's IRadioBackend::submitTxAudio -> WDSP -> IQ path).
    // Samples are consumed by the TX callback as fast as USB drains them;
    // HackRfBackend is expected to pace submission to the stream's actual
    // sample rate, the same assumption Hl2Backend::queueTxIq() makes of its
    // callers — this queue has no backpressure signal and will grow
    // unbounded if fed faster than the transfer drains it.
    void submitTxIq(const QVector<std::complex<float>>& iq);

signals:
    // RX samples, wideband and unfiltered — HackRfDdc (not yet built) is
    // what tunes a slice out of this. Delivered via a queued connection
    // from the libhackrf callback thread (not a QThread AetherSDR owns) to
    // whatever thread the receiver lives on.
    void rxIqReady(QVector<std::complex<float>> iq);
    // A stream stopped for a reason other than stopRx()/stopTx() being
    // called — device error, unplug, or the callback itself returning
    // non-zero. wasRx distinguishes direction for HackRfBackend's
    // arbitration state machine.
    void streamStopped(bool wasRx, const QString& reason);
    // The TX queue was empty when the callback needed samples; the
    // transmitted block was silence-padded. Worth surfacing distinctly
    // from a hard error — this is an underrun, not a device fault.
    void txUnderrun();

private:
    static int rxCallback(hackrf_transfer* transfer);
    static int txCallback(hackrf_transfer* transfer);
    int handleRxTransfer(hackrf_transfer* transfer);
    int handleTxTransfer(hackrf_transfer* transfer);

    hackrf_device* m_device{nullptr};
    std::atomic<bool> m_rxStreaming{false};
    std::atomic<bool> m_txStreaming{false};

    // Producer (submitTxIq(), any thread) / consumer (txCallback(),
    // libhackrf's internal thread) queue. A plain mutex-protected deque
    // rather than a lock-free ring buffer: v1's TX modes are FM/CW only, an
    // order of magnitude below HackRF's wideband RX sample rate, so lock
    // contention here is not the concern a wideband path would have — see
    // HackRfDdc (RX side) for where that concern actually applies.
    QMutex m_txQueueMutex;
    std::deque<std::complex<float>> m_txQueue;
};

} // namespace AetherSDR::hackrf
