// Every transmit mode the HackRF offers, through the real modulator.
//
// The HackRF transmitted FM and CW only; SSB, AM and DSB were refused as
// receive-only. Each mode is pinned here with no hardware: a test tone goes
// through HackRfTxDsp, configured exactly as HackRfBackend configures it for
// that mode, and the emitted IQ (at the HackRF's 8 MS/s) is measured:
//   - USB/DIGU/RTTY put the tone above the carrier, LSB/DIGL below, with the
//     carrier and the other sideband 40+ dB down;
//   - AM carries a carrier and both sidebands, DSB both sidebands and no
//     carrier, FM the Bessel lines of the tone;
//   - nothing the rate conversion could leave behind (images at multiples of
//     the low baseband rates) reaches 60 dB of the wanted signal.
// The sideband sign is the analytic convention: a positive frequency is above
// the carrier on the air (HackRF's IQ, unlike the HPSDR wire, is analytic).
#include "core/backends/hackrf/HackRfBackend.h"
#include "core/backends/hackrf/HackRfTxDsp.h"

#include <QCoreApplication>

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hackrf;

namespace {

int g_failed = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_failed;
}

constexpr double kOutRate = 8'000'000.0;
constexpr int kAudioRate = 24'000;

// Amplitude of the complex tone at f over [from, from+n), in dB re full scale.
// n spans 0.2 s, so every tone used here (multiples of 5 Hz) is a whole
// number of cycles and a rectangular window is exact.
double lineDb(const std::vector<std::complex<float>>& x, std::size_t from, std::size_t n, double f)
{
    std::complex<double> acc{0, 0};
    const double step = -2.0 * M_PI * f / kOutRate;
    std::complex<double> rot{1, 0};
    const std::complex<double> dRot{std::cos(step), std::sin(step)};
    for (std::size_t i = 0; i < n; ++i) {
        acc += std::complex<double>(x[from + i]) * rot;
        rot *= dRot;
        if ((i & 1023) == 0) rot /= std::abs(rot);
    }
    return 20.0 * std::log10(std::max(std::abs(acc) / static_cast<double>(n), 1e-15));
}

