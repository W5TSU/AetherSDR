// The PureSignal window with no radio behind it: it draws what it's given, and its
// transmit button can't key anything without a connected radio.
// Pass a path to save what it drew, to look at it.

#include "core/TxKeyingMarker.h"
#include "gui/PureSignalDialog.h"

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QPushButton>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static PureSignalCurves sampleCurves(int calibration, double squeeze)
{
    PureSignalCurves c;
    c.calibration = calibration;
    for (int i = 1; i <= 800; ++i) {
        const double drive = i / 800.0;
        const double output = std::tanh(squeeze * drive) / std::tanh(squeeze);
        c.amplitude.append({drive, output});
        c.phaseDeg.append({output, 8.0 * squeeze * output * output - 8.0 * squeeze});
    }
    for (int i = 0; i < 512; ++i) {
        const double x = i / 511.0;
        c.correctionGain.append({x, 0.8 + 0.3 * x * x * squeeze});
        c.correctionPhaseDeg.append({x, 7.0 * squeeze * (1.0 - x * x)});
    }
    return c;
}

static QPushButton* button(PureSignalDialog& d, const QString& text)
{
    for (QPushButton* b : d.findChildren<QPushButton*>()) {
        if (b->text().startsWith(text))
            return b;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    PureSignalDialog dialog(nullptr);
    dialog.resize(900, 560);
    dialog.show();
    app.processEvents();

    QPushButton* transmit = button(dialog, QStringLiteral("Transmit"));
    check(transmit != nullptr, "a transmit button");
    check(transmit && transmit->property(kTxKeyingProperty).toBool(),
          "it's marked as keying the transmitter");
    if (transmit) {
        transmit->click();
        check(transmit->text() == QStringLiteral("Transmit"), "no radio: no run starts");
    }

    QPushButton* hold = button(dialog, QStringLiteral("Hold curve"));
    QPushButton* clear = button(dialog, QStringLiteral("Clear held"));
    check(hold && !hold->isEnabled(), "nothing to hold before a calibration");

    const QImage empty = dialog.grab().toImage();
    dialog.showCurves(sampleCurves(3, 1.5));
    app.processEvents();
    const QImage drawn = dialog.grab().toImage();
    check(drawn != empty, "curves are drawn");
    check(hold && hold->isEnabled(), "a calibration can be held");

    hold->click();
    check(clear && clear->isEnabled(), "a held curve can be cleared");
    dialog.showCurves(sampleCurves(9, 0.7));
    app.processEvents();
    const QImage compared = dialog.grab().toImage();
    check(compared != drawn, "the held curve stays behind the new one");
    if (argc > 1)
        compared.save(QString::fromLocal8Bit(argv[1]));

    clear->click();
    check(!clear->isEnabled(), "cleared");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("pure_signal_dialog_test: pass");
    return 0;
}
