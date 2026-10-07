#include "HackRfTxInterpolator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::hackrf {

HackRfTxInterpolator::Stage HackRfTxInterpolator::makeStage(int taps)
{
    // Blackman-windowed sinc half-band, cutoff a quarter of the OUTPUT rate,
    // unity DC gain; then x2 for the zero-stuffing.
    //
    // Zero-stuffed input u (u[2m] = x[m], u[2m+1] = 0) through g = 2h, with
    // centre c = taps/2 odd and h zero at every even offset from c but 0:
    //   y[2m]   = sum_i g[2i] x[m - i]        (the even taps: a short FIR)
    //   y[2m+1] = g[c] x[m - (c-1)/2]         (= x delayed; g[c] = 1)
    const int c = taps / 2;
    std::vector<double> h(static_cast<std::size_t>(taps));
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const int k = i - c;
        const double sinc = k == 0 ? 0.5 : std::sin(std::numbers::pi * k / 2.0) / (std::numbers::pi * k);
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (taps - 1))
                       + 0.08 * std::cos(4.0 * std::numbers::pi * i / (taps - 1));
        h[static_cast<std::size_t>(i)] = sinc * w;
        sum += sinc * w;
    }
    Stage s;
    for (int i = 0; i < taps; i += 2)
        s.evenTaps.push_back(static_cast<float>(2.0 * h[static_cast<std::size_t>(i)] / sum));
    s.delay = (c - 1) / 2;
    return s;
}

void HackRfTxInterpolator::configure(double inRateHz, double outRateHz)
{
    m_inRate = inRateHz;
    m_outRate = outRateHz;
    m_stages.clear();
    // Half-bands only up to ~384 kHz: linear interpolation's images sit at
    // multiples of its input rate with an AMPLITUDE of about (f / rate)^2, so
    // a 3 kHz voice signal from 384 kHz is already ~80 dB down, and stages at
    // MHz rates cost most of the CPU for nothing (measured: 87% of a laptop
    // E-core for FM at 16 MS/s with stages up to 6 MHz).
    constexpr double kLinearFromHz = 384'000.0;
    double r = inRateHz;
    int n = 0;
    while (r < kLinearFromHz && r * 2.0 <= outRateHz / 2.0) {
        m_stages.push_back(makeStage(n == 0 ? 55 : n == 1 ? 23 : 15));
        r *= 2.0;
        ++n;
    }
    m_midRate = r;
    m_step = m_midRate / m_outRate;
    reset();
}

void HackRfTxInterpolator::reset()
{
    for (Stage& s : m_stages)
        s.hist.assign(s.evenTaps.size(), {0.0f, 0.0f});
    // Start from the same zero history the half-bands start from, so the
    // first input already produces output and the output rate is exact.
    m_prev = m_cur = {0.0f, 0.0f};
    m_pos = 0.0;
}

void HackRfTxInterpolator::runStage(Stage& s, const std::vector<std::complex<float>>& in,
                                    std::vector<std::complex<float>>& out)
{
    const std::size_t taps = s.evenTaps.size();
    out.resize(2 * in.size());
    // Work buffer: history (oldest first) then this chunk.
    std::vector<std::complex<float>>& buf = m_b;
    buf.assign(s.hist.begin(), s.hist.end());
    buf.insert(buf.end(), in.begin(), in.end());
    for (std::size_t m = 0; m < in.size(); ++m) {
        // x is the newest input this output pair reads: in[m - 1] (the
        // history holds `taps` inputs, so one sample of latency, the same for
        // both phases); x - i is i inputs before it.
        const std::complex<float>* x = buf.data() + m + taps - 1;
        std::complex<float> acc{0.0f, 0.0f};
        for (std::size_t i = 0; i < taps; ++i)
            acc += s.evenTaps[i] * *(x - i);
        out[2 * m] = acc;
        out[2 * m + 1] = *(x - s.delay);
    }
    s.hist.assign(buf.end() - static_cast<std::ptrdiff_t>(taps), buf.end());
}

std::vector<std::complex<float>> HackRfTxInterpolator::process(const std::vector<std::complex<float>>& in)
{
    return process(in.data(), in.size());
}

std::vector<std::complex<float>> HackRfTxInterpolator::process(const std::complex<float>* in, std::size_t n)
{
    std::vector<std::complex<float>> cur(in, in + n);
    std::vector<std::complex<float>> next;
    for (Stage& s : m_stages) {
        runStage(s, cur, next);
        cur.swap(next);
    }

    // Linear step to the output rate.
    std::vector<std::complex<float>> out;
    out.reserve(static_cast<std::size_t>(static_cast<double>(cur.size()) / m_step) + 2);
    for (const std::complex<float>& x : cur) {
        m_prev = m_cur;
        m_cur = x;
        while (m_pos < 1.0) {
            const float t = static_cast<float>(m_pos);
            out.push_back(m_prev + (m_cur - m_prev) * t);
            m_pos += m_step;
        }
        m_pos -= 1.0;
    }
    return out;
}

} // namespace AetherSDR::hackrf