std::vector<std::complex<float>> modulate(const QString& mode, double toneHz, double seconds)
{
    HackRfTxDsp dsp;
    HackRfTxDsp::Config cfg;
    cfg.audioSampleRateHz = kAudioRate;
    cfg.outputSampleRateHz = kOutRate;
    dsp.configure(HackRfBackend::txConfigFor(mode, cfg));
    std::vector<std::complex<float>> iq;
    QObject::connect(&dsp, &HackRfTxDsp::iqReady, [&](const QVector<std::complex<float>>& b) {
        iq.insert(iq.end(), b.begin(), b.end());
    });
    const std::size_t total = static_cast<std::size_t>(seconds * kAudioRate);
    const std::size_t block = 480;   // 20 ms, AudioEngine's cadence
    for (std::size_t off = 0; off < total; off += block) {
        std::vector<float> audio(block);
        for (std::size_t i = 0; i < block; ++i)
            audio[i] = 0.3f * static_cast<float>(std::sin(2.0 * M_PI * toneHz * static_cast<double>(off + i) / kAudioRate));
        dsp.processAudioBlock(audio);
    }
    return iq;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Only broadcast WFM stays receive-only.
    for (const char* m : {"USB", "LSB", "DIGU", "DIGL", "RTTY", "AM", "SAM", "DSB", "FM", "FMN", "DFM"})
        check(HackRfBackend::canTransmitMode(QString::fromLatin1(m)), std::string(m) + " can transmit");
    check(!HackRfBackend::canTransmitMode(QStringLiteral("WFM")), "WFM stays receive-only");

    struct SsbCase { const char* mode; double tone; double sign; };
    for (const SsbCase& c : {SsbCase{"USB", 1000, +1}, SsbCase{"DIGU", 1500, +1}, SsbCase{"RTTY", 2125, +1},
                             SsbCase{"LSB", 1000, -1}, SsbCase{"DIGL", 1500, -1}}) {
        const auto iq = modulate(QString::fromLatin1(c.mode), c.tone, 0.5);
        const std::size_t from = static_cast<std::size_t>(0.2 * kOutRate);
        const std::size_t n = static_cast<std::size_t>(0.2 * kOutRate);
        if (iq.size() < from + n) { check(false, std::string(c.mode) + ": enough IQ"); continue; }
        const double wanted = lineDb(iq, from, n, c.sign * c.tone);
        const double other = lineDb(iq, from, n, -c.sign * c.tone) - wanted;
        const double carrier = lineDb(iq, from, n, 0.0) - wanted;
        double image = -300.0;
        for (double r : {24'000.0, 48'000.0, 96'000.0, 192'000.0, 384'000.0, 768'000.0, 1'536'000.0, 3'072'000.0})
            for (double s : {+1.0, -1.0})
                image = std::max(image, lineDb(iq, from, n, s * r + c.sign * c.tone) - wanted);
        std::printf("  %-4s: tone %+.0f Hz at %.1f dBFS; other sideband %+.1f dB, carrier %+.1f dB, worst image %+.1f dB\n",
                    c.mode, c.sign * c.tone, wanted, other, carrier, image);
        check(wanted > -20.0 && other < -40.0 && carrier < -40.0,
              std::string(c.mode) + (c.sign > 0 ? ": the tone goes out above the carrier" : ": the tone goes out below the carrier"));
        check(image < -60.0, std::string(c.mode) + ": no rate-conversion image within 60 dB");
    }

    for (const char* m : {"AM", "SAM"}) {
        const auto iq = modulate(QString::fromLatin1(m), 1000, 0.5);
        const std::size_t from = static_cast<std::size_t>(0.2 * kOutRate), n = static_cast<std::size_t>(0.2 * kOutRate);
        const double carrier = lineDb(iq, from, n, 0.0);
        const double up = lineDb(iq, from, n, 1000.0) - carrier, lo = lineDb(iq, from, n, -1000.0) - carrier;
        double image = -300.0;
        for (double r : {24'000.0, 48'000.0, 96'000.0, 3'072'000.0})
            for (double s : {+1.0, -1.0})
                image = std::max(image, lineDb(iq, from, n, s * r) - carrier);
        std::printf("  %-4s: carrier %.1f dBFS; sidebands %+.1f / %+.1f dB re carrier; worst image %+.1f dB\n",
                    m, carrier, up, lo, image);
        check(carrier > -20.0 && std::fabs(up - lo) < 0.5 && up > -30.0 && up < -3.0,
              std::string(m) + ": a carrier with both sidebands");
        check(image < -60.0, std::string(m) + ": no rate-conversion image within 60 dB");
    }

    {
        const auto iq = modulate(QStringLiteral("DSB"), 1000, 0.5);
        const std::size_t from = static_cast<std::size_t>(0.2 * kOutRate), n = static_cast<std::size_t>(0.2 * kOutRate);
        const double up = lineDb(iq, from, n, 1000.0), lo = lineDb(iq, from, n, -1000.0);
        const double carrier = lineDb(iq, from, n, 0.0) - up;
        std::printf("  DSB : sidebands %.1f / %.1f dBFS, carrier %+.1f dB\n", up, lo, carrier);
        check(up > -20.0 && std::fabs(up - lo) < 0.5 && carrier < -40.0, "DSB: both sidebands, no carrier");
    }

    // FM used to hold each audio sample across the output samples it spans,
    // which left lines at multiples of 24 kHz only 42-62 dB down (worst with
    // a 2125 Hz tone): spurious emissions, and above 30 MHz the FCC asks for
    // 60 dB. The audio now goes through the interpolator before integration.
    for (const char* m : {"FM", "FMN", "DFM"}) {
        for (double toneHz : {1000.0, 2125.0}) {
            const auto iq = modulate(QString::fromLatin1(m), toneHz, 0.5);
            const std::size_t from = static_cast<std::size_t>(0.2 * kOutRate), n = static_cast<std::size_t>(0.2 * kOutRate);
            const double up = lineDb(iq, from, n, toneHz), lo = lineDb(iq, from, n, -toneHz);
            double image = -300.0;
            for (double r : {24'000.0, 48'000.0, 72'000.0, 96'000.0, 3'072'000.0})
                for (double s : {+1.0, -1.0})
                    for (double b : {+toneHz, -toneHz})
                        image = std::max(image, lineDb(iq, from, n, s * r + b) - up);
            std::printf("  %-4s %4.0f Hz: first Bessel lines %.1f / %.1f dBFS, worst image %+.1f dB\n",
                        m, toneHz, up, lo, image);
            check(up > -20.0 && std::fabs(up - lo) < 0.5, std::string(m) + ": the tone's FM sidebands, symmetric");
            check(image < -60.0, std::string(m) + ": no rate-conversion image within 60 dB");
        }
    }

    std::printf("%s\n", g_failed == 0 ? "hackrf_tx_modes_test: OK" : "hackrf_tx_modes_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
