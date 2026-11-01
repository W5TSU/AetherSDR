#include "core/backends/hl2/Hl2PureSignal.h"

#include "core/dsp/WdspChannel.h"

#include <aether_wdsp.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace AetherSDR::hl2 {
namespace {

// Read from source, not chosen here: deskHPSDR's transmitter.c for the HL2 on the
// original protocol. The peak is what the DAC reference reads for a full-scale
// wire sample (measured 0.2386 there), and a little high does no harm where a
// little low does.
constexpr double kHwPeak = 0.2400;
constexpr double kTxDelaySec = 150e-9;
constexpr double kMoxDelaySec = 0.2;
constexpr double kLoopDelaySec = 0.0;

// A reset takes effect only while samples reach pscc (deskHPSDR tx_ps_onoff).
constexpr int kDrainBlocks = 7;
constexpr int kCalccStateReset = 0;   // LRESET in calcc.c

// save_file and restore_file in calcc.c, including the terminator.
constexpr int kWdspPathBytes = 256;

std::optional<double> takePeakDb(float& peak)
{
    if (peak < 0.0f)
        return std::nullopt;
    const double db = 10.0 * std::log10(std::max(1e-14, static_cast<double>(peak)));
    peak = -1.0f;
    return db;
}

}  // namespace

Hl2PureSignal::Hl2PureSignal() = default;

Hl2PureSignal::~Hl2PureSignal()
{
    close();
}

bool Hl2PureSignal::open(int feedbackRateHz, std::string* error)
{
    if (m_channel)
        return true;
    WdspChannel::Config tx;
    tx.direction = WdspChannel::Direction::Transmit;
    tx.inputSampleRate = 48000;
    tx.dspSampleRate = 48000;
    tx.outputSampleRate = 48000;
    m_channel = WdspChannel::create(tx, error);
    if (!m_channel)
        return false;
    m_channelId = m_channel->channelId();

    m_feedback.assign(kFeedBlock * 2, 0.0);
    m_reference.assign(kFeedBlock * 2, 0.0);
    m_correctBuffer.assign(kTxSamplesPerPacket * 2, 0.0);
    m_filled = 0;
    m_mox = false;

    SetPSHWPeak(m_channelId, kHwPeak);
    SetPSFeedbackRate(m_channelId, feedbackRateHz);
    SetPSTXDelay(m_channelId, kTxDelaySec);
    SetPSMoxDelay(m_channelId, kMoxDelaySec);
    SetPSLoopDelay(m_channelId, kLoopDelaySec);
    return true;
}

