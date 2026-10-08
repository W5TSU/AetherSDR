// Every receive mode the HackRF offers, through the real receive chain.
//
// The HackRF listed nine modes, and the WDSP channel behind it can demodulate
// more (DIGU, DIGL, DSB, RTTY, DFM), so those are added. Each mode is pinned
// here with no hardware: a synthetic signal goes through HackRfRxDsp (DDC ->
// WDSP), set up exactly as HackRfBackend sets it up, and the demodulated audio
// must carry the right tone:
//   - the sideband modes pass their own sideband and reject the other one;
//   - CW is heard at the operator's pitch. It came out at 0 Hz (silent) before
//     the backend had a BFO: the passband was centred on the carrier with
//     nothing to lift it into the audio range;
//   - AM, SAM, DSB and every FM flavour recover their modulating tone.
#include "core/backends/hackrf/HackRfBackend.h"
#include "core/backends/hackrf/HackRfRxDsp.h"
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <mutex>
#include <vector>

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

constexpr double kInRate = 2'000'000.0;    // the capture: small, so the test is quick
constexpr double kCentre = 100'000'000.0;  // LO
constexpr double kSlice = kCentre + 200'000.0;
constexpr double kAudioRate = 24'000.0;
constexpr int kPitch = 600;

// Power of `f` in the audio relative to the audio's total power, in dB.
// The share of the audio's energy within +-60 Hz of `f`, in dB: a Hann-
// windowed DFT over that band against the whole windowed signal (Parseval).
// A band, not one bin, so an AGC still settling (the level steps by tens of
// dB going into WFM) cannot smear the tone out of the measurement. DC is
// excluded: WDSP's AM and SAM detectors pass the carrier through as DC.
double toneShareDb(const std::vector<float>& x, double f)
{
    const std::size_t n = x.size();
    if (n < 2) return -300.0;
    double mean = 0.0;
    for (float v : x) mean += v;
    mean /= static_cast<double>(n);
    std::vector<double> w(n);
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n - 1));
        w[i] = (static_cast<double>(x[i]) - mean) * hann;
        total += w[i] * w[i];
    }
    const double binHz = kAudioRate / static_cast<double>(n);
    const int k0 = static_cast<int>(std::ceil((f - 60.0) / binHz));
    const int k1 = static_cast<int>(std::floor((f + 60.0) / binHz));
    double band = 0.0;
    for (int k = std::max(1, k0); k <= k1; ++k) {
        std::complex<double> acc{0, 0};
        const double step = -2.0 * M_PI * k / static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i)
            acc += w[i] * std::complex<double>(std::cos(step * i), std::sin(step * i));
        band += 2.0 * std::norm(acc);   // the mirrored negative-frequency bin too
    }
    return 10.0 * std::log10(std::max(band, 1e-30) / std::max(static_cast<double>(n) * total, 1e-30));
}

// The signal, as complex baseband relative to the SLICE, at time t.
using Signal = std::function<std::complex<double>(double t)>;
Signal tone(double offsetHz, double amp = 0.05)
{
    return [=](double t) { return amp * std::exp(std::complex<double>(0, 2 * M_PI * offsetHz * t)); };
}
Signal sum(Signal a, Signal b) { return [=](double t) { return a(t) + b(t); }; }
Signal am(double fm, double depth = 0.6)
{
    return [=](double t) { return 0.05 * (1.0 + depth * std::cos(2 * M_PI * fm * t)); };
}
Signal fm(double fmHz, double deviationHz)
{
    return [=](double t) {
        return 0.05 * std::exp(std::complex<double>(0, deviationHz / fmHz * std::sin(2 * M_PI * fmHz * t)));
    };
}

struct Case {
    const char* mode;
    Signal signal;
    double wantHz;      // must dominate the audio
    double rejectHz;    // must be far down (0 = no rejection check)
    const char* what;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The mode list the VFO shows.
    const QStringList modes = HackRfBackend::supportedModes();
    for (const char* m : {"USB", "LSB", "CW", "CWR", "AM", "SAM", "DSB", "FM", "FMN", "DFM",
                          "WFM", "DIGU", "DIGL", "RTTY"})
        check(modes.contains(QString::fromLatin1(m)), (std::string("mode list offers ") + m).c_str());

    // CW geometry: the BFO leans the detector by the pitch, sideband by mode.
    check(HackRfBackend::cwBfoHz(QStringLiteral("CW"), kPitch) == kPitch, "CW BFO is +pitch");
    check(HackRfBackend::cwBfoHz(QStringLiteral("CWR"), kPitch) == -kPitch, "CWR BFO is -pitch");
    check(HackRfBackend::cwBfoHz(QStringLiteral("USB"), kPitch) == 0.0, "no BFO outside CW");

