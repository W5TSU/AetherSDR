#include "core/backends/hackrf/HackRfIq.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace AetherSDR::hackrf;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

void expectTrue(const char* name, bool ok) { report(name, ok); }

void expectNear(const char* name, double got, double want, double eps = 1e-6)
{
    if (std::fabs(got - want) < eps) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %f, want %f\n", name, got, want);
        ++g_failed;
    }
}

void expectEqual(const char* name, int got, int want)
{
    if (got == want) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %d, want %d\n", name, got, want);
        ++g_failed;
    }
}

// ── unpackRxIq ──────────────────────────────────────────────────────────

void testUnpackZero()
{
    const std::uint8_t raw[2] = {0, 0};
    std::vector<std::complex<float>> out;
    unpackRxIq(raw, 2, out);
    expectTrue("one sample produced", out.size() == 1);
    expectNear("zero byte -> zero I", out[0].real(), 0.0);
    expectNear("zero byte -> zero Q", out[0].imag(), 0.0);
}

void testUnpackPositiveFullScale()
{
    // 127 as int8_t is +127, the largest positive signed 8-bit value.
    const std::uint8_t raw[2] = {127, 127};
    std::vector<std::complex<float>> out;
    unpackRxIq(raw, 2, out);
    expectNear("+127 byte -> +127/128", out[0].real(), 127.0 / 128.0);
    expectNear("+127 byte -> +127/128 (Q)", out[0].imag(), 127.0 / 128.0);
}

void testUnpackNegativeFullScale()
{
    // 128 as a byte reinterpreted as int8_t is -128 (two's complement).
    const std::uint8_t raw[2] = {128, 128};
    std::vector<std::complex<float>> out;
    unpackRxIq(raw, 2, out);
    expectNear("byte 128 -> -1.0 (int8 -128)", out[0].real(), -1.0);
}

void testUnpackMinusOne()
{
    // 255 as a byte reinterpreted as int8_t is -1.
    const std::uint8_t raw[2] = {255, 255};
    std::vector<std::complex<float>> out;
    unpackRxIq(raw, 2, out);
    expectNear("byte 255 -> -1/128 (int8 -1)", out[0].real(), -1.0 / 128.0);
}

void testUnpackMultipleSamplesInterleaved()
{
    // Two samples: (I=10,Q=20), (I=-10 as byte 246,Q=-20 as byte 236)
    const std::uint8_t raw[4] = {10, 20, 246, 236};
    std::vector<std::complex<float>> out;
    unpackRxIq(raw, 4, out);
    expectTrue("two samples produced", out.size() == 2);
    expectNear("sample 0 I", out[0].real(), 10.0 / 128.0);
    expectNear("sample 0 Q", out[0].imag(), 20.0 / 128.0);
    expectNear("sample 1 I", out[1].real(), -10.0 / 128.0);
    expectNear("sample 1 Q", out[1].imag(), -20.0 / 128.0);
}

void testUnpackOddTrailingByteDropped()
{
    // A trailing unpaired byte must not be read as a sample, and must not
    // crash / read out of bounds.
    const std::uint8_t raw[3] = {10, 20, 30};
    std::vector<std::complex<float>> out;
    unpackRxIq(raw, 3, out);
    expectTrue("odd byteCount drops the trailing byte", out.size() == 1);
    expectNear("the one complete sample still decodes", out[0].real(), 10.0 / 128.0);
}

void testUnpackEmpty()
{
    std::vector<std::complex<float>> out;
    unpackRxIq(nullptr, 0, out);
    expectTrue("zero bytes produces zero samples", out.empty());
}

// ── packTxIq ────────────────────────────────────────────────────────────

void testPackZero()
{
    std::vector<std::complex<float>> in = {{0.0f, 0.0f}};
    std::uint8_t raw[2];
    packTxIq(in, raw);
    expectEqual("0.0 -> byte 0", static_cast<std::int8_t>(raw[0]), 0);
    expectEqual("0.0 -> byte 0 (Q)", static_cast<std::int8_t>(raw[1]), 0);
}

void testPackFullScalePositive()
{
    std::vector<std::complex<float>> in = {{1.0f, 1.0f}};
    std::uint8_t raw[2];
    packTxIq(in, raw);
    // Scaled by 127 (not 128) so +1.0 lands exactly at int8_t's max, +127 —
    // never overflowing into undefined/wrapped behaviour.
    expectEqual("+1.0 -> byte +127", static_cast<std::int8_t>(raw[0]), 127);
}

void testPackFullScaleNegative()
{
    std::vector<std::complex<float>> in = {{-1.0f, -1.0f}};
    std::uint8_t raw[2];
    packTxIq(in, raw);
    expectEqual("-1.0 -> byte -127", static_cast<std::int8_t>(raw[0]), -127);
}

