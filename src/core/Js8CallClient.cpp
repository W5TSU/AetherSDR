#include "Js8CallClient.h"
#include "Js8CallParser.h"
#include "LogManager.h"

#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace AetherSDR {

// Cap the line-assembly buffer. JS8Call's JSON lines are tiny (well under
// 1 KB even for a full RX.DIRECTED text), so a buggy peer or a stalled
// connection dribbling bytes with no '\n' hitting even 1 MiB is already far
// past any legitimate message — same defensive pattern as
// DxClusterClient::kMaxReadBuffer (issue #2955 / GHSA-7w4w-wfqm-wh93), just
// scaled down to this protocol's much smaller message size.
static constexpr int kMaxReadBuffer = 1 * 1024 * 1024;

Js8CallClient::Js8CallClient(QObject* parent)
    : QObject(parent)
{
    // Socket and timer are created in initialize() on the SpotClients thread (#1929).
}

void Js8CallClient::initialize()
{
    if (m_socket) return;  // already initialized

    m_socket = new QTcpSocket(this);
    m_reconnectTimer = new QTimer(this);

    connect(m_socket, &QTcpSocket::connected,    this, &Js8CallClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &Js8CallClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,    this, &Js8CallClient::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &Js8CallClient::onSocketError);

    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &Js8CallClient::onReconnectTimer);
}

Js8CallClient::~Js8CallClient()
{
    m_intentionalDisconnect = true;
    if (m_reconnectTimer) m_reconnectTimer->stop();
    m_logFile.close();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

QString Js8CallClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/AetherSDR/spothub/js8call.log";
}

void Js8CallClient::connectToJs8Call(const QString& host, quint16 port)
{
    if (m_connected || m_socket->state() == QAbstractSocket::ConnectingState) {
        qCWarning(lcDxCluster) << "Js8CallClient: connect attempt already in progress";
        return;
    }

    m_host = host;
    m_port = port;
    m_intentionalDisconnect = false;
    m_readBuffer.clear();

    qCDebug(lcDxCluster) << "Js8CallClient: connecting to" << host << ":" << port;
    m_socket->connectToHost(host, port);

    // Connection timeout — capture epoch so a stale timeout from attempt N
    // cannot abort a later attempt that has already succeeded (#2380).
    const int epoch = ++m_connectEpoch;
    QTimer::singleShot(ConnectTimeoutMs, this, [this, epoch] {
        if (m_connectEpoch != epoch) return;  // superseded by a later attempt
        if (!m_connected && m_socket->state() != QAbstractSocket::ConnectedState) {
            qCWarning(lcDxCluster) << "Js8CallClient: connection timeout";
            m_socket->abort();
            emit connectionError("Connection timeout");
            scheduleReconnect();
        }
    });
}

void Js8CallClient::disconnectFromJs8Call()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    m_socket->disconnectFromHost();
}

// ── Socket slots ────────────────────────────────────────────────────────────

void Js8CallClient::onConnected()
{
    qCDebug(lcDxCluster) << "Js8CallClient: TCP connected to" << m_host;
    m_connected = true;
    m_reconnectAttempts = 0;
    m_notedNonSpotTypes.clear();

    // Open log file (truncate on each new connection)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- Connected to %1:%2 at %3 ---\n")
            .arg(m_host).arg(m_port)
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"))
            .toUtf8());
        m_logFile.flush();
    }

    emit connected();
}

void Js8CallClient::onDisconnected()
{
    qCDebug(lcDxCluster) << "Js8CallClient: disconnected";
    const bool wasConnected = m_connected;
    m_connected = false;

    if (wasConnected)
        emit disconnected();

    scheduleReconnect();
}

