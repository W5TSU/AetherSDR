// PureSignal end to end against a software amplifier, no radio: Hl2PureSignal's
// correction drives a compressing amplifier with AM-PM, its output comes back as
// feedback, and third-order intermodulation has to fall. Everything else here is
// what the operator is promised around that: uncorrected on stop, no calibration
// from a non-modulated signal, a close that cannot hang, and a correction that
// comes back from a file.

#include "core/backends/hl2/Hl2PureSignal.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace AetherSDR::hl2;

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 126;
constexpr double kHwPeak = 0.24;   // the DAC reference for a full-scale wire sample
constexpr double kF1 = 700.0;
constexpr double kF2 = 1903.0;

std::complex<double> amplifier(std::complex<double> in)
{
    const double r = std::abs(in);
    if (r <= 0.0)
        return {};
    const double out = std::tanh(1.5 * r) / 1.5;
    const double phase = 5.0 * M_PI / 180.0 * r * r;
    return std::polar(out, std::arg(in) + phase);
}

class Loop {
public:
    explicit Loop(Hl2PureSignal& ps) : m_ps(ps) {}

    // One EP2 block through correction, the amplifier and back as feedback.
    void block(double feedbackGain = 1.0, bool feedPs = true)
    {
        std::vector<std::complex<float>> iq(kBlock);
        for (auto& s : iq) {
            const double t = m_n++ / kRate;
            const std::complex<double> v = 0.49 * std::polar(1.0, 2.0 * M_PI * kF1 * t)
                                         + 0.49 * std::polar(1.0, 2.0 * M_PI * kF2 * t);
            s = std::complex<float>(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
        lastSource = iq;
        m_ps.correct(iq, true);
        lastOutput = iq;

        std::vector<std::complex<float>> feedback(kBlock);
        std::vector<std::complex<float>> reference(kBlock);
        // Scaled so the feedback level sits near the 152 the loop aims for.
        const double scale = 152.0 / 256.0 * kHwPeak / amplifier({1.0, 0.0}).real();
        for (int i = 0; i < kBlock; ++i) {
            const std::complex<double> in(iq[static_cast<std::size_t>(i)].real(),
                                          iq[static_cast<std::size_t>(i)].imag());
            peak = std::max(peak, std::abs(in));
            const std::complex<double> out = amplifier(in);
            captured.push_back(out);
            feedback[static_cast<std::size_t>(i)] = std::complex<float>(
                static_cast<float>(out.real() * scale * feedbackGain),
                static_cast<float>(out.imag() * scale * feedbackGain));
            reference[static_cast<std::size_t>(i)] = iq[static_cast<std::size_t>(i)]
                                                     * static_cast<float>(kHwPeak);
        }
        if (feedPs)
            m_ps.feed(feedback, reference);
    }

    template <typename Pred>
    bool runUntil(Pred pred, double seconds)
    {
        const auto start = std::chrono::steady_clock::now();
        int blocks = 0;
        while (!pred()) {
            block();
            if (++blocks % 200 == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                > seconds)
                return false;
        }
        return true;
    }

    // Each product's level below the stronger tone, over one second of output.
    struct Imd { double imd3 = 0.0; double imd5 = 0.0; };
    Imd measure()
    {
        captured.clear();
        for (int i = 0; i < 48000 / kBlock; ++i)
            block();
        const auto at = [this](double hz) {
            std::complex<double> sum{};
            const std::size_t n = captured.size();
            for (std::size_t k = 0; k < n; ++k) {
                const double w = 2.0 * M_PI * hz * static_cast<double>(k) / kRate;
                const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(k)
                                                         / static_cast<double>(n - 1));
                sum += captured[k] * hann * std::polar(1.0, -w);
            }
            return 20.0 * std::log10(std::max(1e-12, std::abs(sum) / static_cast<double>(n)));
        };
        const double tone = std::max(at(kF1), at(kF2));
        const double d3 = 2 * kF1 - kF2;
        const double u3 = 2 * kF2 - kF1;
        const double d5 = 3 * kF1 - 2 * kF2;
        const double u5 = 3 * kF2 - 2 * kF1;
        return {std::max(at(d3), at(u3)) - tone, std::max(at(d5), at(u5)) - tone};
    }

    std::vector<std::complex<float>> lastSource;
    std::vector<std::complex<float>> lastOutput;
    std::vector<std::complex<double>> captured;
    double peak = 0.0;

private:
    Hl2PureSignal& m_ps;
    long m_n = 0;
};

}  // namespace

