// The HL2's receive LNA gain while transmitting (register 0x0e) reaches the
// wire: as a one-shot when it changes, in the round robin on every rotation, and
// from Params when a session starts. Unset, the 0x0e bank is the all-zero
// generic ADC assignment it always was.
//
// SOCKET-FREE except for the start() case, which sends its start packets to a
// closed localhost port; nothing listens and nothing is keyed. Same packet
// builder and friend seam as hl2_band_filter_frame_test.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QVariantMap>

#include <array>
#include <cstdio>
#include <optional>

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

static const std::uint8_t* bank(const Ep2& pkt, int which)
{
    return pkt.data() + (which == 0 ? 8 : 8 + kFrameSize) + 3;
}
static std::uint8_t addrOf(const std::uint8_t* cc)
{
    return static_cast<std::uint8_t>(cc[0] & ~kC0MoxBit);
}
static Cc ccOf(const std::uint8_t* cc)
{
    return Cc{addrOf(cc), cc[1], cc[2], cc[3], cc[4]};
}

// The 0x0e bank of the next frame's bank B, if it is one.
static std::optional<Cc> nextTxGainBank(MetisClient& c)
{
    const Ep2 pkt = c.buildNextControlPacket();
    const std::uint8_t* b = bank(pkt, 1);
    if (addrOf(b) != kC0AdcAssignOrTxGain)
        return std::nullopt;
    return ccOf(b);
}

// The 0x0e bank the round robin carries, found by building one full rotation.
static std::optional<Cc> rotationTxGainBank(MetisClient& c)
{
    std::optional<Cc> found;
    for (int i = 0; i < 16; ++i) {
        if (auto b = nextTxGainBank(c))
            found = b;
    }
    return found;
}

static const Cc kUnset{kC0AdcAssignOrTxGain, 0, 0, 0, 0};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        check(rotationTxGainBank(c) == kUnset,
              "default: the rotation's 0x0e bank is the all-zero generic assignment");
    }

    {
        MetisClient c;
        MetisClientTestAccess::setStreaming(c);
        c.setTxLnaGainDb(-6);
        check(MetisClientTestAccess::oneShotQueued(c) == 1,
              "set while streaming: exactly one one-shot is queued");
        check(nextTxGainBank(c) == ccAdcAssignOrTxGain(-6),
              "set while streaming: the very next frame carries 0x0e with the gain (C3 0xC6)");
        check(rotationTxGainBank(c) == ccAdcAssignOrTxGain(-6),
              "set while streaming: the round robin re-asserts it");

        c.clearTxLnaGain();
        check(MetisClientTestAccess::oneShotQueued(c) == 1,
              "clear while streaming: exactly one one-shot is queued");
        check(nextTxGainBank(c) == kUnset,
              "clear: the next frame carries the all-zero bank, gain bits gone too");
        check(rotationTxGainBank(c) == kUnset,
              "clear: the round robin carries the all-zero bank again");
    }

    {
        MetisClient c;
        c.setTxLnaGainDb(10);
        check(MetisClientTestAccess::oneShotQueued(c) == 0,
              "set while stopped: nothing is queued");
        MetisClientTestAccess::setStreaming(c);
        check(rotationTxGainBank(c) == ccAdcAssignOrTxGain(10),
              "set while stopped: the rotation carries it once streaming (C3 0xD6)");
    }

    {
        MetisClient c;
        MetisClient::Params p;
        p.host = QHostAddress::LocalHost;
        p.port = 9;                       // discard port; nothing answers
        p.txLnaGainDb = 20;
        check(c.start(p), "start() with a transmit gain in Params");
        check(rotationTxGainBank(c) == ccAdcAssignOrTxGain(20),
              "start(): Params seeds the 0x0e bank (C3 0xE0) without a setter call");
        c.stop();

        p.txLnaGainDb.reset();
        check(c.start(p), "restart() with no transmit gain");
        check(rotationTxGainBank(c) == kUnset,
              "restart without a gain: the bank is all zero again, not the last session's");
        c.stop();
    }

    // ---- the backend: the hl2/txlnagain.set verb and its health rows ----
    //
    // Default-constructed, no link: the verb is answered locally (like the
    // options.* verbs), and the row reports what the backend will hand
    // MetisClient, whether or not a session is up.
    {
        AetherSDR::hl2::Hl2Backend backend;
        int results = 0;
        int errors = 0;
        quint64 lastId = 0;
        QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
                         [&](quint64 id, const QVariant&) { ++results; lastId = id; });
        QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionError, &backend,
                         [&](quint64, const QString&) { ++errors; });
        auto row = [&backend](const char* key) {
            return backend.healthSnapshot().values.value(QString::fromLatin1(key));
        };

        check(row("txLnaGainDb").toString() == QStringLiteral("off"),
              "backend: the transmit gain is reported, and reported off, before anything asks");
        check(!row("rx1IqPeakDbfs").isValid(),
              "backend: no raw IQ peak row before any telemetry, not a fabricated 0 dBFS");

        backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("txlnagain.set"),
                                71, QVariant(-6));
        check(errors == 0 && results == 1 && lastId == 71u,
              "backend: txlnagain.set is an implemented verb that answers its requestId");
        check(row("txLnaGainDb").toInt() == -6, "backend: the row reads the gain set (-6 dB)");

        backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("txlnagain.set"),
                                72, QVariant(100));
        check(row("txLnaGainDb").toInt() == 48, "backend: an out-of-range gain is clamped to +48");

        backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("txlnagain.set"),
                                73, QVariant());
        check(errors == 0 && results == 3, "backend: an invalid argument means off, not an error");
        check(row("txLnaGainDb").toString() == QStringLiteral("off"),
              "backend: off after a value reads off again");

        backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("txlnagain.set"),
                                74, QVariant(QStringLiteral("loud")));
        check(errors == 1, "backend: a non-numeric argument is an error, not off");
    }

    if (g_failures == 0)
        std::printf("hl2_tx_lna_gain_frame_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
