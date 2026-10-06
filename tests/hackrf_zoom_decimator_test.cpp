// HackRfZoomDecimator: the narrow-zoom spectrum's decimator (a cascade of
// half-band x2 stages). Pins: exact output count, a tone inside the zoomed
// view passes at ~0 dB, and a strong tone OUTSIDE the view, which a naive
// decimator would fold into it as a false signal, is rejected.
#include "core/backends/hackrf/HackRfZoomDecimator.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace AetherSDR::hackrf;

namespace {
int g_failed = 0;
void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failed;
}

std::vector<std::complex<float>> tone(double freqHz, double rateHz, std::size_t n, float amp = 1.0f)
{
    std::vector<std::complex<float>> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * std::numbers::pi * freqHz * static_cast<double>(i) / rateHz;
        v[i] = {amp * static_cast<float>(std::cos(ph)), amp * static_cast<float>(std::sin(ph))};
    }
    return v;
}

// Power (dB re a unit tone) of `freqHz` in the steady-state part of `x`.
double powerAtDb(const std::vector<std::complex<float>>& x, double freqHz, double rateHz)
{
    const std::size_t skip = x.size() / 4;      // past the filters' start-up
    std::complex<double> acc{0, 0};
    std::size_t n = 0;
    for (std::size_t i = skip; i < x.size(); ++i, ++n) {
        const double ph = -2.0 * std::numbers::pi * freqHz * static_cast<double>(i) / rateHz;
        acc += std::complex<double>(x[i]) * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    const double mag = std::abs(acc) / static_cast<double>(n);
    return 20.0 * std::log10(std::max(mag, 1e-12));
}
} // namespace

int main()
{
    const double rate = 8'000'000.0;        // narrow zoom decimates the 8 MS/s capture
    const std::size_t n = 1 << 18;

    {
        HackRfZoomDecimator d;
        d.setDecimation(1);
        const auto in = tone(100'000.0, rate, 1000);
        const auto out = d.process(in);
        check(out.size() == in.size(), "decimation 1 passes every sample through");
    }

    for (int dec : {2, 4, 8, 16, 32, 64, 128}) {
        HackRfZoomDecimator d;
        d.setDecimation(dec);
        const double outRate = rate / dec;
        const double view = outRate / 2.0;                 // displayed half-span
        const double inside = 0.3 * view;                  // well inside the view
        const double outside = view * 1.6;                 // folds to -0.4 * view naively
        const auto passOut = d.process(tone(inside, rate, n));
        d.reset();
        const auto rejectOut = d.process(tone(outside, rate, n));
        const double alias = outside - outRate;            // where it would land
        const double pass = powerAtDb(passOut, inside, outRate);
        const double rej = powerAtDb(rejectOut, alias, outRate);
        std::printf("D=%2d: out %zu samples, in-view tone %.2f dB, out-of-view tone folded %.1f dB\n",
                    dec, passOut.size(), pass, rej);
        check(passOut.size() == n / static_cast<std::size_t>(dec), "exact output count");
        check(std::fabs(pass) < 0.5, "a tone inside the zoomed view passes at ~0 dB");
        check(rej < -60.0, "a strong tone outside the view does not fold in (> 60 dB down)");
    }

    {
        HackRfZoomDecimator d;
        d.setDecimation(4);
        std::vector<std::complex<float>> all;
        // Arbitrary chunking must not change the output (state carries over).
        const auto in = tone(50'000.0, rate, 10'000);
        for (std::size_t off = 0; off < in.size(); off += 777) {
            const std::size_t len = std::min<std::size_t>(777, in.size() - off);
            const auto part = d.process(std::vector<std::complex<float>>(in.begin() + off, in.begin() + off + len));
            all.insert(all.end(), part.begin(), part.end());
        }
        HackRfZoomDecimator whole;
        whole.setDecimation(4);
        const auto ref = whole.process(in);
        bool same = all.size() == ref.size();
        for (std::size_t i = 0; same && i < ref.size(); ++i)
            same = std::abs(all[i] - ref[i]) < 1e-5f;
        check(same, "chunked input gives the same output as one block");
    }

    std::printf("%s\n", g_failed == 0 ? "hackrf_zoom_decimator_test: OK" : "hackrf_zoom_decimator_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
