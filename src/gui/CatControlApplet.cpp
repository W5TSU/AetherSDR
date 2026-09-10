#include "CatControlApplet.h"
#include "core/CatSettings.h"
#include "core/ThemeManager.h"
#include "gui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

const char* kHint =
    "QLabel { color: {{color.text.secondary}}; font-size: 11px; }";
const char* kListStyle =
    "QLabel { color: {{color.text.primary}}; font-size: 11px; "
    "font-family: monospace; }";
const char* kSettingsBtn =
    "QPushButton { background: transparent; border: none; "
    "color: {{color.accent}}; font-size: 11px; text-align: left; padding: 0; }"
    "QPushButton:hover { color: {{color.accent.bright}}; }";

} // namespace

CatControlApplet::CatControlApplet(QWidget* parent) : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/cat"));
    setStyleSheet("QWidget { background: transparent; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(6);
    m_statusDot = new QLabel(QStringLiteral("●"));
    m_statusDot->setStyleSheet("QLabel { color: #708090; font-size: 12px; }");
    m_statusText = new QLabel(QStringLiteral("CAT server disabled"));
    ThemeManager::instance().applyStyleSheet(m_statusText, kHint);
    statusRow->addWidget(m_statusDot);
    statusRow->addWidget(m_statusText, 1);
    root->addLayout(statusRow);

    m_listenerList = new QLabel;
    m_listenerList->setTextFormat(Qt::PlainText);
    m_listenerList->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(m_listenerList, kListStyle);
    root->addWidget(m_listenerList);

    root->addStretch();

    m_settingsBtn = new QPushButton(QStringLiteral("CAT settings…"));
    m_settingsBtn->setFlat(true);
    m_settingsBtn->setCursor(Qt::PointingHandCursor);
    ThemeManager::instance().applyStyleSheet(m_settingsBtn, kSettingsBtn);
    connect(m_settingsBtn, &QPushButton::clicked, this,
            &CatControlApplet::openSettingsRequested);
    root->addWidget(m_settingsBtn);

    refresh();
    hide();
}

void CatControlApplet::setPorts(CatPort** ports, int count)
{
    m_portCount = qMin(count, kMaxPorts);
    for (int i = 0; i < m_portCount; ++i) {
        m_ports[i] = ports[i];
        if (m_ports[i]) {
            connect(m_ports[i], &CatPort::clientCountChanged, this,
                    [this] { refresh(); }, Qt::UniqueConnection);
        }
    }
    for (int i = m_portCount; i < kMaxPorts; ++i) {
        m_ports[i] = nullptr;
    }
    refresh();
}

void CatControlApplet::setCatEnabled(bool on)
{
    m_enabled = on;
    refresh();
}

void CatControlApplet::refresh()
{
    if (!m_statusDot) {
        return;
    }
    m_statusDot->setStyleSheet(
        m_enabled ? "QLabel { color: #00c040; font-size: 12px; }"
                  : "QLabel { color: #708090; font-size: 12px; }");
    m_statusText->setText(m_enabled ? QStringLiteral("CAT server enabled")
                                    : QStringLiteral("CAT server disabled"));

    QStringList lines;
    for (int i = 0; i < m_portCount; ++i) {
        CatPort* p = m_ports[i];
        if (!p || !p->isRunning()) {
            continue;
        }
        const QString dialect = catDialectToken(p->dialect());
        lines << QStringLiteral("%1  %2  %3 client%4")
                     .arg(p->port())
                     .arg(dialect)
                     .arg(p->clientCount())
                     .arg(p->clientCount() == 1 ? QString() : QStringLiteral("s"));
    }
    m_listenerList->setText(lines.isEmpty() ? QStringLiteral("No listeners running.")
                                            : lines.join(QLatin1Char('\n')));
}

} // namespace AetherSDR
