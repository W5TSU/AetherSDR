// HackRfRxDsp: HackRF's receive chain (DDC, spectrum, WDSP demod) on its own
// thread, fed through a bounded queue.
//
// The bug this exists for: the chain ran on the GUI thread and waited on WDSP
// for every block, so when WFM demodulation ran even slightly slower than real
// time the GUI stalled for longer and longer (1 s .. 27 s observed) and IQ
// piled up without limit. Pinned here, with no hardware:
//   - enqueueIq() returns without doing DSP work, and audio is produced on the
//     DSP thread, never the caller's;
//   - a busy DSP thread makes the queue DROP the oldest blocks (and count them)
//     instead of growing;
//   - spectrum frames come out; clearChannel() stops audio.
#include "core/backends/hackrf/HackRfRxDsp.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::hackrf;

namespace {

int g_failed = 0;
void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    std::fflush(stdout);
    if (!ok)
        ++g_failed;
}

template <typename Pred>
bool spinUntil(Pred done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

// One USB-transfer-sized block at 8 MS/s: a tone 10 kHz above centre.
QVector<std::complex<float>> toneBlock(std::size_t n, double& phase)
{
    QVector<std::complex<float>> v;
    v.reserve(static_cast<int>(n));
    const double step = 2.0 * M_PI * 10'000.0 / 8'000'000.0;
    for (std::size_t i = 0; i < n; ++i) {
        v.append(std::complex<float>(0.3f * static_cast<float>(std::cos(phase)),
                                     0.3f * static_cast<float>(std::sin(phase))));
        phase += step;
    }
    return v;
}

constexpr std::size_t kBlock = 131'072;     // one 256 KiB libhackrf transfer
constexpr std::size_t kMaxQueued = 8;

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QThread dspThread;
    auto* dsp = new HackRfRxDsp(/*fftSize=*/2048, /*audioBlock=*/1024, kMaxQueued);
    dsp->moveToThread(&dspThread);
    QObject::connect(&dspThread, &QThread::finished, dsp, &QObject::deleteLater);
    dspThread.start();

    dsp->ddc().setInputSampleRateHz(8'000'000.0);
    dsp->ddc().setCenterFrequencyHz(100'000'000.0);
    dsp->ddc().setSliceFrequencyHz(100'000'000.0);
    dsp->ddc().setOutputSampleRateHz(384'000.0);
    dsp->setSpectrumIntervalMs(0);   // every block may produce a frame

    std::atomic<int> audioFrames{0}, spectrumFrames{0};
    std::atomic<bool> audioOnCaller{false};
    QThread* const caller = QThread::currentThread();
    QObject::connect(dsp, &HackRfRxDsp::audioFrame, dsp, [&](const QByteArray&) {
        if (QThread::currentThread() == caller) audioOnCaller = true;
        ++audioFrames;
    }, Qt::DirectConnection);
    QObject::connect(dsp, &HackRfRxDsp::spectrumFrame, dsp, [&](const QByteArray&) {
        ++spectrumFrames;
    }, Qt::DirectConnection);

    // A real WFM channel, as HackRfBackend builds it. The suite's isolation TU
    // bounds the FFTW planner, so this opens in well under a second.
    WdspChannel::Config cfg;
    cfg.direction = WdspChannel::Direction::Receive;
    cfg.inputBlockSize = 1024;
    cfg.dspBlockSize = 1024;
    cfg.inputSampleRate = 384'000;
    cfg.dspSampleRate = 384'000;
    cfg.outputSampleRate = 24'000;
    cfg.mode = WdspChannel::Mode::Wbfm;
    cfg.filterLowHz = -40'000;
    cfg.filterHighHz = 40'000;
    cfg.blockForOutput = true;
    std::string err;
    std::shared_ptr<WdspChannel> channel = WdspChannel::create(cfg, &err);
    check(channel != nullptr, "a WFM channel opens");
    if (!channel) return 1;
    HackRfRxDsp::RxSettings settings{WdspChannel::Mode::Wbfm, -40'000, 40'000, 3, 65.0};
    dsp->installChannel(channel, settings);

    // ---- enqueue never does the work itself ----
    double phase = 0.0;
    qint64 worstEnqueueUs = 0;
    for (int i = 0; i < 6; ++i) {
        const auto block = toneBlock(kBlock, phase);
        QElapsedTimer t;
        t.start();
        dsp->enqueueIq(block);
        worstEnqueueUs = std::max<qint64>(worstEnqueueUs, t.nsecsElapsed() / 1000);
        QThread::msleep(20);   // ~ the real 16 ms transfer cadence
    }
    std::printf("worst enqueueIq() %lld us\n", static_cast<long long>(worstEnqueueUs));
    check(worstEnqueueUs < 5'000, "enqueueIq() returns without running the DSP chain");
    check(spinUntil([&] { return audioFrames > 0; }, 10'000), "demodulated audio is produced");
    check(!audioOnCaller, "...on the DSP thread, never the caller's");
    check(spectrumFrames > 0, "spectrum frames are produced");

    // ---- a busy DSP thread drops, it does not pile up ----
    // Deterministic: let the earlier backlog finish, then occupy the DSP thread
    // and wait until it CONFIRMS it is occupied before flooding the queue.
    spinUntil([] { return false; }, 1500);
    std::atomic<bool> release{false}, blocking{false};
    QMetaObject::invokeMethod(dsp, [&] {
        blocking = true;
        while (!release) QThread::msleep(1);
    }, Qt::QueuedConnection);
    check(spinUntil([&] { return blocking.load(); }, 5000), "the DSP thread is occupied");
    const quint64 droppedBefore = dsp->droppedBlocks();
    for (int i = 0; i < 30; ++i)
        dsp->enqueueIq(toneBlock(kBlock, phase));
    const quint64 dropped = dsp->droppedBlocks() - droppedBefore;
    std::printf("dropped %llu of 30 while the DSP thread was busy\n",
                static_cast<unsigned long long>(dropped));
    check(dropped == 30 - kMaxQueued, "the queue holds at most its bound and drops the rest");
    release = true;

    // ---- clearChannel stops audio ----
    spinUntil([] { return false; }, 1000);   // let the backlog drain
    dsp->clearChannel();
    spinUntil([] { return false; }, 200);
    const int framesAtClear = audioFrames;
    for (int i = 0; i < 4; ++i)
        dsp->enqueueIq(toneBlock(kBlock, phase));
    spinUntil([] { return false; }, 1000);
    check(audioFrames == framesAtClear, "no audio after clearChannel()");

    // ---- spectrum frame rate follows the SAMPLE clock, not the transfer size ----
    // At 2 MS/s one 131072-sample transfer is 65.5 ms; gating one frame per
    // transfer capped the waterfall at ~15 fps on a narrow zoom. With a 33 ms
    // interval, 4 transfers (262 ms) must yield ~8 frames, not 4.
    {
        dsp->clearChannel();                 // spectrum only
        spinUntil([] { return false; }, 300);
        dsp->ddc().setInputSampleRateHz(2'000'000.0);
        dsp->setSpectrumIntervalMs(33);
        spinUntil([] { return false; }, 100);
        const int before = spectrumFrames;
        for (int i = 0; i < 4; ++i) dsp->enqueueIq(toneBlock(kBlock, phase));
        spinUntil([] { return false; }, 1500);
        const int frames = spectrumFrames - before;
        std::printf("spectrum frames for 262 ms at 2 MS/s: %d\n", frames);
        check(frames >= 7 && frames <= 9, "~one spectrum frame per 33 ms of SAMPLES at 2 MS/s");
    }

    // ---- narrow zoom: the spectrum comes from the decimated stream ----
    {
        dsp->setZoomDecimation(8);           // 2 MS/s -> 250 kHz view
        spinUntil([] { return false; }, 100);
        const int before = spectrumFrames;
        QByteArray lastFrame;
        auto conn = QObject::connect(dsp, &HackRfRxDsp::spectrumFrame, dsp,
                                     [&](const QByteArray& f) { lastFrame = f; }, Qt::DirectConnection);
        for (int i = 0; i < 4; ++i) dsp->enqueueIq(toneBlock(kBlock, phase));
        spinUntil([] { return false; }, 1500);
        QObject::disconnect(conn);
        const int frames = spectrumFrames - before;
        std::printf("zoomed (D=8) spectrum frames for 262 ms: %d\n", frames);
        check(frames >= 6 && frames <= 9, "a zoomed view keeps ~one frame per 33 ms");
        check(lastFrame.size() == 2048 * static_cast<int>(sizeof(float)),
              "a zoomed frame is a full 2048-bin spectrum");
        dsp->setZoomDecimation(1);
    }

    dspThread.quit();
    dspThread.wait();
    std::printf("%s\n", g_failed == 0 ? "hackrf_rx_dsp_test: OK" : "hackrf_rx_dsp_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