int main()
{
    std::printf("\n  PureSignal against a software amplifier\n\n");
    const auto corrFile = std::filesystem::temp_directory_path()
                        / ("hl2-ps-loop-" + std::to_string(::time(nullptr)) + ".corr");

    Hl2PureSignal ps;
    std::string error;
    check(ps.open(static_cast<int>(kRate), &error), "opens: " + error);
    Loop loop(ps);

    const Loop::Imd baseline = loop.measure();
    std::printf("  uncorrected: IMD3 %.1f dBc, IMD5 %.1f dBc\n", baseline.imd3, baseline.imd5);
    check(baseline.imd3 > -35.0, "premise: the amplifier really does distort");

    ps.setMox(true);
    ps.start();
    check(loop.runUntil([&] { return ps.snapshot().info[14] == 1; }, 30.0),
          "correction starts within the time limit");
    const int successes = ps.snapshot().info[5];
    check(loop.runUntil([&] { return ps.snapshot().info[5] >= successes + 6; }, 30.0),
          "calibrations keep succeeding");
    const Hl2PsSnapshot snap = ps.snapshot();

    // The window's curves, from a real calibration of a compressing amplifier: more
    // output than drive partway up, because the output is scaled to its own top.
    {
        const AetherSDR::PureSignalCurves curves = ps.curves(snap.info[5]);
        check(curves.valid(), "curves carry the calibration");
        check(curves.amplitude.size() > 100, "curves have samples");
        check(curves.correctionGain.size() == 512 && curves.correctionPhaseDeg.size() == 512,
              "curves have the correction");
        double bow = 0.0;
        for (const QPointF& p : curves.amplitude) {
            check(p.y() > 0.0 && p.y() <= 1.5, "sample level in range");
            if (p.x() > 0.4 && p.x() < 0.6)
                bow = std::max(bow, p.y() - p.x());
        }
        check(bow > 0.05, "the amplifier's compression shows on the curve");

        // The amplifier here turns 5 degrees by the top. The samples and the
        // correction are both drawn against the same reference, so they must agree.
        double worst = 0.0;
        double reach = 0.0;
        for (const QPointF& p : curves.phaseDeg) {
            const auto index = static_cast<qsizetype>(std::lround(p.x() * 511.0));
            if (index >= 80 && index < 512) {
                worst = std::max(worst, std::abs(p.y() - curves.correctionPhaseDeg[index].y()));
                reach = std::max(reach, std::abs(curves.correctionPhaseDeg[index].y()));
            }
        }
        check(reach > 1.0, "the correction turns the phase");
        check(worst < 1.5, "measured phase sits on the correction");
    }
    std::printf("  feedback level %d, maxTx %.4f, hwPeak %.4f\n", snap.info[4], snap.maxTx, snap.hwPeak);
    check(snap.info[4] > 128 && snap.info[4] < 181, "the feedback level reads in the working range");

    loop.peak = 0.0;
    const Loop::Imd corrected = loop.measure();
    std::printf("  corrected:   IMD3 %.1f dBc, IMD5 %.1f dBc\n", corrected.imd3, corrected.imd5);
    check(corrected.imd3 <= baseline.imd3 - 15.0, "IMD3 falls by at least 15 dB");
    check(corrected.imd5 <= baseline.imd5 + 0.5, "IMD5 does not rise");
    check(loop.peak <= 1.0, "the corrected output never exceeds full scale");

    // Peaks from the feed path, read after a fresh block.
    loop.block();
    {
        const Hl2PsSnapshot fresh = ps.snapshot();
        check(fresh.feedbackPeakDbfs.has_value() && fresh.referencePeakDbfs.has_value(),
              "a read after a feed has both peaks");
        const Hl2PsSnapshot next = ps.snapshot();
        check(!next.feedbackPeakDbfs.has_value(), "the next read with no feed has none");
    }

    // Not modulated: calibration has to stop, the feed gate is what stops it.
    ps.setMox(false);
    for (int i = 0; i < 400; ++i)
        loop.block();
    const int held = ps.snapshot().info[5];
    for (int i = 0; i < 2000; ++i)
        loop.block();
    check(ps.snapshot().info[5] == held, "unkeyed or a single carrier: no more calibrations");
    check(ps.snapshot().info[14] == 1, "correction stays loaded across that");
    ps.setMox(true);

    // Save, stop, restore: the correction comes back with no calibration.
    check(ps.saveCorrection(QString::fromStdString(corrFile.string())),
          "saving reports that the file was written");
    std::error_code sizeError;
    check(std::filesystem::file_size(corrFile, sizeError) > 0 && !sizeError,
          "the correction is in its file");
    check(ps.snapshot().correctionRunning, "correction is running while it is saved");

    ps.stop();
    check(loop.runUntil([&] { return ps.snapshot().info[14] == 0; }, 10.0), "stop ends correction");
    check(loop.runUntil([&] { return !ps.snapshot().correctionRunning; }, 10.0),
          "and the correction stage stops running");
    for (int i = 0; i < 100; ++i)
        loop.block();
    {
        bool same = true;
        for (std::size_t i = 0; i < loop.lastOutput.size(); ++i) {
            if (std::abs(loop.lastOutput[i] - loop.lastSource[i]) > 1e-5f)
                same = false;
        }
        check(same, "stopped: the output is exactly the input");
    }

    ps.setMox(false);
    const int beforeRestore = ps.snapshot().info[5];
    ps.restoreCorrection(QString::fromStdString(corrFile.string()));
    check(loop.runUntil([&] { return ps.snapshot().info[14] == 1; }, 10.0),
          "restore turns correction on");
    // Let the start ramp finish before measuring.
    for (int i = 0; i < 200; ++i)
        loop.block();
    const Loop::Imd restored = loop.measure();
    std::printf("  restored:    IMD3 %.1f dBc\n", restored.imd3);
    check(ps.snapshot().info[5] == beforeRestore, "restore needs no calibration");
    check(restored.imd3 <= baseline.imd3 - 15.0, "the restored correction still lowers IMD3 by 15 dB");

    // A restart from a held correction goes back to calibrating.
    ps.setMox(true);
    ps.start();
    check(loop.runUntil([&] { return ps.snapshot().info[5] > beforeRestore; }, 30.0),
          "start() after a restore calibrates again");

    // The sampler pulled: nothing can be fitted, the caller's stop() ends it.
    check(loop.runUntil([&] { return ps.snapshot().info[14] == 1; }, 30.0), "correcting again");
    ps.stop();
    double worst = 0.0;
    for (int i = 0; i < 400; ++i) {
        loop.block(0.0);
        for (const auto& s : loop.lastOutput)
            worst = std::max(worst, static_cast<double>(std::abs(s)));
    }
    check(ps.snapshot().info[14] == 0, "with no feedback at all, stop still ends correction");
    check(worst <= 1.0, "and nothing exceeds full scale on the way out");

    // A corrupt file: no correction, not a crash.
    {
        std::FILE* f = std::fopen(corrFile.string().c_str(), "w");
        if (f) {
            std::fputs("not a correction", f);
            std::fclose(f);
        }
        ps.restoreCorrection(QString::fromStdString(corrFile.string()));
        for (int i = 0; i < 400; ++i)
            loop.block();
        check(ps.snapshot().info[12] != 0, "a corrupt file is reported");
        bool same = true;
        for (std::size_t i = 0; i < loop.lastOutput.size(); ++i) {
            if (std::abs(loop.lastOutput[i] - loop.lastSource[i]) > 1e-5f)
                same = false;
        }
        check(same, "a corrupt file leaves the output uncorrected");
        ps.stop();
    }

    // Review finding: with the feed running, stop() then start() at once lost the
    // reset, and a band change kept the old band's curve.
    ps.setMox(true);
    ps.start();
    check(loop.runUntil([&] { return ps.snapshot().correctionRunning; }, 30.0), "correcting, keyed");
    ps.stop();
    ps.start();
    check(loop.runUntil([&] { return !ps.snapshot().correctionRunning; }, 10.0),
          "keyed, stop then start at once still ends the old correction");
    check(loop.runUntil([&] { return ps.snapshot().correctionRunning; }, 30.0),
          "and calibrates a new one");
    ps.stop();
    ps.start();
    ps.setMox(false);
    for (int i = 0; i < 400; ++i)
        loop.block();
    check(!ps.snapshot().correctionRunning, "a reset pending at unkey is carried out, not left armed");

    // Review finding: nothing made the folder, so every save failed.
    {
        ps.setMox(true);
        ps.start();
        check(loop.runUntil([&] { return ps.snapshot().correctionRunning; }, 30.0), "correcting to save");
        const auto dir = std::filesystem::temp_directory_path()
                       / ("hl2-ps-newdir-" + std::to_string(::time(nullptr)));
        const auto deep = dir / "radio" / "20m.corr";
        check(ps.saveCorrection(QString::fromStdString(deep.string())),
              "saving makes the folder it needs");
        std::error_code e;
        std::filesystem::remove_all(dir, e);
    }

    // Close in the middle of a start ramp must not hang.
    ps.setMox(true);
    ps.start();
    check(loop.runUntil([&] { return ps.snapshot().info[14] == 1; }, 30.0), "correcting before close");
    const auto closing = std::chrono::steady_clock::now();
    ps.close();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - closing).count();
    std::printf("  close took %.2f s\n", took);
    check(took < 3.0, "close returns promptly");
    check(!ps.isOpen(), "and is closed");
    check(!ps.saveCorrection(QStringLiteral("/nonexistent/x.corr")), "closed: nothing is saved");
    {
        Hl2PureSignal fresh;
        std::string freshError;
        check(fresh.open(48000, &freshError), "reopens");
        const QString tooLong(400, QLatin1Char('x'));
        check(!fresh.saveCorrection(tooLong), "a path WDSP's 256-byte buffer cannot hold is refused");
        fresh.restoreCorrection(tooLong);   // not a buffer overrun
        fresh.close();
    }

    std::error_code ignored;
    std::filesystem::remove(corrFile, ignored);
    std::printf("\n  %s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
