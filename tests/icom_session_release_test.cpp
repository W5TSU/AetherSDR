// Upstream 18d220b1 (#6680) — a connected IC-705 session ends, through the
// ordinary disconnect/reconnect path, when the radio's 144-byte connection-usage
// notification says the current media was released: owner-free "available",
// correlated to this session's control SIDs, auth ID and selected radio. Any
// other usage notice leaves a healthy session alone, and a grant queued behind
// the release cannot revive it. Socket-free: the session is never started and
// its control stream is unstarted, with only its session IDs set.
#include "core/backends/icom/IcomProtocol.h"
#include "core/backends/icom/IcomSession.h"
#include "core/backends/icom/IcomStream.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace AetherSDR::icom {

struct IcomStreamTestAccess {
    static void setSessionIds(IcomStream& stream, quint32 local, quint32 remote)
    {
        stream.m_localSid = local;
        stream.m_remoteSid = remote;
    }
};

// IcomSession befriends this name; the definition is local to this test.
struct IcomCivBackendTestAccess {
    static void establish(IcomSession& s, const AuthId& auth, const RadioId& radio,
                          quint32 local, quint32 remote)
    {
        s.m_control = new IcomStream(&s);
        IcomStreamTestAccess::setSessionIds(*s.m_control, local, remote);
        s.m_authId = auth;
        s.m_radioId = radio;
        s.m_haveRadioId = true;
        s.m_connected = true;
    }
    static void control(IcomSession& s, const std::vector<std::uint8_t>& packet)
    {
        s.onControlPayload(QByteArray(reinterpret_cast<const char*>(packet.data()),
                                      static_cast<qsizetype>(packet.size())));
    }
};

}  // namespace AetherSDR::icom

using namespace AetherSDR::icom;
using Access = IcomCivBackendTestAccess;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr quint32 kLocal = 0x11112222;
constexpr quint32 kRemote = 0x33334444;
const AuthId kLoginId{0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
const RadioId kSelectedRadio{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                             0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};

// RS-BA1 connection-usage notification: reply 03/type 00, not a stream request.
std::vector<std::uint8_t> connectionUsage(std::uint32_t busy = 0)
{
    std::vector<std::uint8_t> packet(kLenConnInfo, 0);
    Header header;
    header.len = static_cast<std::uint32_t>(kLenConnInfo);
    header.sentId = kRemote;
    header.rcvdId = kLocal;
    writeHeader(packet, header);
    packet[0x13] = 0x80;   // 128-byte inner payload, big-endian.
    packet[0x14] = 0x03;
    std::copy(kLoginId.begin(), kLoginId.end(), packet.begin() + 0x1a);
    std::copy(kSelectedRadio.begin(), kSelectedRadio.end(), packet.begin() + 0x20);
    for (std::size_t byte = 0; byte < 4; ++byte) {
        packet[0x60 + byte] = static_cast<std::uint8_t>(busy >> (byte * 8));
    }
    return packet;
}

void remainsConnected(const std::vector<std::uint8_t>& packet, const char* what)
{
    IcomSession session;
    Access::establish(session, kLoginId, kSelectedRadio, kLocal, kRemote);
    QSignalSpy dropped(&session, &IcomSession::disconnected);
    Access::control(session, packet);
    Access::control(session, packet);
    check(session.isConnected() && dropped.isEmpty(), what);
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    remainsConnected(connectionUsage(1), "a busy notification leaves healthy media alone");
    std::vector<std::uint8_t> foreign = connectionUsage();
    foreign[0x08] ^= 0x01;
    remainsConnected(foreign, "a release with another remote SID is inert");
    foreign = connectionUsage();
    foreign[0x0c] ^= 0x01;
    remainsConnected(foreign, "a release with another local SID is inert");
    foreign = connectionUsage();
    foreign[0x1a] ^= 0x01;
    remainsConnected(foreign, "a release with another auth ID cannot retire this session");
    foreign = connectionUsage();
    foreign[0x20] ^= 0x01;
    remainsConnected(foreign, "another radio's availability cannot retire the selected one");
    foreign = connectionUsage();
    foreign[0x14] = 0x02;
    remainsConnected(foreign, "a reply-shaped packet is not a release");
    remainsConnected(connectionUsage(2), "unavailable is not the owner-free release");
    foreign = connectionUsage();
    foreign[0x64] = 'x';
    remainsConnected(foreign, "available with a named owner is not a released stream");
    foreign = connectionUsage();
    foreign[0x84] = 1;
    remainsConnected(foreign, "available with an owner IP is not a released stream");

    {
        IcomSession session;
        Access::establish(session, kLoginId, kSelectedRadio, kLocal, kRemote);
        QSignalSpy dropped(&session, &IcomSession::disconnected);
        QSignalSpy connected(&session, &IcomSession::connected);
        Access::control(session, connectionUsage());
        Access::control(session, connectionUsage());
        check(!session.isConnected() && dropped.count() == 1
                  && !dropped.first().at(0).toString().isEmpty(),
              "a correlated owner-free notification ends the session exactly once, with a reason");
        // One readyRead can deliver more control payloads before the deferred
        // stream teardown runs; none of them may revive the failed session.
        std::vector<std::uint8_t> grant = connectionUsage(1);
        grant[0x15] = 0x03;
        Access::control(session, grant);
        check(!session.isConnected() && connected.isEmpty() && dropped.count() == 1,
              "a payload queued behind the release cannot revive the session");
        QCoreApplication::processEvents();
        Access::control(session, connectionUsage());
        check(dropped.count() == 1, "a release after teardown emits no second terminal event");
    }

    std::printf("%s\n", g_failures == 0 ? "icom_session_release_test: all checks passed"
                                         : "icom_session_release_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
