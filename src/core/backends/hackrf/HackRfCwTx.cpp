#include "HackRfCwTx.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::hackrf {

void HackRfCwTx::configure(const Config& config)
{
    m_cfg = config;
    const std::size_t n = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(m_cfg.rampMs * 1e-3 * m_cfg.sampleRateHz)));
    m_ramp.resize(n + 1);
    for (std::size_t k = 0; k <= n; ++k)
        m_ramp[k] = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * k / n));
    m_latencySamples = std::llround(m_cfg.latencyMs * 1e-3 * m_cfg.sampleRateHz);
    reset();
}

void HackRfCwTx::reset()
{
    m_edges.clear();
    m_pos = 0;
    m_originSet = false;
    m_originSeconds = 0.0;
    m_anchorSample = 0;
    m_target = false;
    m_rampPos = 0;
}

void HackRfCwTx::keyEdge(bool down, double timeSeconds)
{
    if (m_ramp.empty())
        configure(m_cfg);
    if (!m_originSet) {
        // Anchor at the CURRENT render position: under a PTT held before the
        // first paddle edge, rendering is already running, and anchoring at
        // sample 0 would clamp every edge to "now" (the jitter this avoids).
        m_originSet = true;
        m_originSeconds = timeSeconds;
        m_anchorSample = m_pos + m_latencySamples;
    }
    std::int64_t at = m_anchorSample
                    + std::llround((timeSeconds - m_originSeconds) * m_cfg.sampleRateHz);
    // Late (its slot is already rendered) or out of order: apply as soon as
    // possible rather than drop it, so a key-up can never be lost.
    at = std::max(at, m_pos);
    if (!m_edges.empty())
        at = std::max(at, m_edges.back().first);
    m_edges.emplace_back(at, down);
}

void HackRfCwTx::render(std::complex<float>* out, std::size_t n)
{
    if (m_ramp.empty())
        configure(m_cfg);
    const std::size_t rampN = m_ramp.size() - 1;
    for (std::size_t i = 0; i < n; ++i, ++m_pos) {
        while (!m_edges.empty() && m_edges.front().first <= m_pos) {
            m_target = m_edges.front().second;
            m_edges.pop_front();
        }
        if (m_target && m_rampPos < rampN)
            ++m_rampPos;
        else if (!m_target && m_rampPos > 0)
            --m_rampPos;
        out[i] = {m_cfg.amplitude * m_ramp[m_rampPos], 0.0f};
    }
}

} // namespace AetherSDR::hackrf
