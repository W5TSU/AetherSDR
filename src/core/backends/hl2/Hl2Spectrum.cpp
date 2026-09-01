#include "core/backends/hl2/Hl2Spectrum.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>

namespace AetherSDR::hl2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Hl2Spectrum::Hl2Spectrum(int fftSize) : m_fftSize(fftSize < 2 ? 2 : fftSize)
{
    m_acc.reserve(static_cast<std::size_t>(m_fftSize));
    m_window.resize(static_cast<std::size_t>(m_fftSize));
    double sum = 0.0;
    for (int n = 0; n < m_fftSize; ++n) {
        m_window[static_cast<std::size_t>(n)] =
            0.5 * (1.0 - std::cos(2.0 * kPi * n / (m_fftSize - 1)));   // Hanning
        sum += m_window[static_cast<std::size_t>(n)];
    }
    m_coherentGain = sum / 2.0;
    // Guard the per-bin normalization divisor (used in process() below). A
    // degenerate window sums to 0 — e.g. a length-2 Hanning, whose two endpoints
    // are both 0 — which would make the magnitude divide produce inf. Unreachable
    // at the production fftSize (1024) but cheap to make safe; with an all-zero
    // window the input is zeroed anyway, so the bins come out 0 rather than inf.
    if (m_coherentGain < 1e-9)
        m_coherentGain = 1.0;

    m_in = fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(m_fftSize));
    m_out = fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(m_fftSize));
    m_plan = fftw_plan_dft_1d(m_fftSize, static_cast<fftw_complex*>(m_in),
                              static_cast<fftw_complex*>(m_out), FFTW_FORWARD, FFTW_ESTIMATE);
}

Hl2Spectrum::~Hl2Spectrum()
{
    if (m_plan) fftw_destroy_plan(static_cast<fftw_plan>(m_plan));
    if (m_in) fftw_free(m_in);
    if (m_out) fftw_free(m_out);
}

int Hl2Spectrum::process(std::span<const std::complex<float>> iq, std::vector<float>& binsDbfs)
{
    int frames = 0;
    for (const auto& s : iq) {
        m_acc.push_back(s);
        if (static_cast<int>(m_acc.size()) == m_fftSize) {
            computeFrame(binsDbfs);
            m_acc.clear();
            ++frames;
        }
    }
    return frames;
}

void Hl2Spectrum::accumulate(std::span<const std::complex<float>> iq)
{
    m_acc.insert(m_acc.end(), iq.begin(), iq.end());
    // Hold at most fftSize - 1: see the header for why exactly-full would wedge
    // process()'s boundary check.
    const std::size_t keep = static_cast<std::size_t>(m_fftSize) - 1;
    if (m_acc.size() > keep) {
        m_acc.erase(m_acc.begin(),
                    m_acc.end() - static_cast<std::ptrdiff_t>(keep));
    }
}

void Hl2Spectrum::computeFrame(std::vector<float>& binsDbfs)
{
    // Remove the frame's DC offset (the ADC offset lives on I in direct
    // sampling), then apply the window.
    double meanRe = 0.0, meanIm = 0.0;
    for (const auto& s : m_acc) { meanRe += s.real(); meanIm += s.imag(); }
    meanRe /= m_fftSize;
    meanIm /= m_fftSize;

    auto* in = static_cast<fftw_complex*>(m_in);
    for (int n = 0; n < m_fftSize; ++n) {
        const double w = m_window[static_cast<std::size_t>(n)];
        in[n][0] = (static_cast<double>(m_acc[static_cast<std::size_t>(n)].real()) - meanRe) * w;
        in[n][1] = (static_cast<double>(m_acc[static_cast<std::size_t>(n)].imag()) - meanIm) * w;
    }

    fftw_execute(static_cast<fftw_plan>(m_plan));

    const auto* out = static_cast<fftw_complex*>(m_out);
    binsDbfs.resize(static_cast<std::size_t>(m_fftSize));
    const int half = m_fftSize / 2;
    for (int k = 0; k < m_fftSize; ++k) {
        const int src = (k + half) % m_fftSize;                       // fftshift: DC -> centre
        const double re = out[src][0];
        const double im = out[src][1];
        const double mag = std::sqrt(re * re + im * im) / m_coherentGain;
        // IQ is normalized to full scale 1.0, so this is dBFS directly.
        binsDbfs[static_cast<std::size_t>(k)] =
            static_cast<float>(20.0 * std::log10(mag + 1e-12));
    }

    applyDetector(binsDbfs);
}

