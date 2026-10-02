// The ANT panel's second continuous gain row (HackRF's LNA).
//
// Pins: hidden until a radio publishes a label for the stage, and hidden again
// when it publishes none; the label and range are the radio's; a model update
// moves the slider without echoing a command back; an operator drag snaps to
// the step and emits only when the snapped value changes (#1498's rule).
#include "gui/SpectrumOverlayMenu.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QSlider>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QWidget parent;
    SpectrumOverlayMenu menu(&parent);

    auto* slider = parent.findChild<QSlider*>(QStringLiteral("antennaIfGainSlider"));
    report("the row's slider exists", slider != nullptr);
    if (!slider)
        return 1;
    QWidget* row = slider->parentWidget();
    QSignalSpy emitted(&menu, &SpectrumOverlayMenu::ifGainChanged);

    report("hidden on a radio that publishes no second stage", row->isHidden());

    menu.setIfGainRange(0, 40, 8, QStringLiteral("LNA"));
    report("shown once the radio publishes it", !row->isHidden());
    QLabel* name = nullptr;
    for (auto* l : row->findChildren<QLabel*>()) {
        if (l->text() == QStringLiteral("LNA:"))
            name = l;
    }
    report("labelled with the radio's own name for the stage", name != nullptr);
    report("range is the radio's", slider->minimum() == 0 && slider->maximum() == 40);
    report("step is the radio's", slider->singleStep() == 8);

    menu.setIfGain(16);
    report("a model update moves the slider", slider->value() == 16);
    report("...without echoing a command back", emitted.isEmpty());

    slider->setValue(13);   // snaps back to 16: no new step
    report("a drag within the current step snaps back", slider->value() == 16);
    report("...and emits nothing", emitted.isEmpty());

    slider->setValue(21);   // snaps to 24
    report("a drag past half a step snaps to the next one", slider->value() == 24);
    report("...and emits that step once",
           emitted.size() == 1 && emitted.at(0).at(0).toInt() == 24);

    slider->setValue(23);   // still 24
    report("further movement inside the step emits nothing more", emitted.size() == 1);

    slider->setValue(40);
    report("the top of the range is reachable and emitted",
           emitted.size() == 2 && emitted.at(1).at(0).toInt() == 40);

    menu.setIfGainRange(0, 0, 0, QString());
    report("hidden again when the next radio publishes none", row->isHidden());

    std::printf("%s\n", g_failed == 0 ? "spectrum_overlay_if_gain_test: OK"
                                      : "spectrum_overlay_if_gain_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
