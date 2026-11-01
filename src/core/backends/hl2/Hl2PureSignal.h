#pragma once

#include "core/backends/hl2/MetisClient.h"

#include <QString>

#include <array>
#include <complex>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class WdspChannel;

namespace AetherSDR::hl2 {

// What PureSignal is doing and seeing, read in one go. `info` is WDSP's GetPSInfo:
// 0-3 fit flags, 4 feedback level, 5 successes, 6 sanity flags, 7 attempts,
// 14 correcting, 15 calcc's state. A peak is nullopt until a sample was seen since
// the last read.
struct Hl2PsSnapshot {
    std::array<int, 16> info{};
    double maxTx = 0.0;
    double hwPeak = 0.0;
    bool correctionRunning = false;
    std::optional<double> feedbackPeakDbfs;
    std::optional<double> referencePeakDbfs;
};

// The only code that talks to WDSP's PureSignal. A stopped TX channel owns calcc
// (measuring) and iqc (correcting), because Hl2TxDsp is its own modulator and
// never runs a TXA chain (WDSP patch 23). Everything here runs on the I/O thread,
// the thread MetisClient and the EP6 fan-out already live on; the real-time paths
// correct() and feed() do not allocate.
class Hl2PureSignal final : public ITxIqCorrector {
public:
    Hl2PureSignal();
    ~Hl2PureSignal() override;

    bool open(int feedbackRateHz, std::string* error);
    // Ends correction first, running the ramp out on silence, for at most 2 s.
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return m_channel != nullptr; }

    // True only for a modulated signal. A single carrier cannot calibrate: it
    // shows the amplifier one point of its curve.
    void setMox(bool modulated);
    // DDC 2 and DDC 3 of one EP6 delivery, feedback and transmit reference.
    void feed(std::span<const std::complex<float>> feedback,
              std::span<const std::complex<float>> reference) noexcept;

    // Calibrate continually. After a stop() the feed has not carried out yet, this
    // waits for it: SetPSControl would otherwise overwrite the reset unseen.
    void start();
    void stop();    // back to uncorrected, ramped
    void setFeedbackRate(int rateHz);

    // Reading takes the peaks seen since the previous read.
    [[nodiscard]] Hl2PsSnapshot snapshot();

    // Waits for the correction thread, for at most a second (it may be in the middle
    // of a calibration), because a stop() queued behind it would run first. False when nothing could be written.
    bool saveCorrection(const QString& path);
    // Returns at once. correctionRunning says afterwards whether the file was good.
    void restoreCorrection(const QString& path);

    void correct(std::span<std::complex<float>> block, bool applyToOutput) noexcept override;

private:
    static constexpr int kFeedBlock = 1024;

    std::unique_ptr<WdspChannel> m_channel;
    int m_channelId = -1;
    bool m_mox = false;
    bool m_resetPending = false;   // stop() issued, calcc has not reached LRESET
    bool m_startDeferred = false;
    void drainWithSilence() noexcept;
    void noteCalccStepped() noexcept;

    std::vector<double> m_feedback;     // interleaved, kFeedBlock pairs
    std::vector<double> m_reference;
    int m_filled = 0;
    float m_feedbackPeak = -1.0f;       // magnitude squared; negative means none yet
    float m_referencePeak = -1.0f;

    std::vector<double> m_correctBuffer;
};

}  // namespace AetherSDR::hl2
