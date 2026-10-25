#include "vertexav/websocket.h"

#include <QCryptographicHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>

#include "vertexav/security.h"

namespace vertexav {

namespace {
const QByteArray kGuid("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
constexpr int kMaxHandshake = 8 * 1024;

QByteArray acceptKey(const QByteArray& key) {
    return QCryptographicHash::hash(key + kGuid, QCryptographicHash::Sha1).toBase64();
}

// Parses "Name: value" lines into lower-case names.
QHash<QByteArray, QByteArray> parseHeaders(const QList<QByteArray>& lines) {
    QHash<QByteArray, QByteArray> h;
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines[i].indexOf(':');
        if (colon > 0) {
            h.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
    }
    return h;
}
} // namespace

WsConnection::WsConnection(QTcpSocket* socket, Mode mode, QObject* parent)
        : QObject(parent), m_socket(socket), m_mode(mode) {
    socket->setParent(this);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(socket, &QTcpSocket::readyRead, this, &WsConnection::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &WsConnection::onDisconnected);
    connect(socket, &QTcpSocket::connected, this, &WsConnection::onConnected);
    connect(socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_socket && m_socket->state() != QAbstractSocket::ConnectedState) {
            finish();
        }
    });
}

WsConnection::~WsConnection() = default;

WsConnection* WsConnection::connectTo(const QString& host, quint16 port, const QString& path, QObject* parent) {
    auto* socket = new QTcpSocket();
    auto* c = new WsConnection(socket, Mode::Client, parent);
    c->m_host = host;
    c->m_port = port;
    c->m_path = path;
    socket->connectToHost(host, port);
    return c;
}

QHostAddress WsConnection::peerAddress() const {
    return m_socket ? m_socket->peerAddress() : QHostAddress();
}

quint16 WsConnection::peerPort() const {
    return m_socket ? m_socket->peerPort() : 0;
}

qint64 WsConnection::bytesToWrite() const {
    return m_socket ? m_socket->bytesToWrite() : 0;
}

void WsConnection::onConnected() {
    if (m_mode != Mode::Client) {
        return;
    }
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_clientKey = security::randomBytes(16).toBase64();
    QByteArray req;
    req += "GET " + m_path.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: " + m_host.toUtf8() + ":" + QByteArray::number(m_port) + "\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + m_clientKey + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    m_socket->write(req);
}

void WsConnection::onReadyRead() {
    if (!m_socket) {
        return;
    }
    m_buffer += m_socket->readAll();
    if (m_state == State::Handshake) {
        const bool done = m_mode == Mode::Server ? handleServerHandshake() : handleClientHandshake();
        if (!done) {
            return;
        }
    }
    processFrames();
}

void WsConnection::failHttp(int status, const QByteArray& reason) {
    if (m_socket) {
        m_socket->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason +
                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        m_socket->disconnectFromHost();
    }
    m_state = State::Closing;
}

bool WsConnection::handleServerHandshake() {
    const int end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_buffer.size() > kMaxHandshake) {
            abort();
        }
        return false;
    }
    const QList<QByteArray> lines = m_buffer.left(end).split('\n');
    m_buffer.remove(0, end + 4);
    QList<QByteArray> cleaned;
    for (const auto& l : lines) {
        cleaned.append(l.trimmed());
    }
    const QList<QByteArray> requestLine = cleaned.value(0).split(' ');
    if (requestLine.size() < 3 || requestLine[0] != "GET") {
        failHttp(400, "Bad Request");
        return false;
    }
    m_path = QString::fromUtf8(requestLine[1]);
    const QString pathOnly = m_path.section(QLatin1Char('?'), 0, 0);
    const auto headers = parseHeaders(cleaned);
    const bool upgrade = headers.value("upgrade").toLower() == "websocket" &&
            headers.value("connection").toLower().contains("upgrade");
    if (!upgrade) {
        QByteArray body, type;
        if (m_http && m_http(pathOnly, &body, &type)) {
            m_socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            m_socket->disconnectFromHost();
            m_state = State::Closing;
            return false;
        }
        failHttp(404, "Not Found");
        return false;
    }
    if (!m_expectedPath.isEmpty() && pathOnly != m_expectedPath) {
        failHttp(404, "Not Found");
        return false;
    }
    const QByteArray key = headers.value("sec-websocket-key");
    if (key.isEmpty() || headers.value("sec-websocket-version") != "13") {
        failHttp(400, "Bad Request");
        return false;
    }
    m_socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " +
            acceptKey(key) + "\r\n\r\n");
    m_state = State::Open;
    emit opened();
    return true;
}

bool WsConnection::handleClientHandshake() {
    const int end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_buffer.size() > kMaxHandshake) {
            abort();
        }
        return false;
    }
    const QList<QByteArray> lines = m_buffer.left(end).split('\n');
    m_buffer.remove(0, end + 4);
    QList<QByteArray> cleaned;
    for (const auto& l : lines) {
        cleaned.append(l.trimmed());
    }
    const auto headers = parseHeaders(cleaned);
    if (!cleaned.value(0).contains(" 101") || headers.value("sec-websocket-accept") != acceptKey(m_clientKey)) {
        abort();
        return false;
    }
    m_state = State::Open;
    emit opened();
    return true;
}