void testPackClampsOvershoot()
{
    // A WDSP overshoot above [-1,1] must clip, not wrap around into a
    // wildly wrong (possibly opposite-sign) sample.
    std::vector<std::complex<float>> in = {{1.5f, -2.0f}};
    std::uint8_t raw[2];
    packTxIq(in, raw);
    expectEqual("1.5 clamps to +127, not wrapped", static_cast<std::int8_t>(raw[0]), 127);
    expectEqual("-2.0 clamps to -127, not wrapped", static_cast<std::int8_t>(raw[1]), -127);
}

void testPackMultipleSamples()
{
    std::vector<std::complex<float>> in = {{0.5f, -0.5f}, {0.25f, 0.0f}};
    std::uint8_t raw[4];
    packTxIq(in, raw);
    expectEqual("sample 0 I: 0.5*127 rounds to 64", static_cast<std::int8_t>(raw[0]), 64);
    expectEqual("sample 0 Q: -0.5*127 rounds to -64", static_cast<std::int8_t>(raw[1]), -64);
    expectEqual("sample 1 I: 0.25*127 rounds to 32", static_cast<std::int8_t>(raw[2]), 32);
    expectEqual("sample 1 Q: 0.0 -> 0", static_cast<std::int8_t>(raw[3]), 0);
}

void testRoundTripApproximatelyRecoversInput()
{
    std::vector<std::complex<float>> in = {{0.5f, -0.25f}};
    std::uint8_t raw[2];
    packTxIq(in, raw);
    std::vector<std::complex<float>> out;
    unpackRxIq(raw, 2, out);
    // Quantization to 8 bits loses precision (127 vs 128 scale asymmetry
    // adds a little more), but the round trip should stay within ~1%.
    expectNear("round trip recovers I within quantization error", out[0].real(), 0.5, 0.01);
    expectNear("round trip recovers Q within quantization error", out[0].imag(), -0.25, 0.01);
}

// ── Gain clamps ─────────────────────────────────────────────────────────

void testClampLnaGain()
{
    expectEqual("0 -> 0", clampLnaGainDb(0), 0);
    expectEqual("40 -> 40 (max)", clampLnaGainDb(40), 40);
    expectEqual("41 -> 40 (over max clamps)", clampLnaGainDb(41), 40);
    expectEqual("100 -> 40 (way over clamps)", clampLnaGainDb(100), 40);
    expectEqual("-5 -> 0 (negative clamps to floor)", clampLnaGainDb(-5), 0);
    expectEqual("5 -> 0 (rounds DOWN to step)", clampLnaGainDb(5), 0);
    expectEqual("12 -> 8 (rounds down to step)", clampLnaGainDb(12), 8);
    expectEqual("39 -> 32 (rounds down to step)", clampLnaGainDb(39), 32);
}

void testClampVgaGain()
{
    expectEqual("0 -> 0", clampVgaGainDb(0), 0);
    expectEqual("62 -> 62 (max)", clampVgaGainDb(62), 62);
    expectEqual("63 -> 62 (over max clamps)", clampVgaGainDb(63), 62);
    expectEqual("3 -> 2 (rounds down to step)", clampVgaGainDb(3), 2);
    expectEqual("61 -> 60 (rounds down to step)", clampVgaGainDb(61), 60);
}

void testClampTxVgaGain()
{
    expectEqual("0 -> 0", clampTxVgaGainDb(0), 0);
    expectEqual("47 -> 47 (max)", clampTxVgaGainDb(47), 47);
    expectEqual("48 -> 47 (over max clamps)", clampTxVgaGainDb(48), 47);
    expectEqual("-1 -> 0 (negative clamps to floor)", clampTxVgaGainDb(-1), 0);
    expectEqual("20 -> 20 (1 dB steps: no rounding)", clampTxVgaGainDb(20), 20);
}

// RF Power slider (0-100 %) -> the whole TX gain range: TX VGA alone for the
// first 47 dB, then the RF amp (~11 dB) with the VGA backed off by the same
// amount, so 100 % is VGA 47 dB + amp — the most the HackRF can put out.
void testTxDriveForPowerPercent()
{
    auto drive = [](int p) { return txDriveForPowerPercent(p); };
    expectEqual("0 % -> VGA 0 dB", drive(0).vgaDb, 0);
    expectTrue("0 % -> amp off", !drive(0).amp);
    expectEqual("100 % -> VGA 47 dB", drive(100).vgaDb, 47);
    expectTrue("100 % -> amp ON (full output)", drive(100).amp);
    expectEqual("50 % -> 29 dB total, VGA only", drive(50).vgaDb, 29);
    expectTrue("50 % -> amp off", !drive(50).amp);
    expectEqual("81 % -> 47 dB total, the last VGA-only step", drive(81).vgaDb, 47);
    expectTrue("81 % -> amp off", !drive(81).amp);
    expectEqual("82 % -> 48 dB total = amp + VGA 37", drive(82).vgaDb, 37);
    expectTrue("82 % -> amp on", drive(82).amp);
    expectEqual("-10 % clamps to VGA 0 dB", drive(-10).vgaDb, 0);
    expectEqual("150 % clamps to VGA 47 dB", drive(150).vgaDb, 47);
    expectTrue("150 % clamps to amp on", drive(150).amp);
    // Total gain never falls as the slider rises.
    int last = -1;
    bool monotonic = true;
    for (int p = 0; p <= 100; ++p) {
        const HackRfTxDrive d = drive(p);
        const int total = d.vgaDb + (d.amp ? kHackRfTxAmpGainDb : 0);
        monotonic = monotonic && total >= last;
        last = total;
    }
    expectTrue("total TX gain is monotonic over 0..100 %", monotonic);
}

