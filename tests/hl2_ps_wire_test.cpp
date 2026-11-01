// PureSignal on the Protocol 1 wire. 0x0a bit 22 (C2 0x40) switches the second
// mixer to the DAC while transmitting; off, the bytes are exactly what they were.
// The corrector sees every EP2 block and its output reaches the wire only for a
// modulated signal. The two-tone test signal is two clean, equal tones.
//
// SOCKET-FREE. Packets are built and inspected; nothing is sent and nothing keys
// a radio.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static std::size_t oneShotQueued(const MetisClient& c) { return c.m_oneShot.size(); }
    static void setStreaming(MetisClient& c) { c.m_running = true; }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
    else     { std::fprintf(stderr, "[ OK ] %s\n", what); }
}

using Ep2 = std::array<std::uint8_t, kUsbPacketSize>;

static std::uint8_t addrOf(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[0] & ~kC0MoxBit);
}

static std::optional<Cc> rotationGainBank(MetisClient& c)
{
    std::optional<Cc> found;
    for (int i = 0; i < 16; ++i) {
        const Ep2 pkt = c.buildNextControlPacket();
        const std::uint8_t* b = pkt.data() + 8 + kFrameSize + 3;
        if (addrOf(b) == kC0AdcGain)
            found = Cc{addrOf(b), b[1], b[2], b[3], b[4]};
    }
    return found;
}

static std::vector<std::complex<double>> wireIq(const Ep2& pkt)
{
    std::vector<std::complex<double>> out;
    for (const std::size_t fs : {std::size_t{8}, std::size_t{8 + kFrameSize}}) {
        const std::uint8_t* p = pkt.data() + fs + 8;
        for (std::size_t k = 0; k + kTxSampleBytes <= kFramePayload; k += kTxSampleBytes) {
            const auto i = static_cast<std::int16_t>((p[k + 4] << 8) | p[k + 5]);
            const auto q = static_cast<std::int16_t>((p[k + 6] << 8) | p[k + 7]);
            out.emplace_back(i / 32767.0, q / 32767.0);
        }
    }
    return out;
}

// Records each call; halves what it may change, so the wire shows whether it did.
struct HalvingCorrector final : ITxIqCorrector {
    int calls = 0;
    int applied = 0;
    void correct(std::span<std::complex<float>> block, bool applyToOutput) noexcept override
    {
        ++calls;
        if (applyToOutput)
            ++applied;
        for (auto& s : block)
            s *= 0.5f;
    }
};

static double peakOf(const std::vector<std::complex<double>>& iq)
{
    double peak = 0.0;
    for (const auto& s : iq)
        peak = std::max(peak, std::abs(s));
    return peak;
}

