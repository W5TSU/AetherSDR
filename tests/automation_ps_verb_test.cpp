// `ps [status|on|off]`: PureSignal through the bridge. Answers are read back off
// the backend's health snapshot, never echoed from the request. A real,
// default-constructed Hl2Backend with no radio; nothing is keyed. A backend that
// does not declare the `hl2` namespace is refused.

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

QJsonObject ps(AutomationServer& server, const QString& action)
{
    QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("ps")}};
    if (!action.isEmpty()) {
        request[QStringLiteral("action")] = action;
    }
    return AutomationServerTestAccess::handleLine(
        server, QJsonDocument(request).toJson(QJsonDocument::Compact));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-automation-ps-verb-test"));

    qputenv("AETHER_AUTOMATION", "1");
    qputenv("AETHER_AUTOMATION_ALLOW_TX", "1");
    QCoreApplication app(argc, argv);
    std::printf("\n  ps — PureSignal through the bridge\n\n");

    check(settingsProfile.isValid(), "isolated settings profile is available");

    AudioEngine engine;
    RadioModel radio;
    AutomationServer server;
    server.setAudioEngine(&engine);
    server.setRadioModel(&radio);

    check(!ps(server, QStringLiteral("status")).value(QStringLiteral("ok")).toBool(),
          "no backend: refused, not an empty success");

    radio.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));

    {
        const QJsonObject r = ps(server, QString());
        check(r.value(QStringLiteral("ok")).toBool(), "bare verb is status and succeeds");
        check(r.value(QStringLiteral("state")).toString() == QStringLiteral("off"), "status: off");
        check(r.value(QStringLiteral("feedbackLevel")).isNull(), "status: no feedback level while off");
        check(r.value(QStringLiteral("gainDb")).isNull(), "status: no transmit gain set");
    }
    {
        const QJsonObject r = ps(server, QStringLiteral("on"));
        check(r.value(QStringLiteral("ok")).toBool(), "ps on succeeds");
        check(r.value(QStringLiteral("state")).toString() == QStringLiteral("idle"),
              "the reply reads idle back from the backend");
    }
    {
        const QJsonObject r = ps(server, QStringLiteral("off"));
        check(r.value(QStringLiteral("ok")).toBool()
                  && r.value(QStringLiteral("state")).toString() == QStringLiteral("off"),
              "ps off reads off");
    }
    {
        // Found on the radio: TransmitModel's tune mode only changes on a Flex's
        // status echo, so this path sent a single carrier. Through the model, not
        // straight at the backend, because that is where it broke.
        const auto waveform = [&radio] {
            return radio.backend()->healthSnapshot()
                .values.value(QStringLiteral("tuneWaveform")).toString();
        };
        auto& tx = radio.transmitModel();
        tx.setTuneAvailable(true);
        tx.setPttPreflight({});   // no slice here, and the gate is not what is under test
        check(waveform() == QStringLiteral("carrier"), "TUNE is a carrier by default");
        tx.startTwoToneTune();
        check(waveform() == QStringLiteral("two-tone"), "the bridge's two-tone reaches the backend");
        tx.stopTune();
        check(waveform() == QStringLiteral("carrier"), "and it does not outlive the tune");
        tx.setTuneMode(QStringLiteral("two_tone"));
        tx.startTune();
        check(waveform() == QStringLiteral("two-tone"), "the TUNE menu's Two Tone reaches it too");
        tx.stopTune();
        tx.startTune();
        check(waveform() == QStringLiteral("carrier"), "the menu's choice is for one tune only");
        tx.stopTune();
    }
    check(!ps(server, QStringLiteral("loud")).value(QStringLiteral("ok")).toBool(),
          "an unknown action is an error");

    std::printf("\n  %s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
