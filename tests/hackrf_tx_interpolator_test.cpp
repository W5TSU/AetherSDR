// HackRfTxInterpolator: transmit baseband (24 or 48 kHz) up to the HackRF's
// sample rate (8 or 16 MS/s) without spurious images.
//
// Why it exists: the FM modulator holds each audio sample across the output
// samples it spans, which is harmless there (the phase is integrated at the
// full rate), but SSB, AM and DSB are built at a low rate as IQ, and holding
// IQ samples puts a copy of the signal every 48 kHz only ~13 dB down: a
// spurious emission on the air. Pinned here:
//   - a tone keeps its frequency and level (0 dB);
//   - every image the cascade could leave (each stage's rate, and the final
//     linear step's) is at least 60 dB down;
//   - the output rate is exact over time, and chunking does not change the
//     output.
#include "core/backends/hackrf/HackRfTxInterpolator.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hackrf;

namespace {

int g_failed = 0;
void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failed;
}

std::vector<std::complex<float>> tone(double f, double rate, std::size_t n, double amp = 0.5)
{
    std::vector<std::complex<float>> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * M_PI * f * static_cast<double>(i) / rate;
        v[i] = {static_cast<float>(amp * std::cos(ph)), static_cast<float>(amp * std::sin(ph))};
    }
    return v;
}

// Power of the complex tone at f (Hz) over x, relative to amp^2, in dB. The
// window is a whole number of seconds' worth of integer-Hz cycles, so every
// integer-Hz tone is orthogonal and a rectangular window is exact.
double toneDb(const std::vector<std::complex<float>>& x, std::size_t from, std::size_t n,
              double f, double rate, double amp = 0.5)
{
    std::complex<double> acc{0, 0};
    const double step = -2.0 * M_PI * f / rate;
    std::complex<double> rot{1, 0};
    const std::complex<double> dRot{std::cos(step), std::sin(step)};
    for (std::size_t i = 0; i < n; ++i) {
        acc += std::complex<double>(x[from + i]) * rot;
        rot *= dRot;
        if ((i & 1023) == 0) rot /= std::abs(rot);
    }
    const double mag = std::abs(acc) / static_cast<double>(n);
    return 20.0 * std::log10(std::max(mag / amp, 1e-15));
}

void runCase(double inRate, double outRate)
{
    std::printf("-- %.0f Hz -> %.1f MS/s\n", inRate, outRate / 1e6);
    HackRfTxInterpolator interp;
    interp.configure(inRate, outRate);
    const std::size_t nIn = static_cast<std::size_t>(inRate * 0.2);   // 0.2 s
    const double f = 1000.0;
    const auto out = interp.process(tone(f, inRate, nIn));

    const double expected = static_cast<double>(nIn) * outRate / inRate;
    std::printf("   %zu in -> %zu out (expected %.1f)\n", nIn, out.size(), expected);
    check(std::fabs(static_cast<double>(out.size()) - expected) <= 1.0, "the output rate is exact");

    // Measure over 0.1 s after the filters' start-up: every tone and image
    // here is a multiple of 1 kHz, so a whole number of cycles.
    const std::size_t from = static_cast<std::size_t>(0.05 * outRate);
    const std::size_t n = static_cast<std::size_t>(0.1 * outRate);
    if (out.size() < from + n) { check(false, "enough output to measure"); return; }
    const double wanted = toneDb(out, from, n, f, outRate);
    std::printf("   tone at %+.0f Hz: %+.2f dB\n", f, wanted);
    check(std::fabs(wanted) < 0.5, "a tone keeps its frequency and level");

    // Every image: each stage's input rate r leaves copies at +-r + f, and the
    // linear step at multiples of its input rate.
    double worst = -300.0, worstAt = 0.0;
    for (double r = inRate; r <= outRate / 2.0 + 1.0; r *= 2.0) {
        for (int k : {1, 2, 3}) {
            for (double img : {k * r + f, -k * r + f}) {
                if (std::fabs(img) >= outRate / 2.0) continue;
                const double db = toneDb(out, from, n, img, outRate);
                if (db > worst) { worst = db; worstAt = img; }
            }
        }
    }
    std::printf("   worst image %+.1f dB at %+.0f Hz\n", worst, worstAt);
    check(worst < -60.0, "every image at least 60 dB down");

    // Chunking.
    HackRfTxInterpolator a, b;
    a.configure(inRate, outRate);
    b.configure(inRate, outRate);
    const auto in = tone(1700.0, inRate, 20'000);
    std::vector<std::complex<float>> chunked;
    for (std::size_t off = 0; off < in.size(); off += 333) {
        const std::size_t len = std::min<std::size_t>(333, in.size() - off);
        const auto part = a.process(in.data() + off, len);
        chunked.insert(chunked.end(), part.begin(), part.end());
    }
    const auto whole = b.process(in);
    bool same = chunked.size() == whole.size();
    for (std::size_t i = 0; same && i < whole.size(); ++i)
        same = std::abs(chunked[i] - whole[i]) < 1e-5f;
    check(same, "chunked input gives the same output as one block");
}

} // namespace

int main()
{
    runCase(48'000.0, 8'000'000.0);    // SSB (Hl2TxDsp's 48 kHz) at 8 MS/s
    runCase(48'000.0, 16'000'000.0);   // ... at 16 MS/s
    runCase(24'000.0, 8'000'000.0);    // AM / DSB from 24 kHz audio
    runCase(48'000.0, 20'000'000.0);   // a HackRF One at 20 MS/s
    std::printf("%s\n", g_failed == 0 ? "hackrf_tx_interpolator_test: OK" : "hackrf_tx_interpolator_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