void Hl2PureSignal::close()
{
    if (!m_channel)
        return;
    setMox(false);  // nothing feeds pscc from here on, so stop() drains it
    stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // The correction thread spins in SetTXAiqcEnd until the ramp has run, and a
    // running ramp only advances in PSApplyCorrection.
    std::array<std::complex<float>, kTxSamplesPerPacket> silence{};
    int quietPasses = 0;
    while (quietPasses < 8 && std::chrono::steady_clock::now() < deadline) {
        correct(silence, false);
        int info[16] = {};
        GetPSInfo(m_channelId, info);
        quietPasses = (PSCorrectionBusy(m_channelId) == 0 && info[14] == 0) ? quietPasses + 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m_channel.reset();
    m_channelId = -1;
}

void Hl2PureSignal::setMox(bool modulated)
{
    if (!m_channel || m_mox == modulated)
        return;
    m_mox = modulated;
    m_filled = 0;
    SetPSMox(m_channelId, modulated ? 1 : 0);
    // Nothing feeds pscc now, and a reset would sit armed until the next key-down.
    if (!modulated && m_resetPending)
        drainWithSilence();
}

void Hl2PureSignal::feed(std::span<const std::complex<float>> feedback,
                         std::span<const std::complex<float>> reference) noexcept
{
    if (!m_channel)
        return;
    const std::size_t n = std::min(feedback.size(), reference.size());
    for (std::size_t i = 0; i < n; ++i) {
        m_feedbackPeak = std::max(m_feedbackPeak, std::norm(feedback[i]));
        m_referencePeak = std::max(m_referencePeak, std::norm(reference[i]));
    }
    if (!m_mox)
        return;
    for (std::size_t i = 0; i < n; ++i) {
        m_feedback[static_cast<std::size_t>(m_filled) * 2] = feedback[i].real();
        m_feedback[static_cast<std::size_t>(m_filled) * 2 + 1] = feedback[i].imag();
        m_reference[static_cast<std::size_t>(m_filled) * 2] = reference[i].real();
        m_reference[static_cast<std::size_t>(m_filled) * 2 + 1] = reference[i].imag();
        if (++m_filled == kFeedBlock) {
            pscc(m_channelId, kFeedBlock, m_reference.data(), m_feedback.data());
            noteCalccStepped();
            m_filled = 0;
        }
    }
}

void Hl2PureSignal::start()
{
    if (!m_channel)
        return;
    if (m_resetPending) {
        m_startDeferred = true;
        return;
    }
    SetPSControl(m_channelId, 0, 0, 1, 0);
}

void Hl2PureSignal::stop()
{
    if (!m_channel)
        return;
    m_startDeferred = false;
    m_resetPending = true;
    SetPSControl(m_channelId, 1, 0, 0, 0);
    if (!m_mox)
        drainWithSilence();
}

void Hl2PureSignal::drainWithSilence() noexcept
{
    std::fill(m_feedback.begin(), m_feedback.end(), 0.0);
    for (int i = 0; i < kDrainBlocks && m_resetPending; ++i) {
        pscc(m_channelId, kFeedBlock, m_feedback.data(), m_feedback.data());
        noteCalccStepped();
    }
    // A calibration in progress holds calcc in LCALC past these blocks. correct()
    // runs on every EP2 packet and comes back here until LRESET has run.
}

void Hl2PureSignal::noteCalccStepped() noexcept
{
    if (!m_resetPending)
        return;
    int info[16] = {};
    GetPSInfo(m_channelId, info);
    // info[15] is the state pscc just ran. LRESET has run, so the reset is done.
    if (info[15] != kCalccStateReset)
        return;
    m_resetPending = false;
    if (m_startDeferred) {
        m_startDeferred = false;
        SetPSControl(m_channelId, 0, 0, 1, 0);
    }
}

void Hl2PureSignal::setFeedbackRate(int rateHz)
{
    if (m_channel)
        SetPSFeedbackRate(m_channelId, rateHz);
}

Hl2PsSnapshot Hl2PureSignal::snapshot()
{
    Hl2PsSnapshot s;
    if (!m_channel)
        return s;
    GetPSInfo(m_channelId, s.info.data());
    GetPSMaxTX(m_channelId, &s.maxTx);
    GetPSHWPeak(m_channelId, &s.hwPeak);
    s.correctionRunning = PSCorrectionRunning(m_channelId) != 0;
    // calcc sets this after any restore, even one whose file could not be read.
    if (!s.correctionRunning)
        s.info[14] = 0;
    s.feedbackPeakDbfs = takePeakDb(m_feedbackPeak);
    s.referencePeakDbfs = takePeakDb(m_referencePeak);
    return s;
}

bool Hl2PureSignal::saveCorrection(const QString& path)
{
    const QByteArray name = QFile::encodeName(path);
    if (!m_channel || name.size() >= kWdspPathBytes)
        return false;
    if (!QDir().mkpath(QFileInfo(path).path()))
        return false;
    QFile::remove(path);
    QByteArray writable = name;
    PSSaveCorr(m_channelId, writable.data());

    // Stable size: the correction thread writes and closes in one go, and a swap
    // it may be finishing first only advances in correct().
    std::array<std::complex<float>, kTxSamplesPerPacket> silence{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    qint64 lastSize = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        correct(silence, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const qint64 size = QFileInfo(path).size();
        if (size > 0 && size == lastSize)
            return true;
        lastSize = size;
    }
    return false;
}

void Hl2PureSignal::restoreCorrection(const QString& path)
{
    const QByteArray name = QFile::encodeName(path);
    if (!m_channel || name.size() >= kWdspPathBytes)
        return;
    QByteArray writable = name;
    PSRestoreCorr(m_channelId, writable.data());
    // PSRestoreCorr arms LTURNON, and calcc's state machine only steps in pscc.
    if (!m_mox) {
        std::fill(m_feedback.begin(), m_feedback.end(), 0.0);
        for (int i = 0; i < kDrainBlocks; ++i)
            pscc(m_channelId, kFeedBlock, m_feedback.data(), m_feedback.data());
    }
}

void Hl2PureSignal::correct(std::span<std::complex<float>> block, bool applyToOutput) noexcept
{
    if (!m_channel || block.empty())
        return;
    if (m_resetPending && !m_mox)
        drainWithSilence();
    const std::size_t n = std::min<std::size_t>(block.size(), kTxSamplesPerPacket);
    for (std::size_t i = 0; i < n; ++i) {
        m_correctBuffer[i * 2] = block[i].real();
        m_correctBuffer[i * 2 + 1] = block[i].imag();
    }
    PSApplyCorrection(m_channelId, static_cast<int>(n), m_correctBuffer.data(),
                      m_correctBuffer.data());
    if (!applyToOutput)
        return;
    for (std::size_t i = 0; i < n; ++i) {
        double re = m_correctBuffer[i * 2];
        double im = m_correctBuffer[i * 2 + 1];
        // iqc already limits to 0.999. This is the promise in one more place,
        // and what a NaN from a bad curve meets on its way to the wire.
        const double mag = std::hypot(re, im);
        if (!std::isfinite(mag)) {
            re = block[i].real();
            im = block[i].imag();
        } else if (mag > 1.0) {
            re /= mag;
            im /= mag;
        }
        block[i] = {static_cast<float>(re), static_cast<float>(im)};
    }
}

}  // namespace AetherSDR::hl2