// Magnitude at one frequency, relative to full scale, by correlation.
static double toneDb(const std::vector<std::complex<double>>& iq, double hz)
{
    std::complex<double> sum{};
    for (std::size_t n = 0; n < iq.size(); ++n) {
        const double w = 2.0 * M_PI * hz * static_cast<double>(n) / 48000.0;
        const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(n)
                                                 / static_cast<double>(iq.size() - 1));
        sum += iq[n] * hann * std::polar(1.0, -w);
    }
    return 20.0 * std::log10(std::max(1e-12, 2.0 * std::abs(sum) / static_cast<double>(iq.size())));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    check(ccRxGain(10, true)[2] == 0x40, "ccRxGain with PureSignal sets C2 bit 6 (0x0a bit 22)");
    check(ccRxGain(10) == ccRxGain(10, false) && ccRxGain(10)[2] == 0x00,
          "ccRxGain without PureSignal: C2 stays clear");

    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        check(rotationGainBank(c)->at(2) == 0x00, "default rotation: no PureSignal bit");

        c.setPureSignalEnabled(true);
        check(MetisClientTestAccess::oneShotQueued(c) == 1, "enable queues exactly one one-shot");
        check(rotationGainBank(c)->at(2) == 0x40, "enabled: the rotation carries the bit");
        c.setLnaGainDb(5);
        check(rotationGainBank(c)->at(2) == 0x40, "a later LNA change keeps the bit");

        c.setPureSignalEnabled(false);
        check(rotationGainBank(c)->at(2) == 0x00, "disabled: the bit is gone from the rotation");
    }

    {
        MetisClient c;
        HalvingCorrector corrector;
        c.setTxIqCorrector(&corrector);
        MetisClientTestAccess::setStreaming(c);

        c.buildNextControlPacket();
        check(corrector.calls == 1 && corrector.applied == 0,
              "unkeyed: the corrector still runs, on silence, not applied");

        c.enableTransmit(true);
        c.setMox(true);

        c.setTxTestTone(0.0, 0.8);
        const auto tone = wireIq(c.buildNextControlPacket());
        check(corrector.applied == 0 && std::abs(peakOf(tone) - 0.8) < 0.001,
              "single test tone: the corrector runs but the carrier goes out untouched");
        c.setTxTestTone(0.0, 0.0);

        std::vector<std::complex<float>> voice(kTxSamplesPerPacket, {0.8f, 0.0f});
        c.queueTxIq(voice);
        const auto queued = wireIq(c.buildNextControlPacket());
        check(corrector.applied == 1 && std::abs(peakOf(queued) - 0.4) < 0.001,
              "queued IQ: the corrector's output is what reaches the wire");

        c.setTxTwoTone(700.0, 1900.0, 0.45);
        const auto twoTone = wireIq(c.buildNextControlPacket());
        check(corrector.applied == 2 && peakOf(twoTone) < 0.46,
              "two-tone: the corrector's output is what reaches the wire");
        c.setTxTestTone(0.0, 0.0);

        c.setCwKeyDown(true);
        c.buildNextControlPacket();
        check(corrector.applied == 2, "CW: not corrected");
        c.clearCwKeying();

        c.setMox(false);
        c.setTxIqCorrector(nullptr);
        const auto none = corrector.calls;
        c.buildNextControlPacket();
        check(corrector.calls == none, "corrector removed: not called again");
    }

    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.enableTransmit(true);
        c.setMox(true);
        c.setTxTwoTone(700.0, 1900.0, 0.45);
        check(c.txTestToneEnabled(), "two-tone reports as the active test signal");

        std::vector<std::complex<double>> iq;
        for (int i = 0; i < 40; ++i) {
            const auto block = wireIq(c.buildNextControlPacket());
            iq.insert(iq.end(), block.begin(), block.end());
        }
        // The wire's handedness is the conjugate, so upper-sideband tones read
        // at negative frequencies here.
        const double f1 = toneDb(iq, -700.0);
        const double f2 = toneDb(iq, -1900.0);
        check(std::abs(f1 - f2) < 0.5, "the two tones are within 0.5 dB of each other");
        check(std::abs(f1 - 20.0 * std::log10(0.45)) < 1.0, "each tone is at the amplitude asked");
        check(peakOf(iq) <= 0.9001, "the peak is the sum of the two, 0.9");
        // Offsets where an imperfect generator or phase steps would show up.
        bool clean = true;
        for (const double hz : {700.0, 1900.0, 500.0, -100.0, -1300.0, -3100.0, -4300.0}) {
            if (toneDb(iq, hz) > f1 - 60.0)
                clean = false;
        }
        check(clean, "nothing else above -60 dBc, including the opposite sideband");

        c.flushTxIq();
        check(!c.txTestToneEnabled(), "flush clears the two-tone");

        c.setTxTwoTone(700.0, 1900.0, 0.45);
        c.setTxTestTone(0.0, 0.0);
        check(!c.txTestToneEnabled(), "a zero test tone clears the two-tone");

        c.setTxTwoTone(700.0, 1900.0, 0.8);
        check(peakOf(wireIq(c.buildNextControlPacket())) <= 1.0, "amplitudes cannot overflow");
        c.setMox(false);
        c.setTxTestTone(0.0, 0.0);
    }

    {
        MetisClient c;
        MetisClient::Params p;
        p.host = QHostAddress::LocalHost;
        p.port = 9;
        p.pureSignal = true;
        check(c.start(p), "start() with PureSignal in Params");
        check(rotationGainBank(c)->at(2) == 0x40, "start(): Params seeds the bit");
        c.stop();
    }

    if (g_failures == 0)
        std::printf("hl2_ps_wire_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
