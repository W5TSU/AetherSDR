// #6053 (upstream f42d432b) — the automation bridge may always switch VOX off,
// with or without TX permission, and without permission it may never switch it
// on. The Phone applet's VOX button carries the TX marker plus the disarm-only
// relaxation; the bridge's real invoke/clickAt paths are driven here. The
// vox_toggle shortcut's keysTx gate lives in MainWindow's registration and is
// checked against the running app (tools/automation_vox_guard.py).
// No radio, no socket, no transmission: a bare RadioModel's TransmitModel.

#include "TestEventLoop.h"
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/TxKeyingMarker.h"
#include "gui/PhoneApplet.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTest>

#include <atomic>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& object)
    {
        return server.handleLine(QJsonDocument(object).toJson(QJsonDocument::Compact), nullptr);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;
std::atomic<int> nameMatchWarnings{0};
QtMessageHandler previousHandler = nullptr;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    failures += ok ? 0 : 1;
}

void captureNameMatch(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (msg.contains(QStringLiteral("TX guard fell back to name match on VOX"))) {
        ++nameMatchWarnings;
    }
    if (previousHandler) {
        previousHandler(type, ctx, msg);
    }
}

const QString kVox = QStringLiteral("VOX voice-operated transmit");

QJsonObject invoke(AutomationServer& server, const QString& action, const QString& value = {})
{
    QJsonObject o{{QStringLiteral("cmd"), QStringLiteral("invoke")},
                  {QStringLiteral("target"), kVox},
                  {QStringLiteral("action"), action}};
    if (!value.isEmpty()) {
        o[QStringLiteral("value")] = value;
    }
    return AutomationServerTestAccess::request(server, o);
}

QJsonObject clickAt(AutomationServer& server)
{
    return AutomationServerTestAccess::request(server,
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("clickAt")},
                    {QStringLiteral("target"), kVox},
                    {QStringLiteral("value"), QStringLiteral("5 5")}});
}

bool ok(const QJsonObject& reply) { return reply.value(QStringLiteral("ok")).toBool(); }

// The bridge may defer an accepted invoke; a refused one must never land later.
void drain() { AetherTest::pumpFor(100); }

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-phone-applet-vox-disarm-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    previousHandler = qInstallMessageHandler(captureNameMatch);
    QApplication app(argc, argv);

    RadioModel radio;
    TransmitModel& tx = radio.transmitModel();
    PhoneApplet applet;
    applet.setTransmitModel(&tx);
    applet.show();
    AetherTest::waitFor([&] { return applet.isVisible(); }, 2000);

    QPushButton* button = nullptr;
    for (auto* b : applet.findChildren<QPushButton*>()) {
        if (b->accessibleName() == kVox) {
            button = b;
        }
    }
    check(button != nullptr, "the Phone applet has the VOX button");
    if (!button) {
        return 1;
    }

    // ── Classification ──────────────────────────────────────────────────────
    check(transmitControlMatch(button) == TransmitControlMatch::Marker,
          "the VOX button is a TX control by marker, not by a word in its name");
    check(isTxDisarmRequest(button, QStringLiteral("setChecked"), QStringLiteral("false"))
              && !isTxDisarmRequest(button, QStringLiteral("setChecked"), QStringLiteral("true"))
              && !isTxDisarmRequest(button, QStringLiteral("click"), QString())
              && !isTxDisarmRequest(button, QStringLiteral("setChecked"), QString()),
          "only an explicit setChecked off counts as a disarm");
    {
        QPushButton mox(QStringLiteral("MOX"));
        markTxKeying(&mox);
        check(!isTxDisarmRequest(&mox, QStringLiteral("setChecked"), QStringLiteral("false")),
              "a keying control without the disarm mark gets no relaxation");
    }

    AutomationServer server;
    server.setRadioModel(&radio);

    // ── Bridge, no TX permission: disarm passes, arming is refused ──────────
    server.setTxAllowed(false);
    nameMatchWarnings = 0;
    tx.setVoxEnable(true);
    check(tx.voxEnable() && button->isChecked(), "precondition: VOX armed, button shows it");
    {
        const bool off = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("false")));
        const bool landed = AetherTest::waitFor([&] { return !tx.voxEnable() && !button->isChecked(); }, 2000);
        check(off && landed, "TX not allowed: setChecked false is accepted and disarms VOX");
    }
    for (const char* spelling : {"0", "off", "no", " FALSE ", "unchecked"}) {
        tx.setVoxEnable(true);
        const bool accepted = ok(invoke(server, QStringLiteral("setChecked"),
                                        QString::fromLatin1(spelling)));
        const bool disarmed = AetherTest::waitFor([&] { return !tx.voxEnable(); }, 2000);
        const QByteArray what = QByteArray("TX not allowed: setChecked '") + spelling + "' disarms";
        check(accepted && disarmed, what.constData());
    }
    check(ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("false"))),
          "TX not allowed: disarming an already-off VOX is accepted");
    drain();
    check(!tx.voxEnable(), "TX not allowed: and VOX stays off");
    check(nameMatchWarnings == 0, "no name-fallback warning for VOX");

    {
        const bool on = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("true")));
        const bool one = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("1")));
        const bool bare = ok(invoke(server, QStringLiteral("setChecked")));
        const bool click = ok(invoke(server, QStringLiteral("click")));
        const bool toggle = ok(invoke(server, QStringLiteral("toggle")));
        const bool pointer = ok(clickAt(server));
        drain();
        check(!on && !one, "TX not allowed: setChecked true is refused");
        check(!bare, "TX not allowed: setChecked without a value is refused");
        check(!click && !toggle, "TX not allowed: click and toggle are refused");
        check(!pointer, "TX not allowed: clickAt on the button is refused");
        check(!tx.voxEnable() && !button->isChecked(),
              "TX not allowed: nothing armed VOX behind a refusal");
    }
    {
        // Toggling verbs on an armed VOX are not a disarm.
        tx.setVoxEnable(true);
        const bool click = ok(invoke(server, QStringLiteral("click")));
        const bool toggle = ok(invoke(server, QStringLiteral("toggle")));
        const bool pointer = ok(clickAt(server));
        drain();
        check(!click && !toggle && !pointer && tx.voxEnable(),
              "TX not allowed: click, toggle and clickAt on an armed VOX are refused");
        tx.setVoxEnable(false);
    }

    // ── Bridge, TX permission granted: unchanged, every verb works ──────────
    server.setTxAllowed(true);
    {
        const bool on = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("true")));
        const bool armed = AetherTest::waitFor([&] { return tx.voxEnable(); }, 2000);
        check(on && armed, "TX allowed: setChecked true arms VOX (operator opted in)");
        const bool off = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("false")));
        const bool disarmed = AetherTest::waitFor([&] { return !tx.voxEnable(); }, 2000);
        check(off && disarmed, "TX allowed: setChecked false disarms VOX");
    }
    server.setTxAllowed(false);

    // ── The operator's own input is untouched ───────────────────────────────
    QTest::mouseClick(button, Qt::LeftButton);
    check(tx.voxEnable() && button->isChecked(), "operator: a mouse click arms VOX");
    QTest::mouseClick(button, Qt::LeftButton);
    check(!tx.voxEnable() && !button->isChecked(), "operator: a mouse click disarms VOX");

    qInstallMessageHandler(previousHandler);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