void Hl2Spectrum::setAveraging(int frames, bool weighted) noexcept
{
    const int f = frames < 1 ? 1 : frames;
    if (f == m_avgFrames && weighted == m_avgWeighted) {
        return;
    }
    m_avgFrames = f;
    m_avgWeighted = weighted;
    // A window-length or mode change starts a fresh average rather than
    // blending the old shape into the new one.
    clearDetector();
}

void Hl2Spectrum::setPeakHold(bool on) noexcept
{
    if (on == m_peakHold) {
        return;
    }
    m_peakHold = on;
    // Switching detector mode starts fresh — a held peak must not bleed into
    // the average that replaces it, or vice versa.
    clearDetector();
}

void Hl2Spectrum::applyDetector(std::vector<float>& binsDbfs)
{
    const std::size_t n = binsDbfs.size();

    if (m_peakHold) {
        if (m_avgFrames <= 1) {
            // Infinite max-hold: the trace only rises until reset().
            if (m_peakVec.size() != n) {
                m_peakVec.assign(binsDbfs.begin(), binsDbfs.end());
            } else {
                for (std::size_t k = 0; k < n; ++k) {
                    m_peakVec[k] = std::max(m_peakVec[k], static_cast<double>(binsDbfs[k]));
                }
            }
            for (std::size_t k = 0; k < n; ++k) {
                binsDbfs[k] = static_cast<float>(m_peakVec[k]);
            }
            return;
        }
        // Sliding maximum over the last m_avgFrames frames — an old peak ages
        // out of the window. Shares m_avgHistory with the boxcar average.
        if (!m_avgHistory.empty() && m_avgHistory.front().size() != n) {
            m_avgHistory.clear();
        }
        m_avgHistory.push_back(binsDbfs);
        while (static_cast<int>(m_avgHistory.size()) > m_avgFrames) {
            m_avgHistory.pop_front();
        }
        for (std::size_t k = 0; k < n; ++k) {
            float hi = m_avgHistory.front()[k];
            for (const std::vector<float>& trace : m_avgHistory) {
                hi = std::max(hi, trace[k]);
            }
            binsDbfs[k] = hi;
        }
        return;
    }

    if (m_avgFrames <= 1) {
        return;   // "sample" detector — binsDbfs is the bare single-frame trace
    }

    if (m_avgWeighted) {
        // dB-domain EMA. Seed from the first frame so the trace does not have
        // to crawl up from silence over the first time constant.
        if (m_avgEma.size() != n) {
            m_avgEma.assign(binsDbfs.begin(), binsDbfs.end());
        } else {
            const double alpha = 2.0 / (m_avgFrames + 1);
            for (std::size_t k = 0; k < n; ++k) {
                m_avgEma[k] += alpha * (static_cast<double>(binsDbfs[k]) - m_avgEma[k]);
            }
        }
        for (std::size_t k = 0; k < n; ++k) {
            binsDbfs[k] = static_cast<float>(m_avgEma[k]);
        }
        return;
    }

    // Boxcar: the plain arithmetic mean of the last m_avgFrames traces. Summed
    // fresh each frame — m_avgFrames is an operator slider value (tens, not
    // thousands) and n is the FFT size, so O(n * frames) per frame is cheap and
    // it sidesteps the slow drift a kept running-sum accumulates.
    if (!m_avgHistory.empty() && m_avgHistory.front().size() != n) {
        m_avgHistory.clear();
    }
    m_avgHistory.push_back(binsDbfs);
    while (static_cast<int>(m_avgHistory.size()) > m_avgFrames) {
        m_avgHistory.pop_front();
    }
    std::vector<double> sum(n, 0.0);
    for (const std::vector<float>& trace : m_avgHistory) {
        for (std::size_t k = 0; k < n; ++k) {
            sum[k] += static_cast<double>(trace[k]);
        }
    }
    const double inv = 1.0 / static_cast<double>(m_avgHistory.size());
    for (std::size_t k = 0; k < n; ++k) {
        binsDbfs[k] = static_cast<float>(sum[k] * inv);
    }
}

}  // namespace AetherSDR::hl2