void Js8CallClient::onSocketError(QAbstractSocket::SocketError /*err*/)
{
    const QString msg = m_socket->errorString();
    qCWarning(lcDxCluster) << "Js8CallClient: socket error:" << msg;
    emit connectionError(msg);
    // When a connect attempt fails (ConnectingState -> error), Qt does NOT
    // emit disconnected(), so onDisconnected() never fires. Arm the
    // reconnect timer here so the chain continues after a failed attempt,
    // same as DxClusterClient (#2380).
    if (!m_connected) {
        scheduleReconnect();
    }
}

void Js8CallClient::scheduleReconnect()
{
    if (m_intentionalDisconnect) return;
    if (m_reconnectTimer->isActive()) return;  // already scheduled — don't compound backoff
    const int delay = std::min(InitialReconnectDelayMs * (1 << m_reconnectAttempts),
                               MaxReconnectDelayMs);
    qCDebug(lcDxCluster) << "Js8CallClient: reconnecting in" << delay
                         << "ms (attempt" << m_reconnectAttempts + 1 << ")";
    m_reconnectTimer->start(delay);
    m_reconnectAttempts++;
}

void Js8CallClient::onReconnectTimer()
{
    if (m_intentionalDisconnect) return;
    qCDebug(lcDxCluster) << "Js8CallClient: attempting reconnect";
    connectToJs8Call(m_host, m_port);
}

// ── Line-buffered read ──────────────────────────────────────────────────────

void Js8CallClient::onReadyRead()
{
    m_readBuffer.append(m_socket->readAll());
    if (m_readBuffer.size() > kMaxReadBuffer) {
        qCWarning(lcDxCluster) << "Js8CallClient: read buffer exceeded"
                               << kMaxReadBuffer << "bytes without newline — disconnecting";
        m_socket->disconnectFromHost();
        m_readBuffer.clear();
        return;
    }

    while (true) {
        const int idx = m_readBuffer.indexOf('\n');
        if (idx < 0) break;

        const QByteArray line = m_readBuffer.left(idx).trimmed();
        m_readBuffer.remove(0, idx + 1);
        if (line.isEmpty()) continue;

        if (m_logFile.isOpen()) {
            m_logFile.write(line + "\n");
            m_logFile.flush();
        }

        Js8CallSpot js8Spot;
        if (!Js8CallParser::parseLine(line, js8Spot)) {
            // JS8Call sends many message types over this one socket (RIG.*,
            // STATION.*, RX.ACTIVITY, ...); note each once rather than
            // logging every line, same reasoning as
            // N1MMSpotClient::m_notedNonSpotRoots.
            const QString type = Js8CallParser::messageType(line);
            if (!type.isEmpty() && !m_notedNonSpotTypes.contains(type)) {
                m_notedNonSpotTypes.insert(type);
                emit rawLineReceived(
                    QString("--- also receiving <%1> messages on this connection; "
                            "not spot-worthy ---").arg(type));
            }
            continue;
        }

        const QString when = js8Spot.utcTime.isValid()
                           ? js8Spot.utcTime.toString("HH:mm:ss")
                           : QDateTime::currentDateTimeUtc().toString("HH:mm:ss");
        QString consoleLine = QString("%1  %2  %3 MHz")
                           .arg(when, js8Spot.dxCall.leftJustified(12),
                                QString::number(js8Spot.freqMhz, 'f', 4));
        if (js8Spot.snr != 0)
            consoleLine += QString("  %1 dB").arg(js8Spot.snr);
        if (!js8Spot.comment.isEmpty())
            consoleLine += "  " + js8Spot.comment;
        emit rawLineReceived(consoleLine);

        DxSpot spot;
        spot.dxCall = js8Spot.dxCall;
        spot.freqMhz = js8Spot.freqMhz;
        spot.spotterCall = QStringLiteral("JS8Call");
        spot.comment = js8Spot.comment;
        spot.utcTime = js8Spot.utcTime;
        spot.source = QStringLiteral("JS8Call");
        spot.snr = js8Spot.snr;
        emit spotReceived(spot);
    }
}

} // namespace AetherSDR
