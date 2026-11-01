// PureSignal in Hl2Backend: when it may turn on and why not, the four-receiver
// layout with at most two slices, the two-tone behind the TUNE gates, and the
// settings it keeps. Nothing is keyed and no socket carries anything: the backend
// is default-constructed, with `m_connected` set by hand so the layout runs
// against a MetisClient that is not streaming.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <cstdio>
#include <memory>

namespace AetherSDR::hl2 {
struct Hl2PureSignalTestAccess {
    static void setConnected(Hl2Backend& b, bool on) { b.m_connected = on; }
    static void setSampleRate(Hl2Backend& b, int hz) { b.m_sampleRateHz = hz; }
    static void setKeyed(Hl2Backend& b, bool on) { b.m_keyed = on; }
    static void addSlice(Hl2Backend& b)
    {
        b.m_ids.append();
        b.m_rx.emplace_back();
    }
    static int wireCount(const Hl2Backend& b) { return b.wireReceiverCount(); }
    static int metisCount(Hl2Backend& b)
    {
        int n = 0;
        QMetaObject::invokeMethod(b.m_metis, [&] { n = b.m_metis->effectiveNumRx(); },
                                  Qt::BlockingQueuedConnection);
        return n;
    }
    static double alcTarget(const Hl2Backend& b) { return b.m_alcTargetPeak; }
    static bool twoToneRunning(Hl2Backend& b)
    {
        bool on = false;
        QMetaObject::invokeMethod(b.m_metis, [&] { on = b.m_metis->txTestToneEnabled(); },
                                  Qt::BlockingQueuedConnection);
        return on;
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
    else     { std::fprintf(stderr, "[ OK ] %s\n", what); }
}

namespace {

struct Answer {
    int results = 0;
    int errors = 0;
    QString message;
};

Answer psSet(Hl2Backend& backend, bool on)
{
    Answer a;
    auto ok = QObject::connect(&backend, &IRadioBackend::extensionResult, &backend,
                               [&a](quint64, const QVariant&) { ++a.results; });
    auto bad = QObject::connect(&backend, &IRadioBackend::extensionError, &backend,
                                [&a](quint64, const QString& m) { ++a.errors; a.message = m; });
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ps.set"), 7, QVariant(on));
    QObject::disconnect(ok);
    QObject::disconnect(bad);
    return a;
}

QVariant row(Hl2Backend& backend, const char* key)
{
    return backend.healthSnapshot().values.value(QString::fromLatin1(key));
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-hl2-ps-backend-test"));
    QCoreApplication app(argc, argv);
    using A = Hl2PureSignalTestAccess;

    check(settingsProfile.isValid(), "isolated settings profile is available");

    {
        Hl2Backend b;
        check(b.capabilities().hasPureSignal, "the HL2 with transmit allowed declares PureSignal");
        check(b.capabilities().twoToneGenerator.has_value(), "and a two-tone generator");
        check(row(b, "psState").toString() == QStringLiteral("off"), "PS reads off before anything asks");
        check(!row(b, "psFeedbackLevel").isValid(), "no feedback level row while off");
    }

    {
        Hl2Backend b;
        A::setConnected(b, true);
        A::addSlice(b);
        A::addSlice(b);
        const Answer a = psSet(b, true);
        check(a.errors == 1 && a.message.contains(QStringLiteral("slices")),
              "three slices open: refused, and the reason says so");
        check(A::wireCount(b) == 3, "and the receivers are untouched");
        check(row(b, "psState").toString() == QStringLiteral("off"), "and it still reads off");
        A::setConnected(b, false);
    }

    {
        Hl2Backend b;
        A::setConnected(b, true);
        A::setSampleRate(b, 384000);
        const Answer a = psSet(b, true);
        check(a.errors == 1 && a.message.contains(QStringLiteral("span")),
              "a span that cannot deliver four receivers: refused, with the reason");
        A::setConnected(b, false);
    }

    {
        Hl2Backend b;
        A::setConnected(b, true);
        A::setKeyed(b, true);
        const Answer a = psSet(b, true);
        check(a.errors == 1 && a.message.contains(QStringLiteral("transmitting")),
              "while transmitting: refused");
        A::setKeyed(b, false);
        A::setConnected(b, false);
    }

    {
        Hl2Backend b;
        A::setConnected(b, true);
        check(A::wireCount(b) == 1, "PS off: the wire runs what the slices need");
        check(A::alcTarget(b) == 0.85, "PS off: the ALC target is the shipped one");

        const Answer on = psSet(b, true);
        check(on.errors == 0 && on.results == 1, "one slice: PS turns on");
        check(row(b, "psState").toString() == QStringLiteral("idle"), "and reads idle, unkeyed");
        check(A::wireCount(b) == 4, "the wire runs four receivers");
        check(A::metisCount(b) == 4, "and MetisClient was told");
        check(b.capabilities().maxSlices == 2, "only two are left for slices");
        check(A::alcTarget(b) == 0.99, "the ALC reaches calcc's top bucket");
        check(row(b, "txLnaGainDb").toInt() == -12, "the receive gain starts at its minimum");

        b.invokeExtension(QStringLiteral("hl2"), QStringLiteral("txlnagain.set"), 8, QVariant(10));
        check(row(b, "txLnaGainDb").toInt() == -12, "txlnagain cannot fight the level loop");

        A::addSlice(b);
        check(A::wireCount(b) == 4, "a second slice uses the same four");

        const Answer off = psSet(b, false);
        check(off.errors == 0, "PS turns off");
        check(row(b, "psState").toString() == QStringLiteral("off"), "reads off");
        check(A::wireCount(b) == 2, "the wire goes back to the two slices");
        check(A::metisCount(b) == 2, "and MetisClient follows");
        check(b.capabilities().maxSlices > 2, "the slices are given back");
        check(A::alcTarget(b) == 0.85, "the ALC target goes back");
        check(row(b, "txLnaGainDb").toString() == QStringLiteral("off"),
              "and the receive gain goes back to what the operator had");
        A::setConnected(b, false);
    }

    {
        Hl2Backend b;
        A::setConnected(b, true);
        psSet(b, true);
        A::setKeyed(b, true);
        const Answer off = psSet(b, false);
        check(off.errors == 0, "off while transmitting is always allowed");
        check(row(b, "psState").toString() == QStringLiteral("off"), "and correction reads off at once");
        check(A::wireCount(b) == 4, "the layout waits for unkey");
        A::setKeyed(b, false);
        b.setTune(false, -1);
        QCoreApplication::processEvents();
        check(A::wireCount(b) == 1, "and returns once unkeyed");
        A::setConnected(b, false);
    }

    {
        // Disconnected: recorded, and what the button shows.
        Hl2Backend b;
        const Answer on = psSet(b, true);
        check(on.errors == 0, "disconnected: PS on is recorded");
        check(row(b, "psState").toString() == QStringLiteral("idle"), "reads idle");
        check(A::wireCount(b) == 1, "the layout waits for a radio");
    }

    {
        // The two-tone behind TUNE: same path, ended the same way.
        Hl2Backend b;
        check(!A::twoToneRunning(b), "nothing running");
        b.setTuneTwoTone(true);
        b.setTune(true, 10);
        check(A::twoToneRunning(b), "TUNE in two-tone runs the two-tone");
        b.setTune(false, 10);
        check(!A::twoToneRunning(b), "released: it stops");
    }

    if (g_failures == 0)
        std::printf("hl2_ps_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
