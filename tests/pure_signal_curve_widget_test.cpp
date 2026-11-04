// The PureSignal plots: the ranges they pick, and that what they're given gets drawn.
// Pass a path to save what the widget drew, to look at it.

#include "gui/PureSignalCurveWidget.h"

#include <QApplication>
#include <QImage>
#include <QPixmap>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;
using YAxis = PureSignalCurveWidget::YAxis;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static QImage draw(PureSignalCurveWidget& w)
{
    w.resize(320, 240);
    return w.grab().toImage();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    {
        const auto [low, high] = PureSignalCurveWidget::yRange(YAxis::Level, 1.0, {{0.5, 3.0}});
        check(low == 0.0 && high == 1.0, "a level plot is always 0..1");
    }
    {
        const auto [low, high] = PureSignalCurveWidget::yRange(YAxis::AroundZero, 5.0, {});
        check(low < -5.0 && high > 5.0 && low == -high, "empty phase has its minimum span");
    }
    {
        const auto [low, high] =
            PureSignalCurveWidget::yRange(YAxis::AroundZero, 5.0, {{0.5, -20.0}, {0.6, 3.0}});
        check(low <= -20.0 && high >= 20.0 && low == -high, "phase stays symmetric");
    }
    {
        const auto [low, high] =
            PureSignalCurveWidget::yRange(YAxis::Fitted, 0.2, {{0.0, 1.0}, {1.0, 1.01}});
        check(high - low >= 0.2 && low < 1.0 && high > 1.01, "a flat curve keeps its minimum span");
    }
    {
        const auto [low, high] =
            PureSignalCurveWidget::yRange(YAxis::Fitted, 0.2, {{0.0, NAN}, {1.0, 1.0}});
        check(std::isfinite(low) && std::isfinite(high), "a bad point can't wreck the range");
    }

    // Each layer changes the picture.
    {
        PureSignalCurveWidget w(QStringLiteral("Amplitude"), QStringLiteral("drive"),
                                QStringLiteral("output"), YAxis::Level, 1.0);
        const QImage empty = draw(w);

        QVector<QPointF> bowed;
        for (int i = 1; i <= 400; ++i) {
            const double drive = i / 400.0;
            bowed.append({drive, std::tanh(1.5 * drive) / std::tanh(1.5)});
        }
        w.setSamples(bowed);
        const QImage samples = draw(w);
        check(samples != empty, "samples are drawn");

        QVector<QPointF> line;
        for (int i = 0; i <= 50; ++i)
            line.append({i / 50.0, 0.2 + 0.5 * i / 50.0});
        w.setLine(line);
        const QImage withLine = draw(w);
        check(withLine != samples, "the line is drawn");

        QVector<QPointF> held;
        for (int i = 1; i <= 400; ++i)
            held.append({i / 400.0, i / 400.0 * 0.8});
        w.setHeld(held);
        const QImage withHeld = draw(w);
        check(withHeld != withLine, "the held curve is drawn");
        if (argc > 1)
            withHeld.save(QString::fromLocal8Bit(argv[1]));
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("pure_signal_curve_widget_test: pass");
    return 0;
}
