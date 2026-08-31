#include "core/backends/hl2/Hl2Spectrum.h"

#include <fftw3.h>

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

    applyAveraging(binsDbfs);
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
    m_avgEma.clear();
    m_avgHistory.clear();
    m_avgSum.clear();
}

void Hl2Spectrum::applyAveraging(std::vector<float>& binsDbfs)
{
    if (m_avgFrames <= 1) {
        return;   // stage bypassed — binsDbfs is the bare single-frame trace
    }

    const std::size_t n = binsDbfs.size();

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

    // Boxcar: arithmetic mean of the last m_avgFrames traces, kept as a running
    // per-bin sum so each frame is O(n) rather than O(n * frames).
    if (m_avgSum.size() != n) {
        m_avgSum.assign(n, 0.0);
        m_avgHistory.clear();
    }
    m_avgHistory.push_back(binsDbfs);
    for (std::size_t k = 0; k < n; ++k) {
        m_avgSum[k] += static_cast<double>(binsDbfs[k]);
    }
    while (static_cast<int>(m_avgHistory.size()) > m_avgFrames) {
        const std::vector<float>& oldest = m_avgHistory.front();
        for (std::size_t k = 0; k < n; ++k) {
            m_avgSum[k] -= static_cast<double>(oldest[k]);
        }
        m_avgHistory.pop_front();
    }
    const double inv = 1.0 / static_cast<double>(m_avgHistory.size());
    for (std::size_t k = 0; k < n; ++k) {
        binsDbfs[k] = static_cast<float>(m_avgSum[k] * inv);
    }
}

}  // namespace AetherSDR::hl2
