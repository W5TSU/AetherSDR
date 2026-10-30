// Upstream 2e02f0d7 (#6599) — a DVK panel shorter than its 12 slot rows
// scrolls instead of squashing each row below its F-key, a long recording
// name cannot push the F-key or duration out of view, and a tall panel still
// shows every row without a scroll bar. A real DvkPanel, no radio.
#include "TestSettingsProfile.h"
#include "gui/DvkPanel.h"
#include "models/DvkModel.h"

#include <QApplication>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

void settle()
{
    for (int i = 0; i < 5; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

// The panel as a child with an imposed geometry, the way a short dock or
// splitter pane hands it less height than its layout asks for.
void place(QWidget& host, DvkPanel& panel, int width, int height)
{
    host.resize(width, height);
    panel.setGeometry(0, 0, width, height);
    settle();
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dvk-panel-layout-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);

    DvkModel model;
    QWidget host;
    auto* panel = new DvkPanel(&model, &host);
    host.show();

    auto* scroll = panel->findChild<QScrollArea*>(QStringLiteral("dvkSlotsScroll"));
    auto* row1 = panel->findChild<QFrame*>(QStringLiteral("dvkSlot1"));
    auto* row12 = panel->findChild<QFrame*>(QStringLiteral("dvkSlot12"));
    auto* fkey1 = panel->findChild<QPushButton*>(QStringLiteral("dvkPlaySlot1"));
    auto* name1 = panel->findChild<QLabel*>(QStringLiteral("dvkSlotName1"));
    auto* dur1 = panel->findChild<QLabel*>(QStringLiteral("dvkSlotLength1"));
    auto* rec = panel->findChild<QPushButton*>(QStringLiteral("dvkRecord"));
    report("slot grid sits in a scroll area", scroll && row1 && row12 && fkey1 && name1 && dur1 && rec);
    if (!(scroll && row1 && row12 && fkey1 && name1 && dur1 && rec)) {
        std::printf("%d/%d checks failed\n", g_failed, g_total);
        return 1;
    }

    // Short: 250 x 300 is far less than 12 readable rows plus the transport.
    place(host, *panel, 250, 300);
    report("a short panel keeps row 1 at least as tall as its F-key",
           row1->height() >= fkey1->minimumSizeHint().height());
    report("a short panel keeps the F-key inside its row",
           fkey1->geometry().bottom() < row1->height());
    report("a short panel scrolls to reach slot 12", scroll->verticalScrollBar()->maximum() > 0);
    scroll->ensureWidgetVisible(row12);
    settle();
    const QRect row12InViewport(row12->mapTo(scroll->viewport(), QPoint(0, 0)), row12->size());
    report("slot 12 can be scrolled fully into view",
           scroll->viewport()->rect().contains(row12InViewport));
    report("the transport buttons stay outside the scrolling region",
           !scroll->isAncestorOf(rec) && rec->isVisible());

    // A long unbroken name yields width; nothing scrolls sideways.
    name1->setText(QString(120, QLatin1Char('W')));
    settle();
    report("a long name leaves no horizontal scroll range",
           scroll->horizontalScrollBar()->maximum() == 0);
    report("a long name leaves the duration inside the row",
           dur1->mapTo(row1, QPoint(dur1->width(), 0)).x() <= row1->width());

    // Tall: every row fits, so nothing scrolls.
    place(host, *panel, 250, 1200);
    report("a tall panel shows every row without scrolling",
           scroll->verticalScrollBar()->maximum() == 0);

    std::printf("%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
