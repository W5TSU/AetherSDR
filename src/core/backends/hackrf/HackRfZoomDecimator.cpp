#include "HackRfZoomDecimator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::hackrf {

namespace {
constexpr int kTaps = 55;   // 4k + 3: a half-band filter with zero even-offset taps
}

HackRfZoomDecimator::HackRfZoomDecimator()
{
    // Windowed-sinc half-band: cutoff at a quarter of the input rate.
    m_taps.resize(kTaps);
    const int mid = kTaps / 2;
    double sum = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        const int k = i - mid;
        const double sinc = k == 0 ? 0.5 : std::sin(std::numbers::pi * k / 2.0) / (std::numbers::pi * k);
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (kTaps - 1))
                       + 0.08 * std::cos(4.0 * std::numbers::pi * i / (kTaps - 1));
        m_taps[static_cast<std::size_t>(i)] = static_cast<float>(sinc * w);
        sum += sinc * w;
    }
    for (float& t : m_taps)
        t = static_cast<float>(t / sum);   // unity gain at DC
}

void HackRfZoomDecimator::setDecimation(int decimation)
{
    int d = 1;
    while (d * 2 <= std::clamp(decimation, 1, 32))
        d *= 2;
    if (d == m_decimation && !m_stages.empty() == (d > 1))
        return;
    m_decimation = d;
    int stages = 0;
    for (int x = d; x > 1; x /= 2)
        ++stages;
    m_stages.assign(static_cast<std::size_t>(stages), Stage{});
    reset();
}

void HackRfZoomDecimator::reset()
{
    for (Stage& s : m_stages) {
        s.history.assign(kTaps - 1, {0.0f, 0.0f});
        s.phase = false;
    }
}

std::vector<std::complex<float>> HackRfZoomDecimator::process(const std::vector<std::complex<float>>& in)
{
    return process(in.data(), in.size());
}

std::vector<std::complex<float>> HackRfZoomDecimator::process(const std::complex<float>* in, std::size_t n)
{
    std::vector<std::complex<float>> cur(in, in + n);
    if (m_decimation <= 1)
        return cur;
    const int mid = kTaps / 2;
    for (Stage& s : m_stages) {
        // Work buffer: the previous taps-1 inputs followed by this chunk.
        std::vector<std::complex<float>> buf;
        buf.reserve(s.history.size() + cur.size());
        buf.insert(buf.end(), s.history.begin(), s.history.end());
        buf.insert(buf.end(), cur.begin(), cur.end());
        std::vector<std::complex<float>> out;
        out.reserve(cur.size() / 2 + 1);
        for (std::size_t i = 0; i < cur.size(); ++i) {
            s.phase = !s.phase;
            if (!s.phase)
                continue;                            // keep every second output
            // The filter's newest input is buf[i + taps - 1].
            const std::complex<float>* x = buf.data() + i;
            std::complex<float> acc = m_taps[static_cast<std::size_t>(mid)] * x[mid];
            for (int k = 1; k <= mid; k += 2)        // odd offsets only: even ones are zero
                acc += m_taps[static_cast<std::size_t>(mid + k)] * (x[mid + k] + x[mid - k]);
            out.push_back(acc);
        }
        s.history.assign(buf.end() - (kTaps - 1), buf.end());
        cur = std::move(out);
    }
    return cur;
}

} // namespace AetherSDR::hackrf
