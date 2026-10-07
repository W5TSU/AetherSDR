#include "HackRfTxDsp.h"

#include "core/backends/hl2/Hl2TxDsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::hackrf {

namespace {
// Hl2TxDsp's output rate: twice AudioEngine's 24 kHz, the rate its phasing
// network and bandpass were designed and measured at.
constexpr int kSsbRateHz = 48'000;
constexpr int kAudioBandpassTaps = 127;
// AM: the carrier at half scale, so full modulation (|audio| = 1) peaks at
// full scale and never overmodulates past zero.
constexpr float kAmCarrier = 0.5f;
}

HackRfTxDsp::HackRfTxDsp(QObject* parent) : QObject(parent) {}

HackRfTxDsp::~HackRfTxDsp() = default;

void HackRfTxDsp::configure(const Config& config)
{
    m_config = config;
    m_ssb.reset();
    m_bandpass.clear();
    using M = Config::Modulation;
    if (m_config.audioSampleRateHz <= 0 || m_config.outputSampleRateHz <= 0.0)
        return;
    if (m_config.modulation == M::Usb || m_config.modulation == M::Lsb) {
        m_ssb = std::make_unique<hl2::Hl2TxDsp>();
        hl2::Hl2TxDsp::Config s;
        s.inputSampleRateHz = m_config.audioSampleRateHz;
        s.outputSampleRateHz = kSsbRateHz;
        s.mode = m_config.modulation == M::Lsb ? WdspChannel::Mode::Lsb : WdspChannel::Mode::Usb;
        s.filterLowHz = m_config.filterLowHz;
        s.filterHighHz = m_config.filterHighHz;
        if (!m_ssb->configure(s)) {
            m_ssb.reset();   // an audio rate that does not divide 48 kHz: nothing to send
            return;
        }
        // Same thread, synchronous: processAudioBlock() emits its IQ before
        // returning, so the samples go out in the order the audio came in.
        connect(m_ssb.get(), &hl2::Hl2TxDsp::iqReady, this,
                [this](const std::vector<std::complex<float>>& wire) {
            // Back from the HPSDR wire's handedness to the analytic one.
            std::vector<std::complex<float>> analytic(wire.size());
            for (std::size_t i = 0; i < wire.size(); ++i)
                analytic[i] = std::conj(wire[i]);
            emitInterpolated(analytic);
        }, Qt::DirectConnection);
        m_interp.configure(kSsbRateHz, m_config.outputSampleRateHz);
    } else {
        if (m_config.modulation != M::Fm)
            designAudioBandpass();
        m_interp.configure(m_config.audioSampleRateHz, m_config.outputSampleRateHz);
    }
    reset();
}

void HackRfTxDsp::designAudioBandpass()
{
    // Windowed-sinc bandpass (Blackman): lowpass(high) - lowpass(low). The low
    // edge also removes DC, which in DSB would put a carrier on the air.
    const double fs = m_config.audioSampleRateHz;
    const double lo = std::clamp(m_config.filterLowHz, 0.0, fs / 2.0) / fs;
    const double hi = std::clamp(m_config.filterHighHz, 0.0, fs / 2.0) / fs;
    const int n = kAudioBandpassTaps;
    const int c = n / 2;
    m_bandpass.assign(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        const int k = i - c;
        const double lp = [&](double f) {
            return k == 0 ? 2.0 * f : std::sin(2.0 * std::numbers::pi * f * k) / (std::numbers::pi * k);
        }(hi) - (k == 0 ? 2.0 * lo : std::sin(2.0 * std::numbers::pi * lo * k) / (std::numbers::pi * k));
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (n - 1))
                       + 0.08 * std::cos(4.0 * std::numbers::pi * i / (n - 1));
        m_bandpass[static_cast<std::size_t>(i)] = static_cast<float>(lp * w);
    }
}

void HackRfTxDsp::reset()
{
    m_phase = 0.0;
    m_interp.reset();
    m_bpHist.assign(m_bandpass.size(), 0.0f);
    m_bpPos = 0;
    if (m_ssb) {
        m_ssb->reset();
    }
}

