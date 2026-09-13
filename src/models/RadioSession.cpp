#include "models/RadioSession.h"

#include "core/CatPort.h"
#include "core/ShutdownTrace.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif

namespace AetherSDR {

RadioSession::RadioSession(QObject* parent)
    : QObject(parent)
{
}

RadioSession::~RadioSession()
{
    // Destruction order is the contract (#2385): every owned server holds a
    // raw RadioModel*, so they die here — in the destructor body — before
    // the m_radioModel member destructs.
    //
    // This destructor runs as part of MainWindow's IMPLICIT member teardown
    // (m_sessions is a plain std::vector<std::unique_ptr<RadioSession>>), which
    // happens *after* ~MainWindow()'s own explicit body — and therefore after
    // that body's "main_window.destructor_body" ShutdownTrace scope has already
    // logged event=end. Everything below was invisible to a Windows force-quit
    // log until this trace was added: a hang in here (e.g. a CAT TCP listener,
    // the very socket WSJT-X/rigctld clients connect to, per an AetherSDR issue
    // where a leftover Windows process held CAT ports open until Task Manager
    // killed it) would previously show a clean-looking log ending right after
    // main_window.destructor_body with no indication teardown wasn't finished.
    ShutdownTrace destructorTrace("radio_session.destructor");
#ifdef HAVE_WEBSOCKETS
    {
        ShutdownTrace trace("radio_session.tci_server.destroy");
        shutdownTciServer();
    }
#endif
    {
        ShutdownTrace trace("radio_session.cat_ports.destroy");
        for (CatPort*& port : m_catPorts) {
            delete port;
            port = nullptr;
        }
    }
}

#ifdef HAVE_WEBSOCKETS
void RadioSession::setTciServer(TciServer* server)
{
    Q_ASSERT(!server || !server->parent());  // parent would recreate #2385
    shutdownTciServer();
    m_tciServer = server;
}

void RadioSession::shutdownTciServer()
{
    delete m_tciServer;
    m_tciServer = nullptr;
}
#endif

CatPort* RadioSession::catPort(int i) const
{
    return (i >= 0 && i < kCatPorts) ? m_catPorts[size_t(i)] : nullptr;
}

void RadioSession::setCatPort(int i, CatPort* port)
{
    if (i < 0 || i >= kCatPorts) {
        delete port;
        return;
    }
    Q_ASSERT(!port || !port->parent());
    delete m_catPorts[size_t(i)];
    m_catPorts[size_t(i)] = port;
}

} // namespace AetherSDR
