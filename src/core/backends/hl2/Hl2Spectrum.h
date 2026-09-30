#pragma once

#include <complex>
#include <cstddef>
#include <deque>
#include <span>
#include <vector>

namespace AetherSDR::hl2 {

// The HL2 panadapter path: accumulate raw IQ (normalized [-1, 1)) into fixed
// FFT frames and produce a DC-centered magnitude spectrum in dBFS. Ported from
// the live-validated tools/hl2/spectrum.py — Hanning-windowed, per-frame DC
// removal (the direct-sampling ADC offset sits on I), coherent-gain normalized,
// fftshifted so DC lands at the centre bin.
//
// Owns an FFTW plan; construction/destruction allocate, process() does not
// (fftw_execute is allocation-free). FFTW's global planner is not thread-safe,
// so construct instances off the real-time path (single-channel today).
class Hl2Spectrum {
public:
    explicit Hl2Spectrum(int fftSize = 1024);
    ~Hl2Spectrum();
    Hl2Spectrum(const Hl2Spectrum&) = delete;
    Hl2Spectrum& operator=(const Hl2Spectrum&) = delete;

    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }

    // Append IQ samples; each time a full frame accumulates, compute one
    // spectrum. `binsDbfs` is resized to fftSize (DC at index fftSize/2) and
    // holds the most recent frame. Returns the number of frames produced (a
    // partial frame is carried to the next call).
    int process(std::span<const std::complex<float>> iq, std::vector<float>& binsDbfs);

    // Append IQ WITHOUT transforming, keeping only the newest samples. Used
    // while the display-rate cap is between frames: the window keeps filling, so
    // when the next frame comes due it completes from recent contiguous samples
    // instead of refilling from empty.
    //
    // Refilling was what made the achieved frame rate track the SPAN rather than
    // the operator's slider. A frame is fftSize samples and an EP6 block is 126,
    // so an empty accumulator costs ~9 block intervals before a frame can be
    // emitted at all — 23.6 ms at 48 kHz but only 3.0 ms at 384 kHz. Feeding it
    // instead bounds that to a single block.
    //
    // Caps at fftSize - 1 deliberately: older samples can never contribute to
    // the next transform, and leaving the buffer exactly full would break
    // process()'s frame-boundary detection (it fires on == fftSize after a
    // push_back, so a pre-filled buffer would step straight past it and never
    // emit another frame).
    void accumulate(std::span<const std::complex<float>> iq);

    // Drop whatever partial frame has accumulated -- and ONLY that -- returning
    // how many samples went with it (upstream a449dee4 calls this reset()).
    // went with it (0 = nothing was in flight, so nothing was salvaged).
    //
    // The caller is a TRANSPORT SEQUENCE GAP, not the geometry change this was
    // originally written for. A geometry change RECONSTRUCTS this object --
    // Hl2RxDsp::configure() does `m_spectrum = std::make_unique<Hl2Spectrum>(...)`
    // -- so the accumulator is already empty on the far side of one, and that is
    // why this function sat with no caller in the tree at all. The case that
    // genuinely needs it is the one nobody wired: lost EP6 packets.
    //
    // WHY A GAP MATTERS HERE and not merely to a packet counter. process()
    // carries a partial frame ACROSS calls and transforms only on
    // `m_acc.size() == m_fftSize`. An EP6 block is 126 IQ samples and a frame is
    // fftSize, so ~8 blocks build one frame at the 1024 points every backend
    // here actually runs. When packets are lost mid-frame the accumulator keeps
    // the pre-gap samples and fills the rest from post-gap ones: the FFT then
    // spans a time discontinuity, and the phase relationship across the seam is
    // not a measurement of anything. Discarding is the right answer rather than
    // zero-filling the hole -- there is no sample to interpolate, the radio
    // never sent it, and a zero run is a broadband transient this window would
    // faithfully render as signal.
    //
    // The RETURN VALUE is what makes "a gap arrived" distinguishable from "a gap
    // cost us a frame". A gap landing exactly on a frame boundary discards
    // nothing and corrupts nothing; see Hl2RxDsp::spectrumGapDiscards().
    //
    // FORK: separate from reset() below. A sequence gap corrupts the frame in
    // flight, not the operator's trace average or peak hold (Plan 3's
    // detector history), so a gap must not wipe those; reset() still does, for
    // the geometry change it was written for.
    std::size_t discardPartialFrame() noexcept
    {
        const std::size_t discarded = m_acc.size();
        m_acc.clear();
        return discarded;
    }

