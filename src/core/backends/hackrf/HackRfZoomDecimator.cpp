#include "HackRfZoomDecimator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::hackrf {

HackRfZoomDecimator::Stage HackRfZoomDecimator::makeStage(int taps)
{
    // Blackman-windowed sinc half-band: cutoff at a quarter of the input rate.
    const int mid = taps / 2;
    std::vector<double> h(static_cast<std::size_t>(taps));
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const int k = i - mid;
        const double sinc = k == 0 ? 0.5 : std::sin(std::numbers::pi * k / 2.0) / (std::numbers::pi * k);
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (taps - 1))
                       + 0.08 * std::cos(4.0 * std::numbers::pi * i / (taps - 1));
        h[static_cast<std::size_t>(i)] = sinc * w;
        sum += sinc * w;
    }
    Stage s;
    s.taps = taps;
    s.centreTap = static_cast<float>(h[static_cast<std::size_t>(mid)] / sum);   // unity gain at DC
    for (int k = 1; k <= mid; k += 2)
        s.pairTaps.push_back(static_cast<float>(h[static_cast<std::size_t>(mid + k)] / sum));
    return s;
}

HackRfZoomDecimator::HackRfZoomDecimator() = default;

void HackRfZoomDecimator::setDecimation(int decimation)
{
    int d = 1;
    while (d * 2 <= std::clamp(decimation, 1, 128))
        d *= 2;
    if (d == m_decimation && !m_stages.empty() == (d > 1))
        return;
    m_decimation = d;
    int stages = 0;
    for (int x = d; x > 1; x /= 2)
        ++stages;
    m_stages.clear();
    for (int i = 0; i < stages; ++i) {
        const int fromLast = stages - 1 - i;
        m_stages.push_back(makeStage(fromLast == 0 ? 55 : fromLast == 1 ? 23 : 15));
    }
    reset();
}

void HackRfZoomDecimator::reset()
{
    for (Stage& s : m_stages) {
        s.re.assign(static_cast<std::size_t>(s.taps - 1), 0.0f);
        s.im.assign(static_cast<std::size_t>(s.taps - 1), 0.0f);
    }
}

std::vector<std::complex<float>> HackRfZoomDecimator::process(const std::vector<std::complex<float>>& in)
{
    return process(in.data(), in.size());
}

void HackRfZoomDecimator::runStage(Stage& s, const float* inRe, const float* inIm, std::size_t n,
                                   std::vector<float>& outRe, std::vector<float>& outIm)
{
    s.re.insert(s.re.end(), inRe, inRe + n);
    s.im.insert(s.im.end(), inIm, inIm + n);
    const int kTaps = s.taps;
    const int kMid = kTaps / 2;
    const std::size_t len = s.re.size();
    outRe.clear();
    outIm.clear();
    if (len < static_cast<std::size_t>(kTaps))
        return;
    const std::size_t outs = (len - static_cast<std::size_t>(kTaps)) / 2 + 1;

    // pending[2j] -> even[j], pending[2j+1] -> odd[j]. Output m's centre tap is
    // pending[2m + mid] = odd[m + mid/2] (mid is odd); its pair taps
    // pending[2m + mid +- k] (k odd) are even[m + (mid +- k)/2].
    const std::size_t halves = len / 2 + 1;
    m_evRe.resize(halves); m_evIm.resize(halves); m_odRe.resize(halves); m_odIm.resize(halves);
    for (std::size_t j = 0; 2 * j < len; ++j) {
        m_evRe[j] = s.re[2 * j];
        m_evIm[j] = s.im[2 * j];
        if (2 * j + 1 < len) {
            m_odRe[j] = s.re[2 * j + 1];
            m_odIm[j] = s.im[2 * j + 1];
        }
    }
    outRe.assign(outs, 0.0f);
    outIm.assign(outs, 0.0f);
    float* const oRe = outRe.data();
    float* const oIm = outIm.data();
    {
        const float c = s.centreTap;
        const float* const pr = m_odRe.data() + kMid / 2;
        const float* const pi = m_odIm.data() + kMid / 2;
        for (std::size_t m = 0; m < outs; ++m) {
            oRe[m] = c * pr[m];
            oIm[m] = c * pi[m];
        }
    }
    for (std::size_t t = 0; t < s.pairTaps.size(); ++t) {
        const int k = static_cast<int>(2 * t + 1);
        const float h = s.pairTaps[t];
        const float* const hiRe = m_evRe.data() + (kMid + k) / 2;
        const float* const loRe = m_evRe.data() + (kMid - k) / 2;
        const float* const hiIm = m_evIm.data() + (kMid + k) / 2;
        const float* const loIm = m_evIm.data() + (kMid - k) / 2;
        for (std::size_t m = 0; m < outs; ++m) {
            oRe[m] += h * (hiRe[m] + loRe[m]);
            oIm[m] += h * (hiIm[m] + loIm[m]);
        }
    }
    // Keep what the next output still needs: everything from 2*outs on.
    s.re.erase(s.re.begin(), s.re.begin() + static_cast<std::ptrdiff_t>(2 * outs));
    s.im.erase(s.im.begin(), s.im.begin() + static_cast<std::ptrdiff_t>(2 * outs));
}

std::vector<std::complex<float>> HackRfZoomDecimator::process(const std::complex<float>* in, std::size_t n)
{
    if (m_decimation <= 1)
        return std::vector<std::complex<float>>(in, in + n);
    m_aRe.resize(n);
    m_aIm.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        m_aRe[i] = in[i].real();
        m_aIm[i] = in[i].imag();
    }
    for (Stage& s : m_stages) {
        runStage(s, m_aRe.data(), m_aIm.data(), m_aRe.size(), m_bRe, m_bIm);
        std::swap(m_aRe, m_bRe);
        std::swap(m_aIm, m_bIm);
    }
    std::vector<std::complex<float>> out(m_aRe.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = {m_aRe[i], m_aIm[i]};
    return out;
}

} // namespace AetherSDR::hackrf
