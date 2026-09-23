#include "HackRfDdc.h"

#include <numbers>

namespace AetherSDR::hackrf {

HackRfDdc::HackRfDdc(QObject* parent)
    : QObject(parent)
{
}

void HackRfDdc::setInputSampleRateHz(double hz)
{
    if (hz > 0.0) m_inputRateHz.store(hz, std::memory_order_relaxed);
}

void HackRfDdc::setCenterFrequencyHz(double hz)
{
    m_centerHz.store(hz, std::memory_order_relaxed);
}

void HackRfDdc::setSliceFrequencyHz(double hz)
{
    m_sliceHz.store(hz, std::memory_order_relaxed);
}

void HackRfDdc::setOutputSampleRateHz(double hz)
{
    if (hz > 0.0) m_outputRateHz.store(hz, std::memory_order_relaxed);
}

void HackRfDdc::process(const QVector<std::complex<float>>& wideband)
{
    if (wideband.isEmpty()) return;

    const double inputRateHz = m_inputRateHz.load(std::memory_order_relaxed);
    const double centerHz = m_centerHz.load(std::memory_order_relaxed);
    const double sliceHz = m_sliceHz.load(std::memory_order_relaxed);
    const double outputRateHz = m_outputRateHz.load(std::memory_order_relaxed);

    // NCO shift: rotate the target slice frequency down to DC. Negative
    // step because a station ABOVE center needs a NEGATIVE rotation to
    // cancel its positive baseband frequency — same sign convention as
    // RtlSdrDdc::processAudio's own ncoStepPhasor.
    const double ncoStep = 2.0 * std::numbers::pi * (sliceHz - centerHz) / inputRateHz;
    const std::complex<float> ncoStepPhasor(
        static_cast<float>(std::cos(-ncoStep)),
        static_cast<float>(std::sin(-ncoStep)));

    QVector<std::complex<float>> out;
    out.reserve(static_cast<int>(wideband.size() * (outputRateHz / inputRateHz)) + 1);

    for (const auto& sample : wideband) {
        m_ncoPhasor *= ncoStepPhasor;
        if (++m_ncoNormalizeCounter >= 1000) {
            m_ncoNormalizeCounter = 0;
            const float mag = std::abs(m_ncoPhasor);
            if (mag > 0.0f) m_ncoPhasor /= mag;
        }
        const std::complex<float> shifted = sample * m_ncoPhasor;

        // Boxcar accumulate (anti-alias lowpass ahead of decimation) +
        // fractional-resample to exactly outputRateHz on average —
        // RtlSdrDdc::processAudio's stage-2 technique, generalized from
        // "decimate demodulated audio to 24 kHz" to "decimate complex IQ to
        // any target rate": a rounded integer divisor drifts for a
        // rate pair with no clean ratio (8,000,000/48,000 = 166.67) and
        // eventually starves or overruns the consumer.
        m_decimAcc += shifted;
        ++m_decimCount;
        m_resamplePhase += outputRateHz;
        if (m_resamplePhase >= inputRateHz) {
            m_resamplePhase -= inputRateHz;
            out.append(m_decimAcc / static_cast<float>(m_decimCount));
            m_decimAcc = {0.0f, 0.0f};
            m_decimCount = 0;
        }
    }

    if (!out.isEmpty()) {
        emit decimatedIqReady(out);
    }
}

} // namespace AetherSDR::hackrf
