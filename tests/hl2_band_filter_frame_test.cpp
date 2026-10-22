// A band change must not put a stale sample rate in the later of an EP2 frame's
// two C&C banks. (aethersdr/AetherSDR#4579)
//
// SOCKET-FREE ON PURPOSE. It drives MetisClient's own packet builder, which is
// public for exactly this reason ("Exists so the gate can be tested on the exact
// bytes that would go out"), rather than standing up a fake radio -- the
// fake-radio fixtures for this path are retired, and are kept in tests.cmake
// only as a bracket comment.
//
// WHAT IS UNDER TEST. buildNextControlPacket() puts LIVE m_ccConfig in bank A of
// every frame; a one-shot only ever fills bank B; and the radio applies bank B
// after bank A. So a COPY of m_ccConfig queued as a one-shot is both redundant
// -- bank A already carried the change -- and, because it is a snapshot, able to
// overwrite the live value for one frame when something else rebuilds the config
// register in between.
//
// WHAT IT DOES NOT TEST. That the radio really applies the second sub-frame last
// is a protocol fact read from the HPSDR frame layout, not something measured
// here: no radio is involved and nothing is keyed. The assertions below do not
// depend on it -- they require that no frame carry two config banks at all, so
// there is no second one to win or lose.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <array>
#include <cstdio>