    QThread dspThread;
    auto* dsp = new HackRfRxDsp(2048, 1024, /*maxQueuedBlocks=*/100'000);
    dsp->moveToThread(&dspThread);
    QObject::connect(&dspThread, &QThread::finished, dsp, &QObject::deleteLater);
    dspThread.start();
    dsp->ddc().setInputSampleRateHz(kInRate);
    dsp->ddc().setCenterFrequencyHz(kCentre);
    dsp->ddc().setOutputSampleRateHz(384'000.0);

    std::mutex audioMutex;
    std::vector<float> audio;   // left channel
    QObject::connect(dsp, &HackRfRxDsp::audioFrame, dsp, [&](const QByteArray& pcm) {
        const auto* f = reinterpret_cast<const float*>(pcm.constData());
        const std::lock_guard<std::mutex> lock(audioMutex);
        for (int i = 0; i + 1 < pcm.size() / int(sizeof(float)); i += 2)
            audio.push_back(f[i]);
    }, Qt::DirectConnection);

    // The channel HackRfBackend builds (rxChannelConfig).
    WdspChannel::Config cfg;
    cfg.direction = WdspChannel::Direction::Receive;
    cfg.inputBlockSize = 1024;
    cfg.dspBlockSize = 1024;
    cfg.inputSampleRate = 384'000;
    cfg.dspSampleRate = 384'000;
    cfg.outputSampleRate = 24'000;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 100;
    cfg.filterHighHz = 2900;
    cfg.blockForOutput = true;
    std::string err;
    std::shared_ptr<WdspChannel> channel = WdspChannel::create(cfg, &err);
    check(channel != nullptr, "a receive channel opens");
    if (!channel) return 1;
    dsp->installChannel(channel, HackRfBackend::rxSettingsFor(QStringLiteral("USB"), 100, 2900, kPitch, 3, 65.0));

    const std::vector<Case> cases = {
        {"USB",  sum(tone(1000), tone(-1500)), 1000, 1500, "USB passes the upper sideband, rejects the lower"},
        {"LSB",  sum(tone(-1000), tone(1500)), 1000, 1500, "LSB passes the lower sideband, rejects the upper"},
        {"DIGU", sum(tone(1500), tone(-1000)), 1500, 1000, "DIGU passes the upper sideband, rejects the lower"},
        {"DIGL", sum(tone(-1500), tone(1000)), 1500, 1000, "DIGL passes the lower sideband, rejects the upper"},
        {"RTTY", sum(tone(2125), tone(1500)), 2125, 1500, "RTTY passes the 2125 Hz mark, rejects outside its window"},
        {"CW",   sum(tone(0), tone(1000)), kPitch, kPitch + 1000, "CW: a carrier on the dial is heard at the pitch"},
        {"CWR",  sum(tone(0), tone(-1000)), kPitch, kPitch + 1000, "CWR: a carrier on the dial is heard at the pitch"},
        {"DSB",  sum(tone(1000), tone(-1000)), 1000, 0, "DSB recovers a double-sideband tone"},
        {"AM",   am(1000), 1000, 0, "AM recovers the modulating tone"},
        {"SAM",  am(1000), 1000, 0, "SAM recovers the modulating tone"},
        {"FM",   fm(1000, 3000), 1000, 0, "FM recovers the modulating tone"},
        {"FMN",  fm(1000, 2500), 1000, 0, "FMN recovers the modulating tone"},
        {"DFM",  fm(1000, 3000), 1000, 0, "DFM recovers the modulating tone"},
        {"WFM",  fm(1000, 25000), 1000, 0, "WFM recovers the modulating tone"},   // inside its +-40 kHz passband
    };

    double t0 = 0.0;
    for (const Case& c : cases) {
        const QString mode = QString::fromLatin1(c.mode);
        const auto [lo, hi] = HackRfBackend::defaultPassbandForMode(mode);
        // As the backend does it: the DDC sits a BFO below the dial in CW.
        dsp->ddc().setSliceFrequencyHz(kSlice - HackRfBackend::cwBfoHz(mode, kPitch));
        dsp->applySettings(HackRfBackend::rxSettingsFor(mode, lo, hi, kPitch, 3, 65.0));
        QThread::msleep(50);

        // 1.6 s of signal: the first 0.8 s lets the AGC and filters settle.
        const std::size_t block = 65'536;
        const std::size_t blocks = static_cast<std::size_t>(1.6 * kInRate) / block;
        {
            const std::lock_guard<std::mutex> lock(audioMutex);
            audio.clear();
        }
        for (std::size_t b = 0; b < blocks; ++b) {
            QVector<std::complex<float>> iq(static_cast<int>(block));
            for (std::size_t i = 0; i < block; ++i) {
                const double t = t0 + static_cast<double>(b * block + i) / kInRate;
                const std::complex<double> s = c.signal(t)
                    * std::exp(std::complex<double>(0, 2 * M_PI * (kSlice - kCentre) * t));
                iq[static_cast<int>(i)] = {static_cast<float>(s.real()), static_cast<float>(s.imag())};
            }
            dsp->enqueueIq(std::move(iq));
        }
        t0 += static_cast<double>(blocks * block) / kInRate;

        // Wait for the audio of all of it.
        const std::size_t want = static_cast<std::size_t>(1.4 * kAudioRate);
        QElapsedTimer wait;
        wait.start();
        for (;;) {
            {
                const std::lock_guard<std::mutex> lock(audioMutex);
                if (audio.size() >= want) break;
            }
            if (wait.elapsed() > 60'000) break;
            QThread::msleep(20);
        }
        std::vector<float> tail;
        {
            const std::lock_guard<std::mutex> lock(audioMutex);
            if (audio.size() > static_cast<std::size_t>(0.8 * kAudioRate))
                tail.assign(audio.begin() + static_cast<std::ptrdiff_t>(0.8 * kAudioRate), audio.end());
        }
        const double wantDb = tail.empty() ? -300 : toneShareDb(tail, c.wantHz);
        const double rejDb = (tail.empty() || c.rejectHz == 0) ? -300 : toneShareDb(tail, c.rejectHz);
        std::printf("  %-4s: %zu audio samples, %.0f Hz at %+.1f dB of the audio", c.mode, tail.size(), c.wantHz, wantDb);
        if (c.rejectHz != 0) std::printf(", %.0f Hz at %+.1f dB", c.rejectHz, rejDb);
        std::printf("\n");
        check(wantDb > -3.0 && (c.rejectHz == 0 || rejDb < -30.0), c.what);
    }

    dspThread.quit();
    dspThread.wait();
    std::printf("%s\n", g_failed == 0 ? "hackrf_rx_modes_test: OK" : "hackrf_rx_modes_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
