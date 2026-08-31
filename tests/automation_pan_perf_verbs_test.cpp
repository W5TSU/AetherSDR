// pan span / pan rate / perf / get dsp selector=backend — boundary behaviour a
// radio-less CI run can assert. The fixture carries a RadioModel with no
// panadapters and no backend, so a well-formed request reaches the handler and
// stops at "no panadapter" / "no backend", while a malformed one is refused by
// the parser first. If parsing drifts behind resolution the two collapse.

#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
    if (!ok) {
        ++g_failed;
    }
}

QJsonObject request(QLocalSocket& socket, const QByteArray& line)
{
    socket.write(line + '\n');
    socket.flush();

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000 && !response.contains('\n')) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        response.append(socket.readAll());
        if (!response.contains('\n')) {
            QThread::msleep(1);
        }
    }
    if (!response.contains('\n')) {
        return QJsonObject{{QStringLiteral("testError"), QStringLiteral("timeout")}};
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(response.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return QJsonObject{{QStringLiteral("testError"), error.errorString()}};
    }
    return doc.object();
}

QString errorOf(const QJsonObject& o)
{
    return o.value(QStringLiteral("error")).toString();
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir testRoot;
    if (!testRoot.isValid()) {
        std::printf("[FAIL] temporary HOME could not be created\n");
        return 1;
    }
    const QByteArray root = testRoot.path().toUtf8();
    qputenv("HOME", root);
    qputenv("XDG_CONFIG_HOME", root);
    qputenv("TMPDIR", root);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QGuiApplication app(argc, argv);

    RadioModel radio;   // no panadapters, no backend
    AutomationServer server;
    server.setRadioModel(&radio);

    const QString serverName = QStringLiteral("aethersdr-panperf-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    if (!server.start(serverName)) {
        std::printf("[FAIL] bridge did not start\n");
        return 1;
    }

    QLocalSocket socket;
    socket.connectToServer(serverName);
    if (!socket.waitForConnected(2000)) {
        std::printf("[FAIL] probe did not connect: %s\n",
                    qPrintable(socket.errorString()));
        server.stop();
        return 1;
    }
    QCoreApplication::processEvents();

    // pan span: a well-formed request parses and dispatches; setPanBandwidth is
    // a no-op with no pans but the verb still answers ok.
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("pan span 0.192"));
        report("pan span 0.192 parses and dispatches",
               r.value(QStringLiteral("ok")).toBool()
                   && qFuzzyCompare(r.value(QStringLiteral("spanMhz")).toDouble(),
                                    0.192),
               errorOf(r));
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("pan span"));
        report("pan span with no value is refused by the parser",
               r.value(QStringLiteral("ok")).toBool() == false
                   && errorOf(r).contains(QStringLiteral("MHz")),
               errorOf(r));
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("pan span abc"));
        report("pan span with a non-numeric value is refused",
               r.value(QStringLiteral("ok")).toBool() == false
                   && errorOf(r).contains(QStringLiteral("MHz")),
               errorOf(r));
    }

    // pan rate: parses, then stops at "no panadapter" — proof it reached the
    // handler rather than the parser or the unknown-verb path.
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("pan rate 30 50"));
        const QString e = errorOf(r);
        report("pan rate 30 50 reaches the handler",
               r.value(QStringLiteral("ok")).toBool() == false
                   && !e.contains(QStringLiteral("unknown"), Qt::CaseInsensitive)
                   && !e.contains(QStringLiteral("requires"), Qt::CaseInsensitive)
                   && e.contains(QStringLiteral("panadapter"), Qt::CaseInsensitive),
               e);
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("pan rate 30"));
        report("pan rate with one operand is refused by the parser",
               r.value(QStringLiteral("ok")).toBool() == false
                   && errorOf(r).contains(QStringLiteral("wfRate")),
               errorOf(r));
    }

    // perf: always answers, with the two frame-rate headline keys (0 headless).
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("perf"));
        report("perf returns panFps and wfFps",
               r.value(QStringLiteral("ok")).toBool()
                   && r.contains(QStringLiteral("panFps"))
                   && r.contains(QStringLiteral("wfFps")),
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    }

    // get dsp selector=backend: declines cleanly with no HL2 backend attached.
    {
        const QJsonObject r = request(
            socket,
            QByteArrayLiteral(
                "{\"cmd\":\"get\",\"model\":\"dsp\",\"selector\":\"backend\"}"));
        report("get dsp selector=backend declines without an HL2 backend",
               r.value(QStringLiteral("ok")).toBool() == false
                   && !r.contains(QStringLiteral("testError")),
               errorOf(r));
    }

    // pan span is now in the verb's own help text (the gen_bridge_docs source).
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("pan bogus"));
        report("pan action list in the error text mentions span and rate",
               errorOf(r).contains(QStringLiteral("span"))
                   && errorOf(r).contains(QStringLiteral("rate")),
               errorOf(r));
    }

    socket.disconnectFromServer();
    server.stop();

    std::printf("\n%s\n", g_failed == 0 ? "all rows passed" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