    // Trace averaging across frames (Plan 3). Mirrors the operator's
    // Display → FFT AVG level and weighted-average toggle: a Flex runs this in
    // firmware and echoes the level back through pan status, and a raw-IQ
    // backend that streams its own spectra runs it here instead.
    //
    //   frames <= 1        no averaging — every frame is the bare magnitude
    //   weighted == true   dB-domain EMA, alpha = 2 / (frames + 1)
    //   weighted == false  boxcar mean of the last `frames` frame traces
    //
    // Averaging is applied to the dBFS trace, not the linear magnitude — it
    // smooths what the operator sees, the same trace-average a spectrum
    // analyser's video averaging produces. Changing either argument clears the
    // accumulator (a level or mode change must not blend two window lengths).
    // Never touches the FFT plan, size, or the partial-frame buffer.
    void setAveraging(int frames, bool weighted) noexcept;

    // Peak (max-hold) detector — the third of the spectrum-analyser detector
    // modes (sample / average / peak; issue #1 story 6). When on, the detector
    // stage outputs a per-bin maximum instead of the average:
    //
    //   averaging frames <= 1   infinite max-hold — the trace only ever rises,
    //                           until reset() (a geometry change) clears it
    //   averaging frames  > 1   sliding maximum over the last `frames` frames,
    //                           so an old peak ages out of the window
    //
    // Shares the frame history with the boxcar average, so the FFT AVG level
    // doubles as the hold window. Turning it off returns the stage to
    // sample / average per setAveraging().
    void setPeakHold(bool on) noexcept;

    // Drop whatever partial frame has accumulated, and the detector history.
    // Used on a geometry change, where the samples either side — and the frames
    // either side — genuinely describe different windows.
    void reset() noexcept
    {
        m_acc.clear();
        clearDetector();
    }

private:
    void computeFrame(std::vector<float>& binsDbfs);
    // Fold the just-computed raw dBFS trace through the detector stage
    // (sample / average / peak), rewriting binsDbfs in place. A no-op only for
    // sample: averaging frames <= 1 with peak-hold off.
    void applyDetector(std::vector<float>& binsDbfs);
    // Append the trace to m_avgHistory, trim to m_avgFrames, and drop a stale
    // window whose width no longer matches. Shared by the boxcar average and
    // the windowed peak.
    void pushFrameHistory(const std::vector<float>& binsDbfs);
    // Forget every accumulated frame so the next one starts a fresh average /
    // peak hold.
    void clearDetector() noexcept
    {
        m_avgEma.clear();
        m_avgHistory.clear();
        m_peakVec.clear();
    }

    int m_fftSize;
    std::vector<std::complex<float>> m_acc;   // accumulation buffer (< m_fftSize)
    std::vector<double> m_window;             // Hanning window
    double m_coherentGain = 1.0;              // sum(window) / 2

    // Detector state. With m_peakHold false and m_avgFrames <= 1 the stage is
    // bypassed entirely (the "sample" detector).
    int m_avgFrames = 1;
    bool m_avgWeighted = true;
    bool m_peakHold = false;
    std::vector<double> m_avgEma;             // weighted average: running EMA, per bin
    std::deque<std::vector<float>> m_avgHistory;  // boxcar / windowed peak: last m_avgFrames traces
    std::vector<double> m_peakVec;            // infinite max-hold: running per-bin maximum
    // Opaque FFTW handles (kept as void* so fftw3.h stays out of the header).
    void* m_in = nullptr;                     // fftw_complex[m_fftSize]
    void* m_out = nullptr;                     // fftw_complex[m_fftSize]
    void* m_plan = nullptr;                    // fftw_plan
};

}  // namespace AetherSDR::hl2