void WsConnection::processFrames() {
    while (m_state == State::Open || m_state == State::Closing) {
        if (m_buffer.size() < 2) {
            return;
        }
        const quint8 b0 = static_cast<quint8>(m_buffer[0]);
        const quint8 b1 = static_cast<quint8>(m_buffer[1]);
        const bool fin = b0 & 0x80;
        const quint8 opcode = b0 & 0x0F;
        const bool masked = b1 & 0x80;
        quint64 len = b1 & 0x7F;
        int pos = 2;
        if (len == 126) {
            if (m_buffer.size() < 4) {
                return;
            }
            len = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(m_buffer.constData() + 2));
            pos = 4;
        } else if (len == 127) {
            if (m_buffer.size() < 10) {
                return;
            }
            len = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(m_buffer.constData() + 2));
            pos = 10;
        }
        if ((b0 & 0x70) || (m_mode == Mode::Server && !masked) || (m_mode == Mode::Client && masked)) {
            close(1002, "protocol error");
            return;
        }
        const bool control = opcode & 0x08;
        if (control && (len > 125 || !fin)) {
            close(1002, "bad control frame");
            return;
        }
        // Size limit from the header, before buffering the payload (§15.7).
        if (!control && static_cast<quint64>(m_message.size()) + len > static_cast<quint64>(m_maxMessage)) {
            emit messageTooLarge();
            if (m_state == State::Open) {
                close(1009, "message too large");
            }
            m_buffer.clear();
            return;
        }
        const int maskPos = pos;
        if (masked) {
            pos += 4;
        }
        if (static_cast<quint64>(m_buffer.size()) < pos + len) {
            return;
        }
        QByteArray payload = m_buffer.mid(pos, static_cast<int>(len));
        if (masked) {
            const char* mask = m_buffer.constData() + maskPos;
            for (int i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            }
        }
        m_buffer.remove(0, pos + static_cast<int>(len));
        switch (opcode) {
        case 0x0: // continuation
            if (!m_inMessage) {
                close(1002, "unexpected continuation");
                return;
            }
            m_message += payload;
            break;
        case 0x1: // text
        case 0x2: // binary (treated as text: the protocol only uses text)
            if (m_inMessage) {
                close(1002, "interleaved message");
                return;
            }
            m_message = payload;
            m_inMessage = true;
            break;
        case 0x8: { // close
            if (m_state == State::Open) {
                quint16 code = 1000;
                if (payload.size() >= 2) {
                    code = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(payload.constData()));
                }
                QByteArray echo(2, 0);
                qToBigEndian<quint16>(code, reinterpret_cast<uchar*>(echo.data()));
                sendFrame(0x8, echo);
            }
            m_state = State::Closing;
            if (m_socket) {
                m_socket->disconnectFromHost();
            }
            return;
        }
        case 0x9: // ping
            sendFrame(0xA, payload);
            continue;
        case 0xA: // pong
            continue;
        default:
            close(1002, "unknown opcode");
            return;
        }
        if (fin && m_inMessage) {
            m_inMessage = false;
            const QByteArray msg = m_message;
            m_message.clear();
            if (m_state == State::Open) {
                emit textReceived(msg);
            }
        }
    }
}

void WsConnection::sendFrame(quint8 opcode, const QByteArray& payload) {
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    QByteArray frame;
    frame.reserve(payload.size() + 14);
    frame.append(static_cast<char>(0x80 | opcode));
    const quint8 maskBit = m_mode == Mode::Client ? 0x80 : 0;
    const qint64 n = payload.size();
    if (n < 126) {
        frame.append(static_cast<char>(maskBit | n));
    } else if (n < 65536) {
        frame.append(static_cast<char>(maskBit | 126));
        QByteArray len(2, 0);
        qToBigEndian<quint16>(static_cast<quint16>(n), reinterpret_cast<uchar*>(len.data()));
        frame.append(len);
    } else {
        frame.append(static_cast<char>(maskBit | 127));
        QByteArray len(8, 0);
        qToBigEndian<quint64>(static_cast<quint64>(n), reinterpret_cast<uchar*>(len.data()));
        frame.append(len);
    }
    if (m_mode == Mode::Client) {
        const QByteArray mask = security::randomBytes(4);
        frame.append(mask);
        QByteArray masked = payload;
        for (int i = 0; i < masked.size(); ++i) {
            masked[i] = static_cast<char>(masked[i] ^ mask[i % 4]);
        }
        frame.append(masked);
    } else {
        frame.append(payload);
    }
    m_socket->write(frame);
}

void WsConnection::sendText(const QByteArray& utf8) {
    if (m_state == State::Open) {
        sendFrame(0x1, utf8);
    }
}

void WsConnection::close(quint16 code, const QByteArray& reason) {
    if (m_state == State::Open) {
        QByteArray payload(2, 0);
        qToBigEndian<quint16>(code, reinterpret_cast<uchar*>(payload.data()));
        payload += reason.left(120);
        sendFrame(0x8, payload);
        m_state = State::Closing;
        if (m_socket) {
            m_socket->flush();
            m_socket->disconnectFromHost();
        }
    } else if (m_state == State::Handshake) {
        abort();
    }
}

void WsConnection::abort() {
    if (m_socket) {
        m_socket->abort();
    }
    finish();
}

void WsConnection::onDisconnected() {
    finish();
}

void WsConnection::finish() {
    m_state = State::Closed;
    if (m_finished) {
        return;
    }
    m_finished = true;
    emit disconnected();
}

WsServer::WsServer(QObject* parent)
        : QObject(parent), m_server(new QTcpServer(this)) {
    connect(m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* s = m_server->nextPendingConnection()) {
            auto* c = new WsConnection(s, WsConnection::Mode::Server, this);
            emit newConnection(c);
        }
    });
}

bool WsServer::listen(const QHostAddress& address, quint16 port) {
    return m_server->listen(address, port);
}

quint16 WsServer::serverPort() const {
    return m_server->serverPort();
}

QString WsServer::errorString() const {
    return m_server->errorString();
}

void WsServer::close() {
    m_server->close();
}

} // namespace vertexav
