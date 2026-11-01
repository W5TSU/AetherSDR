// The PS button: there only where the radio can do it, showing what the backend
// reports rather than what was clicked, and never keying anything.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
    if (!ok)
        ++g_failed;
}

QPushButton* psButton(TxApplet& applet)
{
    for (QPushButton* b : applet.findChildren<QPushButton*>()) {
        if (b->accessibleName() == QStringLiteral("PureSignal"))
            return b;
    }
    return nullptr;
}

QLabel* psLabel(TxApplet& applet)
{
    for (QLabel* l : applet.findChildren<QLabel*>()) {
        if (l->accessibleName() == QStringLiteral("PureSignal status"))
            return l;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tx-applet-ps-button-test"));
    QApplication app(argc, argv);

    {
        TxApplet applet;
        QPushButton* btn = psButton(applet);
        QLabel* label = psLabel(applet);
        report("the button exists", btn != nullptr);
        report("the status line exists", label != nullptr);
        if (!btn || !label) {
            std::printf("FAILURES\n");
            return 1;
        }
        report("hidden with no radio", btn->isHidden() && label->isHidden());

        applet.setPureSignalAvailable(true);
        report("shown where the radio can do it", !btn->isHidden() && !label->isHidden());
        report("it is not a keying control", !btn->property("aetherTxKeying").toBool());

        QSignalSpy spy(&applet, &TxApplet::pureSignalRequested);
        btn->click();
        report("a click asks for it", spy.count() == 1 && spy.at(0).at(0).toBool());
        report("the click alone does not light it", !btn->isChecked());

        applet.showPureSignalStatus(true, QStringLiteral("idle"), -1, QString());
        report("the backend's report lights it", btn->isChecked());
        report("the line says the state", label->text().contains(QStringLiteral("idle")));

        applet.showPureSignalStatus(true, QStringLiteral("correcting"), 150, QString());
        report("the line has the level", label->text().contains(QStringLiteral("correcting"))
                                             && label->text().contains(QStringLiteral("150")));
        report("150 is in the good band", label->property("psBand").toString() == QStringLiteral("good"));

        applet.showPureSignalStatus(true, QStringLiteral("correcting"), 100, QString());
        report("100 is a warning", label->property("psBand").toString() == QStringLiteral("warn"));
        applet.showPureSignalStatus(true, QStringLiteral("correcting"), 200, QString());
        report("200 is a warning", label->property("psBand").toString() == QStringLiteral("warn"));
        applet.showPureSignalStatus(true, QStringLiteral("feedback too weak"), 10, QString());
        report("10 is bad", label->property("psBand").toString() == QStringLiteral("bad"));
        report("the fault is on the line", label->text().contains(QStringLiteral("too weak")));

        btn->click();
        report("a click on a lit button asks for off",
               spy.count() == 2 && !spy.at(1).at(0).toBool());
        report("and it stays lit until the backend says", btn->isChecked());

        applet.showPureSignalStatus(false, QStringLiteral("off"),
                                    -1, QStringLiteral("Close a slice first."));
        report("a refusal puts the button out", !btn->isChecked());
        report("and says why", label->text().contains(QStringLiteral("Close a slice")));
        report("and the tooltip holds it for a narrow panel",
               label->toolTip().contains(QStringLiteral("Close a slice")));

        applet.setPureSignalAvailable(false);
        report("hidden again when the radio goes", btn->isHidden() && label->isHidden());
    }

    {
        // Through a real model and backend, the way MainWindow wires it.
        RadioModel radio;
        radio.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));
        TxApplet applet;
        applet.setRadioModel(&radio);
        QPushButton* btn = psButton(applet);
        QLabel* label = psLabel(applet);

        emit radio.capabilitiesChanged(true, radio.backendCapabilities());
        report("capabilities reveal the button", btn && !btn->isHidden());
        emit radio.backend()->pureSignalStatusChanged(true, QStringLiteral("calibrating"), 80, QString());
        report("the backend's status reaches the line",
               label && label->text().contains(QStringLiteral("calibrating")));
        emit radio.capabilitiesChanged(false, radio.backendCapabilities());
        report("a disconnect hides it", btn && btn->isHidden());

        emit radio.capabilitiesChanged(true, radio.backendCapabilities());
        applet.showPureSignalStatus(false, QStringLiteral("off"), -1, QString());
        btn->click();
        report("a click reaches the backend",
               radio.backend()->healthSnapshot().values.value(QStringLiteral("psState")).toString()
                   == QStringLiteral("idle"));
        report("and comes back lit", btn->isChecked());
        radio.setBackendForTest(nullptr, QString());
    }

    std::printf("%s\n", g_failed == 0 ? "all checks passed" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
