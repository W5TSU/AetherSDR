#pragma once

#include "core/CatPort.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace AetherSDR {

// CAT Control status tile.
//
// Configuration (enable, per-listener port / dialect / VFO) lives in
// Radio Setup ▸ EXTERNAL CONTROL ▸ CAT (issue #17). This tile is read-only:
// it shows whether the CAT server is enabled and, per running listener, its
// port, dialect and connected-client count. The "CAT settings…" button opens
// the Radio Setup page.
class CatControlApplet : public QWidget {
    Q_OBJECT

public:
    explicit CatControlApplet(QWidget* parent = nullptr);

    // Wire the backing CatPort objects so the tile can show live status.
    void setPorts(CatPort** ports, int count);

    // Reflect the master "Enable CAT server" state on the tile.
    void setCatEnabled(bool on);

    // Retained for call-site compatibility; the tile no longer scales any
    // VFO selectors, so this is a no-op.
    void setMaxSlices(int) {}

signals:
    // The "CAT settings…" button was clicked — MainWindow opens Radio Setup.
    void openSettingsRequested();

private:
    void refresh();

    static constexpr int kMaxPorts = 8;

    CatPort*     m_ports[kMaxPorts]{};
    int          m_portCount{0};
    bool         m_enabled{false};

    QLabel*      m_statusDot{nullptr};
    QLabel*      m_statusText{nullptr};
    QLabel*      m_listenerList{nullptr};
    QPushButton* m_settingsBtn{nullptr};
};

} // namespace AetherSDR
