#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <span>

namespace AetherSDR::hl2 {

// What one voice over put on the wire, in EP2 samples (48 kHz) from the first
// keyed sample. Keyed silence has two sources that a capture cannot tell apart:
// a queue with nothing in it, and zeros the TX chain delivered. Only the first
// is something priming the queue could recover (upstream #6052).
struct Hl2TxOverReport {
    int keyedSamples = 0;
    int firstSignalSample = -1;           // -1: no signal reached the wire
    int leadingDrySamples = 0;            // before it, with the queue empty
    int leadingDeliveredZeroSamples = 0;  // before it, zeros the chain delivered
    int gapCount = 0;                     // zero runs between signal; the tail is not one
    int gapSamples = 0;
    int gapDrySamples = 0;                // the part of gapSamples the queue ran dry for
    int longestGapSamples = 0;

    // First sample at or above each level. A filter's pre-ringing is non-zero
    // from the first sample, long before the audio arrives, so firstSignalSample
    // alone cannot say when the signal is there. -1: never reached.
    static constexpr std::array<int, 3> kRiseLevelsDbfs{-60, -20, -10};
    std::array<int, kRiseLevelsDbfs.size()> rise{-1, -1, -1};
    float peakDbfs = -200.0f;

    [[nodiscard]] int riseSample(int levelDbfs) const noexcept
    {
        for (std::size_t i = 0; i < kRiseLevelsDbfs.size(); ++i) {
            if (kRiseLevelsDbfs[i] == levelDbfs)
                return rise[i];
        }
        return -1;
    }
};

class Hl2TxOverAccumulator {
public:
    void begin() noexcept
    {
        m_report = {};
        m_runSamples = 0;
        m_runDrySamples = 0;
        m_peakPower = 0.0f;
    }

    // `block` is what the queue supplied for a packet of `capacity` samples. A
    // short block is zero-padded on the wire, so the rest counts as dry.
    void addKeyedPacket(std::span<const std::complex<float>> block, int capacity) noexcept
    {
        const int supplied = std::min(static_cast<int>(block.size()), capacity);
        for (int i = 0; i < supplied; ++i) {
            const auto& s = block[static_cast<std::size_t>(i)];
            if (s.real() != 0.0f || s.imag() != 0.0f)
                addSignal(std::norm(s));
            else
                addZero(/*dry=*/false);
        }
        for (int i = supplied; i < capacity; ++i)
            addZero(/*dry=*/true);
    }

    [[nodiscard]] const Hl2TxOverReport& report() const noexcept { return m_report; }

private:
    // Shorter runs are a waveform passing through zero, not a gap. The shortest
    // gap measured on hardware in #6052 was 0.4 ms, about 19 samples.
    static constexpr int kMinGapSamples = 8;

    void addSignal(float power) noexcept
    {
        if (power > m_peakPower) {
            m_peakPower = power;
            m_report.peakDbfs = 10.0f * std::log10(power);
            for (std::size_t i = 0; i < m_report.rise.size(); ++i) {
                if (m_report.rise[i] < 0
                    && m_report.peakDbfs >= static_cast<float>(Hl2TxOverReport::kRiseLevelsDbfs[i]))
                    m_report.rise[i] = m_report.keyedSamples;
            }
        }
        if (m_report.firstSignalSample < 0)
            m_report.firstSignalSample = m_report.keyedSamples;
        if (m_runSamples >= kMinGapSamples) {
            ++m_report.gapCount;
            m_report.gapSamples += m_runSamples;
            m_report.gapDrySamples += m_runDrySamples;
            m_report.longestGapSamples = std::max(m_report.longestGapSamples, m_runSamples);
        }
        m_runSamples = 0;
        m_runDrySamples = 0;
        ++m_report.keyedSamples;
    }

    void addZero(bool dry) noexcept
    {
        if (m_report.firstSignalSample < 0) {
            ++(dry ? m_report.leadingDrySamples : m_report.leadingDeliveredZeroSamples);
        } else {
            ++m_runSamples;
            if (dry)
                ++m_runDrySamples;
        }
        ++m_report.keyedSamples;
    }

    Hl2TxOverReport m_report;
    int m_runSamples = 0;
    int m_runDrySamples = 0;
    float m_peakPower = 0.0f;
};

} // namespace AetherSDR::hl2
