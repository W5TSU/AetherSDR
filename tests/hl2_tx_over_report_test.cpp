// What an HL2 voice over actually put on the wire, sample by sample (upstream
// #6052). Keyed silence comes from two places that look identical on a capture:
// a queue with nothing in it, and zeros the TX chain delivered. The report keeps
// them apart, because only the first is something queue priming can recover.
//
// Driven through the real packet builder. Nothing is keyed; there is no socket.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <complex>
#include <cstdio>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
};
}

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}
static void checkEq(int got, int want, const char* what)
{
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s: got %d, want %d\n", what, got, want);
        ++g_failures;
    }
}

static constexpr int kPacket = 126;

static void send(MetisClient& client, int packets)
{
    for (int i = 0; i < packets; ++i)
        client.buildNextControlPacket();
}

static void queueSignal(MetisClient& client, int samples)
{
    client.queueTxIq(std::vector<std::complex<float>>(
        static_cast<std::size_t>(samples), std::complex<float>(0.5f, -0.5f)));
}

static void queueZeros(MetisClient& client, int samples)
{
    client.queueTxIq(std::vector<std::complex<float>>(static_cast<std::size_t>(samples)));
}

static void startOver(MetisClient& client)
{
    MetisClientTestAccess::setStreaming(client);
    client.enableTransmit(true);
    client.setMox(true);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The leading silence splits into the empty queue and the chain's zeros.
    {
        MetisClient client;
        startOver(client);
        send(client, 8);
        queueZeros(client, 2 * kPacket);
        queueSignal(client, 10 * kPacket);
        send(client, 12);
        client.setMox(false);

        const Hl2TxOverReport r = client.lastTxOverReport();
        checkEq(r.keyedSamples, 20 * kPacket, "every keyed sample is counted");
        checkEq(r.leadingDrySamples, 8 * kPacket, "empty queue at key-down");
        checkEq(r.leadingDeliveredZeroSamples, 2 * kPacket, "zeros the chain delivered");
        checkEq(r.firstSignalSample, 10 * kPacket, "first non-zero sample");
        checkEq(r.gapCount, 0, "no gap in a clean over");
    }

    // A short delivery mid-over: the packet is padded, then the queue stays dry.
    {
        MetisClient client;
        startOver(client);
        queueSignal(client, 8 * kPacket + 100);
        send(client, 9);
        send(client, 2);
        queueSignal(client, kPacket);
        send(client, 1);
        client.setMox(false);

        const Hl2TxOverReport r = client.lastTxOverReport();
        checkEq(r.firstSignalSample, 0, "signal from the first sample");
        checkEq(r.leadingDrySamples, 0, "no leading silence");
        checkEq(r.gapCount, 1, "one gap");
        checkEq(r.longestGapSamples, 26 + 2 * kPacket, "padding joins the dry packets");
        checkEq(r.gapDrySamples, 26 + 2 * kPacket, "the whole gap is a dry queue");
    }

    // Zeros delivered mid-over are a gap too, but not the queue's.
    {
        MetisClient client;
        startOver(client);
        queueSignal(client, 8 * kPacket);
        queueZeros(client, kPacket);
        queueSignal(client, kPacket);
        send(client, 10);
        client.setMox(false);

        const Hl2TxOverReport r = client.lastTxOverReport();
        checkEq(r.gapCount, 1, "delivered zeros are a gap");
        checkEq(r.longestGapSamples, kPacket, "delivered gap length");
        checkEq(r.gapDrySamples, 0, "delivered zeros are not a dry queue");
    }

    // A signal crossing zero is not a gap.
    {
        MetisClient client;
        startOver(client);
        queueSignal(client, 50);
        queueZeros(client, 7);
        queueSignal(client, 9 * kPacket - 57);
        send(client, 9);
        client.setMox(false);
        checkEq(client.lastTxOverReport().gapCount, 0, "a few zero samples are not a gap");
    }

    // Silence running into the unkey is the end of the over, not a gap.
    {
        MetisClient client;
        startOver(client);
        queueSignal(client, 9 * kPacket);
        send(client, 12);
        client.setMox(false);

        const Hl2TxOverReport r = client.lastTxOverReport();
        checkEq(r.gapCount, 0, "the tail is not a gap");
        checkEq(r.keyedSamples, 12 * kPacket, "the tail is still keyed");
    }

    // An over with no signal at all has no first sample.
    {
        MetisClient client;
        startOver(client);
        send(client, 3);
        client.setMox(false);

        const Hl2TxOverReport r = client.lastTxOverReport();
        checkEq(r.firstSignalSample, -1, "no signal reached the wire");
        checkEq(r.leadingDrySamples, 3 * kPacket, "all of it dry");
    }

    // Each over reports alone, and a synthesised one does not replace it: a tone
    // never touches the queue, so it says nothing about the queue.
    {
        MetisClient client;
        startOver(client);
        send(client, 8);
        queueSignal(client, MetisClient::kTxPrimeSamples);
        send(client, 1);
        client.setMox(false);

        client.setMox(true);
        queueSignal(client, MetisClient::kTxPrimeSamples);
        send(client, 1);
        client.setMox(false);
        checkEq(client.lastTxOverReport().leadingDrySamples, 0, "a new over starts from nothing");

        client.setTxTestTone(0.0, 0.5);
        client.setMox(true);
        send(client, 4);
        client.setMox(false);
        client.setTxTestTone(0.0, 0.0);
        checkEq(client.lastTxOverReport().keyedSamples, kPacket, "a TUNE leaves the report alone");
    }

    // A filter's pre-ringing is non-zero long before the audio arrives, so the
    // first non-zero sample alone says nothing about when the signal is there.
    {
        MetisClient client;
        startOver(client);
        client.queueTxIq(std::vector<std::complex<float>>(
            2 * kPacket, std::complex<float>(1e-4f, 0.0f)));  // -80 dBFS
        client.queueTxIq(std::vector<std::complex<float>>(
            kPacket, std::complex<float>(0.05f, 0.0f)));      // -26 dBFS
        client.queueTxIq(std::vector<std::complex<float>>(
            MetisClient::kTxPrimeSamples, std::complex<float>(0.5f, 0.0f)));  // -6 dBFS
        send(client, 4);
        client.setMox(false);

        const Hl2TxOverReport r = client.lastTxOverReport();
        checkEq(r.firstSignalSample, 0, "pre-ringing is non-zero");
        checkEq(r.riseSample(-60), 2 * kPacket, "first above -60 dBFS");
        checkEq(r.riseSample(-20), 3 * kPacket, "first above -20 dBFS");
        checkEq(r.riseSample(-10), 3 * kPacket, "first above -10 dBFS");
        check(r.peakDbfs > -6.1f && r.peakDbfs < -5.9f, "over peak");
    }

    // The first audio of an over waits for one DSP block in the queue, so one late
    // delivery early on is not a keyed gap. MOX does not wait (upstream #6052).
    {
        MetisClient client;
        startOver(client);
        queueSignal(client, MetisClient::kTxPrimeSamples - 1);
        send(client, 4);
        checkEq(static_cast<int>(client.txQueueDepth()), MetisClient::kTxPrimeSamples - 1,
                "an unprimed queue is held");

        queueSignal(client, 1);
        send(client, 1);
        checkEq(static_cast<int>(client.txQueueDepth()), MetisClient::kTxPrimeSamples - kPacket,
                "a primed queue flows");

        // Once per over: a queue that runs short later is not held again.
        send(client, MetisClient::kTxPrimeSamples / kPacket);
        client.flushTxIq();
        queueSignal(client, kPacket);
        send(client, 1);
        checkEq(static_cast<int>(client.txQueueDepth()), 0, "held once per over");

        client.setMox(false);
        checkEq(client.lastTxOverReport().leadingDrySamples, 4 * kPacket,
                "the hold reads as queue time, not chain zeros");

        client.setMox(true);
        queueSignal(client, kPacket);
        send(client, 1);
        checkEq(static_cast<int>(client.txQueueDepth()), kPacket, "the next over is held again");
    }

    // Unkeyed packets are not an over.
    {
        MetisClient client;
        MetisClientTestAccess::setStreaming(client);
        client.enableTransmit(true);
        send(client, 4);
        check(client.lastTxOverReport().keyedSamples == 0, "nothing keyed, nothing reported");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("hl2_tx_over_report_test: pass");
    return 0;
}
