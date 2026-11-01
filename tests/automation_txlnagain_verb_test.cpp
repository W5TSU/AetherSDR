// `txlnagain [status|off|<dB>]` — the bridge verb for the Hermes-Lite 2's
// receive gain while transmitting (register 0x0e).
//
// Drives the verb through AutomationServer::handleLine against a real,
// default-constructed Hl2Backend (no socket, no radio, nothing keyed): the
// extension verb answers locally and the reply is read back off the backend's
// health snapshot, never echoed from the request. A backend that does not
// declare the `hl2` namespace is refused.

#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/AutomationServer.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <memory>

namespace AetherSDR {

class AutomationServerTestAccess
{
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

QJsonObject txlnagain(AutomationServer& server, const QString& action)
{
    QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("txlnagain")}};
    if (!action.isEmpty()) {
        request[QStringLiteral("action")] = action;
    }
    return AutomationServerTestAccess::handleLine(
        server, QJsonDocument(request).toJson(QJsonDocument::Compact));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-automation-txlnagain-verb-test"));

    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    std::printf("\n  txlnagain — the HL2's receive gain while transmitting\n\n");

    check(settingsProfile.isValid(), "isolated settings profile is available");

    AudioEngine engine;
    RadioModel radio;
    AutomationServer server;
    server.setAudioEngine(&engine);
    server.setRadioModel(&radio);

    {
        const QJsonObject r = txlnagain(server, QStringLiteral("status"));
        check(!r.value(QStringLiteral("ok")).toBool(),
              "no backend: refused, not an empty success");
    }

    radio.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));

    {
        const QJsonObject r = txlnagain(server, QString());
        check(r.value(QStringLiteral("ok")).toBool(), "bare verb is status and succeeds");
        check(r.value(QStringLiteral("gainDb")).isNull(), "status: off reads as a null gain");
        check(r.value(QStringLiteral("rx1IqPeakDbfs")).isNull(),
              "status: no IQ peak before any telemetry");
    }
    {
        const QJsonObject r = txlnagain(server, QStringLiteral("-6"));
        check(r.value(QStringLiteral("ok")).toBool(), "txlnagain -6 succeeds");
        check(r.value(QStringLiteral("gainDb")).toInt(999) == -6,
              "the reply reads -6 back from the backend");
    }
    {
        const QJsonObject r = txlnagain(server, QStringLiteral("60"));
        check(r.value(QStringLiteral("gainDb")).toInt(999) == 48,
              "an out-of-range gain comes back clamped (+48), not as asked");
    }
    {
        const QJsonObject r = txlnagain(server, QStringLiteral("off"));
        check(r.value(QStringLiteral("ok")).toBool()
                  && r.value(QStringLiteral("gainDb")).isNull(),
              "txlnagain off clears it");
    }
    {
        const QJsonObject r = txlnagain(server, QStringLiteral("loud"));
        check(!r.value(QStringLiteral("ok")).toBool(), "a non-numeric action is an error");
    }

    std::printf("\n  %s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