namespace AetherSDR::hl2 {
// Socket-free access to the one-shot queue and the running flag (#4579 / #6014).
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

// C&C sits SYNC(3) into each 512-byte frame. Frame 0 is bank A, frame 1 bank B.
static const std::uint8_t* bank(const Ep2& pkt, int which)
{
    return pkt.data() + (which == 0 ? 8 : 8 + kFrameSize) + 3;
}
// MOX rides C0 bit 0 of every bank, so it is masked off before the address is read.
static bool isConfigBank(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[0] & ~kC0MoxBit) == kC0Config;
}
static int rateCodeOf(const std::uint8_t* cc) { return cc[1] & 0x03; }
static std::uint8_t addrOf(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[0] & ~kC0MoxBit);
}
static std::uint32_t payloadOf(const std::uint8_t* cc)
{
    return (std::uint32_t(cc[1]) << 24) | (std::uint32_t(cc[2]) << 16)
         | (std::uint32_t(cc[3]) << 8) | std::uint32_t(cc[4]);
}
static bool paEnabledIn(const std::uint8_t* cc) { return (cc[2] & 0x08) != 0; }
// Open-collector outputs are C2[7:1] -- the one-bit shift ccConfig() applies.
static std::uint8_t ocByteOf(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[2] >> 1);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. bank A alone already carries the band change ----
    //
    // This pins the PREMISE of the fix rather than the fix: it passes with the
    // one-shot push present too. If it ever fails, deleting the push stops being
    // safe, so it is the assertion that has to hold for the rest to be honest.
    {
        MetisClient c;
        const Ep2 before = c.buildNextControlPacket();
        check(isConfigBank(bank(before, 0)), "bank A is the config register on every frame");
        check(ocByteOf(bank(before, 0)) == kOcNone, "no relay engaged before the band change");

        c.setBandFilter(kOcLpf80);
        const Ep2 after = c.buildNextControlPacket();
        check(isConfigBank(bank(after, 0)), "bank A is still the config register after the change");
        check(ocByteOf(bank(after, 0)) == kOcLpf80,
              "the new relay pattern reaches the wire on the very next frame, from bank A alone");
    }

    // ---- 2. the defect: a band change then a rate change, no frame in between ----
    {
        MetisClient c;                          // Params::sampleRate defaults to R48k
        c.setBandFilter(kOcLpf80);              // a snapshot taken here holds 48k
        c.setSampleRate(SampleRate::R192k);     // the live config register moves to 192k

        const Ep2 pkt = c.buildNextControlPacket();
        const std::uint8_t* a = bank(pkt, 0);
        const std::uint8_t* b = bank(pkt, 1);

        check(isConfigBank(a) && rateCodeOf(a) == static_cast<int>(SampleRate::R192k),
              "bank A carries the live sample rate");
        check(!isConfigBank(b),
              "bank B is not a second config bank, so nothing can overwrite bank A");
        if (isConfigBank(b)) {
            std::fprintf(stderr,
                         "       bank A rate code %d, bank B rate code %d"
                         " -- two config banks in one frame, and the radio ends on the later\n",
                         rateCodeOf(a), rateCodeOf(b));
        }
    }

    // ---- 3. and none appears in the frames that follow ----
    {
        MetisClient c;
        c.setBandFilter(kOcLpf80);
        c.setSampleRate(SampleRate::R192k);
        bool sawSecondConfig = false;
        for (int i = 0; i < 8; ++i) {
            const Ep2 pkt = c.buildNextControlPacket();
            sawSecondConfig = sawSecondConfig || isConfigBank(bank(pkt, 1));
        }
        check(!sawSecondConfig, "no frame in the next eight carries a second config bank");
    }

    // ---- 4. the cross-session half ----
    //
    // m_oneShot is cleared nowhere -- not in start() alongside m_txSeq,
    // m_roundRobin, m_haveRxSeq, m_drops and m_linkUp -- and stop() deliberately
    // preserves everything that is not an unfinished IO-board write. So a bank
    // queued by a band change while disconnected would ride the next session's
    // first frames. Queuing nothing is what closes that here.
    {
        MetisClient c;
        c.setBandFilter(kOcLpf80);
        c.stop();
        bool survived = false;
        for (int i = 0; i < 4; ++i) {
            const Ep2 pkt = c.buildNextControlPacket();
            survived = survived || isConfigBank(bank(pkt, 1));
        }
        check(!survived, "nothing a disconnected band change queued survives into the next session");
    }

    // ---- 2. no one-shot bank is queued while stopped (upstream #6014) ----
    //
    // m_oneShot survives stop() and start(), so a bank queued while stopped
    // would go out in the next session's priming burst. The values are kept
    // (the next session restates them); only the wire write is withheld.
    {
        constexpr std::uint32_t kRxHz = 14'074'000;
        constexpr std::uint32_t kTxHz = 14'076'000;
        constexpr int kDrive = 200;

        MetisClient c;                          // never started: m_running is false
        c.enableTransmit(true);                 // so the drive is not clamped to 0
        c.setRxFrequencyHz(kRxHz);
        c.setTxFrequencyHz(kTxHz);
        c.setTxDriveLevel(kDrive);
        check(MetisClientTestAccess::oneShotQueued(c) == 0,
              "stopped: the live-control setters queue no one-shot bank");

        MetisClientTestAccess::setStreaming(c);
        const Ep2 first = c.buildNextControlPacket();
        check(addrOf(bank(first, 1)) == kC0Rx1Freq && payloadOf(bank(first, 1)) == kRxHz,
              "the first frame of the next session carries the new RX1 frequency, "
              "from the rotation");
        bool sawTxFreq = false, sawDrive = false, sawPa = false;
        const auto scan = [&](const Ep2& pkt) {
            for (int w = 0; w < 2; ++w) {
                const std::uint8_t* cc = bank(pkt, w);
                sawTxFreq = sawTxFreq || addrOf(cc) == kC0TxFreq;
                if (addrOf(cc) == kC0TxDrive) {
                    sawDrive = true;
                    sawPa = sawPa || paEnabledIn(cc);
                }
            }
        };
        scan(first);
        for (int i = 0; i < 16; ++i)            // several rotations past the priming burst
            scan(c.buildNextControlPacket());
        check(!sawTxFreq, "no TX frequency bank set while stopped reaches the next session");
        check(!sawDrive, "no drive bank set while stopped reaches the next session");
        check(!sawPa, "the PA enable asked for while stopped is not on the wire");
    }

    // ---- 3. while running, each setter is on the next frame ----
    {
        constexpr std::uint32_t kRxHz = 7'074'000;
        constexpr std::uint32_t kTxHz = 7'076'000;
        constexpr int kDrive = 120;

        MetisClient c;
        c.enableTransmit(true);
        MetisClientTestAccess::setStreaming(c);

        c.setTxFrequencyHz(kTxHz);
        const Ep2 tx = c.buildNextControlPacket();
        check(addrOf(bank(tx, 1)) == kC0TxFreq && payloadOf(bank(tx, 1)) == kTxHz,
              "running: a TX frequency change is on the next frame");

        c.setTxDriveLevel(kDrive);
        const Ep2 drive = c.buildNextControlPacket();
        check(addrOf(bank(drive, 1)) == kC0TxDrive && bank(drive, 1)[1] == kDrive
                  && paEnabledIn(bank(drive, 1)),
              "running: a drive change is on the next frame, PA enabled");

        check(MetisClientTestAccess::oneShotQueued(c) == 0, "running: nothing else was queued");
        c.setRxFrequencyHz(kRxHz);
        check(MetisClientTestAccess::oneShotQueued(c) == 1,
              "running: an RX frequency change queues exactly one bank");
    }

    // ---- 4. stop() mid-queue drops the drive and IO-board banks ----
    {
        constexpr std::uint32_t kRxHz = 10'136'000;
        constexpr std::uint32_t kTxHz = 10'138'000;
        constexpr int kDrive = 180;

        MetisClient c;
        c.enableTransmit(true);
        MetisClientTestAccess::setStreaming(c);
        c.setTxFrequencyHz(kTxHz);
        c.setRxFrequencyHz(kRxHz);
        c.setTxDriveLevel(kDrive);              // 0x09, PA enabled
        c.setIoBoardTxFrequencyHz(kTxHz);       // five I2C banks
        check(MetisClientTestAccess::oneShotQueued(c) == 8,
              "running: TX NCO, RX NCO, a drive bank and five I2C banks queued");

        c.stop();                               // before a single frame drained
        check(MetisClientTestAccess::oneShotQueued(c) == 2,
              "stopped mid-queue: only the TX and RX NCO banks survive stop()");
        MetisClientTestAccess::setStreaming(c);

        bool sawDrive = false, sawPa = false, sawI2c = false;
        for (int i = 0; i < 8; ++i) {
            const Ep2 pkt = c.buildNextControlPacket();
            for (int w = 0; w < 2; ++w) {
                const std::uint8_t* cc = bank(pkt, w);
                if (addrOf(cc) == kC0TxDrive) {
                    sawDrive = true;
                    sawPa = sawPa || paEnabledIn(cc);
                }
                sawI2c = sawI2c || addrOf(cc) == kC0I2c2;
            }
        }
        check(!sawDrive, "stopped mid-queue: no drive bank of the ended session reaches the next");
        check(!sawPa, "stopped mid-queue: the ended session's PA enable is not on the wire");
        check(!sawI2c, "stopped mid-queue: the IO board write does not finish in the next session");
    }

    // ---- 5. ADC dither / random queue no CONFIG snapshot (#4579) ----
    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.setAdcDither(true);
        c.setAdcRandom(true);
        check(MetisClientTestAccess::oneShotQueued(c) == 0,
              "dither and random ride live bank A: no CONFIG copy is queued");
        const Ep2 pkt = c.buildNextControlPacket();
        check(isConfigBank(bank(pkt, 0)), "bank A carries the config register");
        check(!isConfigBank(bank(pkt, 1)), "bank B carries no stale CONFIG snapshot");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_band_filter_frame_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