void HackRfTxDsp::emitInterpolated(const std::vector<std::complex<float>>& baseband)
{
    const std::vector<std::complex<float>> up = m_interp.process(baseband);
    if (up.empty())
        return;
    QVector<std::complex<float>> out(static_cast<qsizetype>(up.size()));
    std::copy(up.begin(), up.end(), out.begin());
    emit iqReady(out);
}

void HackRfTxDsp::processAudioBlock(const std::vector<float>& mono)
{
    if (mono.empty() || m_config.audioSampleRateHz <= 0
        || m_config.outputSampleRateHz <= 0.0) {
        return;
    }
    using M = Config::Modulation;
    switch (m_config.modulation) {
    case M::Usb:
    case M::Lsb:
        if (m_ssb)
            m_ssb->processAudioBlock(mono, /*clientLeveled=*/false);
        return;
    case M::Am:
    case M::Dsb: {
        std::vector<std::complex<float>> bb(mono.size());
        const std::size_t taps = m_bandpass.size();
        for (std::size_t s = 0; s < mono.size(); ++s) {
            m_bpHist[m_bpPos] = std::clamp(mono[s], -1.0f, 1.0f);
            float a = 0.0f;
            std::size_t idx = m_bpPos;
            for (std::size_t k = 0; k < taps; ++k) {
                a += m_bpHist[idx] * m_bandpass[k];
                idx = (idx == 0) ? taps - 1 : idx - 1;
            }
            m_bpPos = (m_bpPos + 1) % taps;
            a = std::clamp(a, -1.0f, 1.0f);
            bb[s] = m_config.modulation == M::Am ? std::complex<float>(kAmCarrier * (1.0f + a), 0.0f)
                                                 : std::complex<float>(a, 0.0f);
        }
        emitInterpolated(bb);
        return;
    }
    case M::Fm:
        break;
    }

    // FM. The audio goes up to the output rate first (as a real signal on the
    // I channel), THEN the phase is integrated at the full rate.
    std::vector<std::complex<float>> audio(mono.size());
    for (std::size_t s = 0; s < mono.size(); ++s)
        audio[s] = {std::clamp(mono[s], -1.0f, 1.0f), 0.0f};
    const std::vector<std::complex<float>> up = m_interp.process(audio);
    if (up.empty())
        return;
    // Phase advance per output sample for a signal deviating at exactly
    // maxDeviationHz — same 2*pi*f/rate relationship HackRfDdc's NCO and
    // RtlSdrDdc's FM demod both use, run here to INTEGRATE frequency into
    // phase rather than to recover frequency from it.
    const double phaseStepPerUnitAudio =
        2.0 * std::numbers::pi * m_config.maxDeviationHz / m_config.outputSampleRateHz;
    // Rotating a phasor instead of a cos/sin per output sample, which was most
    // of the cost at 16 MS/s. The step is tiny (5 kHz at 8 MS/s is 0.004 rad),
    // so exp(j*d) = 1 - d^2/2 + j(d - d^3/6) is exact to ~1e-13; the phase is
    // re-derived from the double accumulator once per block, so no error
    // builds up across blocks.
    std::complex<double> ph{std::cos(m_phase), std::sin(m_phase)};
    QVector<std::complex<float>> out(static_cast<qsizetype>(up.size()));
    for (std::size_t i = 0; i < up.size(); ++i) {
        const double d = phaseStepPerUnitAudio * static_cast<double>(up[i].real());
        m_phase += d;
        const double d2 = d * d;
        ph *= std::complex<double>(1.0 - 0.5 * d2, d * (1.0 - d2 / 6.0));
        out[static_cast<qsizetype>(i)] = std::complex<float>(static_cast<float>(ph.real()),
                                                            static_cast<float>(ph.imag()));
    }
    m_phase = std::remainder(m_phase, 2.0 * std::numbers::pi);
    emit iqReady(out);
}

}  // namespace AetherSDR::hackrf
