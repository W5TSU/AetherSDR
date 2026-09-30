#include "HackRfTxDsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::hackrf {

HackRfTxDsp::HackRfTxDsp(QObject* parent) : QObject(parent) {}

void HackRfTxDsp::configure(const Config& config)
{
    m_config = config;
}

void HackRfTxDsp::reset()
{
    m_phase = 0.0;
    m_resamplePhase = 0.0;
}

void HackRfTxDsp::processAudioBlock(const std::vector<float>& mono)
{
    if (mono.empty() || m_config.audioSampleRateHz <= 0
        || m_config.outputSampleRateHz <= 0.0) {
        return;
    }

    QVector<std::complex<float>> out;
    out.reserve(static_cast<int>(
        mono.size() * (m_config.outputSampleRateHz / m_config.audioSampleRateHz)) + 1);

    // Phase advance per output sample for a signal deviating at exactly
    // maxDeviationHz — same 2*pi*f/rate relationship HackRfDdc's NCO and
    // RtlSdrDdc's FM demod both use, run here to INTEGRATE frequency into
    // phase rather than to recover frequency from it.
    const double phaseStepPerUnitAudio =
        2.0 * std::numbers::pi * m_config.maxDeviationHz / m_config.outputSampleRateHz;

    for (const float rawSample : mono) {
        const float sample = std::clamp(rawSample, -1.0f, 1.0f);

        // Fractional accumulator (HackRfDdc's own stage-2 technique, run in
        // the interpolating direction): emit however many output samples
        // this ONE audio sample is due, exact on average even when
        // outputRate/audioRate has no clean integer form (8,000,000/48,000
        // = 166.667).
        m_resamplePhase += m_config.outputSampleRateHz;
        while (m_resamplePhase >= m_config.audioSampleRateHz) {
            m_resamplePhase -= m_config.audioSampleRateHz;

            m_phase += phaseStepPerUnitAudio * sample;
            if (m_phase > std::numbers::pi) {
                m_phase -= 2.0 * std::numbers::pi;
            } else if (m_phase < -std::numbers::pi) {
                m_phase += 2.0 * std::numbers::pi;
            }

            out.append(std::complex<float>(static_cast<float>(std::cos(m_phase)),
                                           static_cast<float>(std::sin(m_phase))));
        }
    }

    if (!out.isEmpty()) {
        emit iqReady(out);
    }
}

}  // namespace AetherSDR::hackrf