// Frequency correction. Convention (Hl2FreqCal's, shared by the Calibration
// page): ppb > 0 means the crystal is FAST, so the hardware lands at
// commanded * (1 + e). To land on F, command F / (1 + e).
void testCorrectedTuneHz()
{
    expectEqual("0 ppb is a pass-through",
                static_cast<long long>(correctedTuneHz(435'000'000.0, 0)), 435'000'000LL);
    // +1 ppm fast: 435 MHz / 1.000001 = 434,999,565.0004
    expectEqual("+1000 ppb (fast) commands LOW",
                static_cast<long long>(correctedTuneHz(435'000'000.0, 1000)), 434'999'565LL);
    // -20 ppm slow: 435 MHz / 0.99998 = 435,008,700.174
    expectEqual("-20000 ppb (slow) commands HIGH",
                static_cast<long long>(correctedTuneHz(435'000'000.0, -20'000)), 435'008'700LL);
    // Round-trip: the commanded value, scaled by the real clock, lands on F.
    const double landed = static_cast<double>(correctedTuneHz(145'800'000.0, 12'345))
                          * (1.0 + 12'345e-9);
    expectNear("commanded * (1 + e) lands within 1 Hz of the target",
               landed, 145'800'000.0, 1.0);
    expectEqual("never negative", static_cast<long long>(correctedTuneHz(-5.0, 0)), 0LL);
}

// TUNE carrier pacing: top the TX queue up only while it holds less than the
// target, so the carrier neither starves (a gap on air) nor piles up (an unkey
// that keeps transmitting the backlog).
void testTuneCarrierPacing()
{
    const double rate = 8'000'000.0;   // HackRF TX samples/s
    expectTrue("an empty queue is topped up", shouldFeedTuneCarrier(0, rate, 100));
    expectTrue("50 ms queued of a 100 ms target is topped up",
               shouldFeedTuneCarrier(400'000, rate, 100));
    expectTrue("exactly the target is NOT topped up",
               !shouldFeedTuneCarrier(800'000, rate, 100));
    expectTrue("more than the target is NOT topped up",
               !shouldFeedTuneCarrier(2'000'000, rate, 100));
    expectTrue("a nonsense rate never feeds", !shouldFeedTuneCarrier(0, 0.0, 100));
}

// TX tuning on a HackRF Pro (firmware 2026.01.3): a frequency change is
// applied at the next stream start, in that stream's direction, and skipped
// when the frequency hasn't changed. TX on the frequency the device already
// holds from RX therefore keeps the RX tuning and transmits ~70 dB down, in
// bursts. TX must be commanded on a frequency that differs from the last one.
void testTxLoHz()
{
    expectEqual("TX on the frequency the device already holds moves 1 Hz",
                static_cast<int>(txLoHz(915'500'000, 915'500'000)), 915'500'001);
    expectEqual("TX on a frequency other than the last command is unchanged",
                static_cast<int>(txLoHz(915'500'000, 915'400'000)), 915'500'000);
    expectEqual("TX after the last command was the nudge itself is unchanged",
                static_cast<int>(txLoHz(915'500'000, 915'500'001)), 915'500'000);
    expectEqual("no previous command (0) is unchanged",
                static_cast<int>(txLoHz(28'400'000, 0)), 28'400'000);
}

} // namespace

int main()
{
    testUnpackZero();
    testUnpackPositiveFullScale();
    testUnpackNegativeFullScale();
    testUnpackMinusOne();
    testUnpackMultipleSamplesInterleaved();
    testUnpackOddTrailingByteDropped();
    testUnpackEmpty();

    testPackZero();
    testPackFullScalePositive();
    testPackFullScaleNegative();
    testPackClampsOvershoot();
    testPackMultipleSamples();
    testRoundTripApproximatelyRecoversInput();

    testClampLnaGain();
    testClampVgaGain();
    testClampTxVgaGain();

    testTxDriveForPowerPercent();
    testCorrectedTuneHz();
    testTuneCarrierPacing();
    testTxLoHz();

    return g_failed == 0 ? 0 : 1;
}
