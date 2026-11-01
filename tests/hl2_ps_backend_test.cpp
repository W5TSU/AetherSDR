// PureSignal in Hl2Backend: when it may turn on and why not, the four-receiver
// layout with at most two slices, the two-tone behind the TUNE gates, and the
// settings it keeps. Nothing is keyed and no socket carries anything: the backend
// is default-constructed, with `m_connected` set by hand so the layout runs
// against a MetisClient that is not streaming.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2MiscOptionsSettings.h"
#include "core/backends/hl2/MetisClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>
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
    static void setSerial(Hl2Backend& b, const QString& serial) { b.m_radioSerial = serial; }
    static void setBandKey(Hl2Backend& b, const QString& band) { b.m_currentBandKey = band; }
    static void changeBand(Hl2Backend& b, double hz) { b.applyPerBandStateFor(hz, "test"); }
    static QString correctionPath(const Hl2Backend& b, const QString& band)
    {
        return b.psCorrectionPath(band);
    }
    static RadioSettingsScope scope(const Hl2Backend& b) { return b.hl2SettingsScope(); }
    static bool active(const Hl2Backend& b) { return b.m_psActive; }
    static void spanDone(Hl2Backend& b, int rate)
    {
        b.finishSpanRebuild(rate, b.m_sampleRateHz, b.m_connectGeneration, SpanRebuildOutcome::Ok);
    }
    static double alcTarget(const Hl2Backend& b) { return b.m_alcTargetPeak; }
    static bool keyed(const Hl2Backend& b) { return b.m_keyed; }
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

bool writeGarbage(const QString& path)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write("not a correction");
    return true;
}

template <typename Pred>
bool waitFor(Pred pred, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return pred();
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-hl2-ps-backend-test"));
    QCoreApplication app(argc, argv);
    using A = Hl2PureSignalTestAccess;

    check(settingsProfile.isValid(), "isolated settings profile is available");
    AppSettings::instance().load();

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

    {
        // Remembered across restarts, and turning on never keys.
        {
            Hl2Backend b;
            psSet(b, true);
        }
        {
            Hl2Backend b;
            check(row(b, "psState").toString() == QStringLiteral("idle"),
                  "PS on is remembered by a new backend");
            A::setConnected(b, true);
            emit b.connected();
            QCoreApplication::processEvents();
            check(A::wireCount(b) == 4, "and comes back on connect");
            check(!A::keyed(b), "and nothing keys");
            psSet(b, false);
            A::setConnected(b, false);
        }
        {
            Hl2Backend b;
            check(row(b, "psState").toString() == QStringLiteral("off"), "PS off is remembered too");
        }
    }

    {
        // The band's receive gain and correction.
        Hl2Backend b;
        A::setSerial(b, QStringLiteral("00:1c:c0:a2:13:dd"));
        A::setBandKey(b, QStringLiteral("20m"));
        // A curve is only for the drive it was measured at.
        Hl2MiscOptionsSettings::setPureSignalCurve(A::scope(b), QStringLiteral("20m"), 4.6, 77);
        const QString stale = A::correctionPath(b, QStringLiteral("20m"));
        check(writeGarbage(stale), "premise: a file from another drive");
        A::setConnected(b, true);
        psSet(b, true);
        QThread::msleep(800);
        QCoreApplication::processEvents();
        check(QFile::exists(stale), "a curve from another drive is not even tried");
        psSet(b, false);
        A::setConnected(b, false);
        Hl2MiscOptionsSettings::setPureSignalCurve(A::scope(b), QStringLiteral("20m"), 4.6, 100);
        Hl2MiscOptionsSettings::setPureSignalCurve(A::scope(b), QStringLiteral("40m"), 4.6, 100);
        Hl2MiscOptionsSettings::setPureSignalGainDb(A::scope(b), QStringLiteral("20m"), 14);
        check(Hl2MiscOptionsSettings::pureSignalGainDb(A::scope(b), QStringLiteral("20m")) == 14,
              "a band's gain is stored");
        check(!Hl2MiscOptionsSettings::pureSignalGainDb(A::scope(b), QStringLiteral("40m")),
              "another band has none");

        const QString path = A::correctionPath(b, QStringLiteral("20m"));
        check(path.endsWith(QStringLiteral("20m.corr")), "the correction file is named for its band");
        check(writeGarbage(path), "premise: a corrupt correction file is in place");

        A::setConnected(b, true);
        psSet(b, true);
        check(row(b, "txLnaGainDb").toInt() == 14, "PS on starts from the band's stored gain");
        check(waitFor([&] { return !QFile::exists(path); }, 3000),
              "a corrupt file is thrown away");
        check(waitFor([&] { return row(b, "psState").toString() != QStringLiteral("correcting"); }, 3000),
              "and it does not read correcting");
        check(!path.contains(QLatin1Char(':')) || path.indexOf(QLatin1Char(':')) == 1,
              "and the path has no colon a Windows file name cannot hold");

        const QString other = A::correctionPath(b, QStringLiteral("40m"));
        check(writeGarbage(other), "premise: a corrupt file for 40 m");
        A::changeBand(b, 7100000.0);
        check(row(b, "txLnaGainDb").toInt() == -12, "a band with no gain starts at the minimum");
        check(waitFor([&] { return !QFile::exists(other); }, 3000),
              "changing band tries that band's file");
        psSet(b, false);
        A::setConnected(b, false);
    }

    {
        // Review finding: an auto-reconnect has no disconnectRadio, and left PS
        // reading on with a new session running none of it.
        Hl2Backend b;
        A::setConnected(b, true);
        psSet(b, true);
        check(A::active(b), "premise: PS is up");
        RadioConnectRequest req;
        req.host = QStringLiteral("127.0.0.1");
        req.port = 9;
        b.connectRadio(req);
        check(!A::active(b), "a new session starts with PS's layout gone");
        check(row(b, "psState").toString() == QStringLiteral("idle"), "still wanted, so idle");
        b.disconnectRadio();
        psSet(b, false);
    }

    {
        // Review finding: remembered slices come back after connected().
        Hl2Backend b;
        psSet(b, true);
        A::setConnected(b, true);
        emit b.connected();
        A::addSlice(b);
        A::addSlice(b);
        QCoreApplication::processEvents();
        check(!A::active(b), "three remembered slices: PS does not silently take one");
        check(A::wireCount(b) == 3, "all three stay");
        A::setConnected(b, false);
        psSet(b, false);
    }

    {
        // Review finding: a refusal at connect left the button lit, for good.
        Hl2Backend b;
        psSet(b, true);
        bool lit = true;
        QString why;
        QObject::connect(&b, &IRadioBackend::pureSignalStatusChanged, &b,
                         [&](bool on, const QString&, int, const QString& refusal) {
            lit = on;
            why = refusal;
        });
        A::setConnected(b, true);
        A::setSampleRate(b, 384000);
        emit b.connected();
        QCoreApplication::processEvents();
        check(!lit && why.contains(QStringLiteral("span")), "refused at connect: not lit, and says why");
        check(row(b, "psState").toString() == QStringLiteral("off"), "and reads off");
        A::spanDone(b, 48000);
        QCoreApplication::processEvents();
        check(A::active(b), "a narrower span brings it back");
        psSet(b, false);
        A::setConnected(b, false);
    }

    if (g_failures == 0)
        std::printf("hl2_ps_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
