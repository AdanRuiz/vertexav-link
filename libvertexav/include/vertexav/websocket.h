// Minimal RFC 6455 WebSocket (text messages only) over QTcpSocket.
//
// Written in-house instead of using QtWebSockets so that Mixxx and QLC+ need no new
// dependency, and so the message size limit can be checked from the frame header before
// any payload is buffered (§15.7).
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <functional>

class QTcpServer;
class QTcpSocket;

namespace vertexav {

class WsConnection : public QObject {
    Q_OBJECT
  public:
    enum class Mode { Server, Client };
    enum class State { Handshake, Open, Closing, Closed };
    // Returns true and fills body/contentType to answer a plain HTTP GET (e.g. artwork).
    using HttpHandler = std::function<bool(const QString& path, QByteArray* body, QByteArray* contentType)>;

    // Server side: wraps an accepted socket (takes ownership).
    WsConnection(QTcpSocket* socket, Mode mode, QObject* parent = nullptr);
    ~WsConnection() override;

    // Client side: connects and performs the opening handshake.
    static WsConnection* connectTo(const QString& host, quint16 port, const QString& path,
            QObject* parent = nullptr);

    void sendText(const QByteArray& utf8);
    void close(quint16 code = 1000, const QByteArray& reason = QByteArray());
    void abort();

    void setMaxMessageSize(qint64 bytes) { m_maxMessage = bytes; }
    qint64 maxMessageSize() const { return m_maxMessage; }
    void setExpectedPath(const QString& path) { m_expectedPath = path; }
    void setHttpHandler(HttpHandler h) { m_http = std::move(h); }

    State state() const { return m_state; }
    bool isOpen() const { return m_state == State::Open; }
    QHostAddress peerAddress() const;
    quint16 peerPort() const;
    QString requestPath() const { return m_path; }
    qint64 bytesToWrite() const;

  signals:
    void opened();
    void textReceived(const QByteArray& utf8);
    void messageTooLarge();
    void disconnected();

  private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();

  private:
    bool handleServerHandshake();
    bool handleClientHandshake();
    void processFrames();
    void sendFrame(quint8 opcode, const QByteArray& payload);
    void failHttp(int status, const QByteArray& reason);
    void finish();

    QPointer<QTcpSocket> m_socket;
    Mode m_mode;
    State m_state = State::Handshake;
    QByteArray m_buffer;
    QByteArray m_message;
    bool m_inMessage = false;
    qint64 m_maxMessage = 1024 * 1024;
    QString m_expectedPath;
    QString m_path;
    QByteArray m_clientKey;
    QString m_host;
    quint16 m_port = 0;
    HttpHandler m_http;
    bool m_finished = false;
};

class WsServer : public QObject {
    Q_OBJECT
  public:
    explicit WsServer(QObject* parent = nullptr);
    bool listen(const QHostAddress& address, quint16 port);
    quint16 serverPort() const;
    QString errorString() const;
    void close();

  signals:
    // Emitted right after the TCP accept; the WebSocket handshake is still pending.
    void newConnection(vertexav::WsConnection* connection);

  private:
    QTcpServer* m_server;
};

} // namespace vertexav
