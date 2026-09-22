#pragma once

#include "DxClusterClient.h"  // for DxSpot

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QFile>
#include <QSet>
#include <QString>
#include <atomic>

namespace AetherSDR {

// JS8Call's TCP JSON API client (#21) — connects to JS8Call's line-delimited
// JSON socket (default 127.0.0.1:2442) and emits spotReceived() for every
// RX.SPOT / RX.DIRECTED message that names a station, feeding the same
// SpotHub pipeline as N1MMSpotClient / WsjtxClient / DxClusterClient.
//
// Unlike N1MM (explicit add/delete) JS8Call's API has no delete: a heard
// station is simply re-spotted on every decode, same as WSJT-X and FreeDV,
// and aged out by the shared per-source lifetime in MainWindow_Spots.cpp
// (isDuplicateSpot / spotLifetimeSeconds) rather than tracked by identity
// here. That's what makes a DxClusterClient-shaped TCP client (connect,
// line-buffer, exponential-backoff reconnect) the right model instead of
// N1MMSpotClient's key->id map.
class Js8CallClient : public QObject {
    Q_OBJECT

public:
    explicit Js8CallClient(QObject* parent = nullptr);
    ~Js8CallClient() override;

    void connectToJs8Call(const QString& host, quint16 port);
    void disconnectFromJs8Call();
    bool isConnected() const { return m_connected; }

    QString logFilePath() const;

public slots:
    // Defer socket + timer construction to the worker thread (#1929) — see
    // DxClusterClient::initialize().
    void initialize();

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void spotReceived(const DxSpot& spot);
    // One compact console line per accepted spot, matching the other SpotHub
    // clients. Raw JSON goes to the log file instead.
    void rawLineReceived(const QString& line);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError err);
    void onReconnectTimer();

private:
    // Arm the exponential-backoff reconnect timer, mirroring
    // DxClusterClient::scheduleReconnect() (#2380's double-scheduling guard
    // applies here too: errorOccurred + disconnected can both fire for one
    // failed attempt).
    void scheduleReconnect();

    QTcpSocket* m_socket{nullptr};
    QByteArray  m_readBuffer;
    QTimer*     m_reconnectTimer{nullptr};
    QFile       m_logFile;

    QString m_host{"127.0.0.1"};
    quint16 m_port{2442};
    std::atomic<bool> m_connected{false};
    bool    m_intentionalDisconnect{false};
    int     m_reconnectAttempts{0};
    int     m_connectEpoch{0};  // guards a stale timeout against a later attempt

    // Non-spot message types (RX.ACTIVITY, RIG.FREQ, STATION.*, ...) noted
    // once each rather than spamming the console — mirrors
    // N1MMSpotClient::m_notedNonSpotRoots.
    QSet<QString> m_notedNonSpotTypes;

    static constexpr int MaxReconnectDelayMs    = 60000;
    static constexpr int InitialReconnectDelayMs = 5000;
    static constexpr int ConnectTimeoutMs        = 10000;
};

} // namespace AetherSDR
