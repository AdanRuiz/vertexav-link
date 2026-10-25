#include "vertexav/receiver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QUdpSocket>
#include <cmath>

#include "vertexav/security.h"
#include "vertexav/websocket.h"

namespace vertexav {

namespace {
QByteArray dumps(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void deepMerge(QJsonObject& dst, const QJsonObject& src) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it.key() == QLatin1String("type") || it.key() == QLatin1String("seq")) {
            continue;
        }
        if (it.value().isObject() && dst.value(it.key()).isObject()) {
            QJsonObject child = dst.value(it.key()).toObject();
            deepMerge(child, it.value().toObject());
            dst[it.key()] = child;
        } else {
            dst[it.key()] = it.value();
        }
    }
}

qint64 asInt(const QJsonValue& v, qint64 def = -1) {
    return v.isDouble() ? static_cast<qint64>(v.toDouble()) : def;
}

QString majorOf(const QString& v) {
    return v.section(QLatin1Char('.'), 0, 0);
}
} // namespace

Receiver::Receiver(const ReceiverConfig& config, QObject* parent)
        : QObject(parent), m_cfg(config) {
    if (m_cfg.clientId.isEmpty()) {
        m_cfg.clientId = QStringLiteral("rx-") + QString::fromLatin1(security::randomBytes(4).toHex());
    }
    m_pingTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pingTimer, &QTimer::timeout, this, &Receiver::sendPing);
    m_heartbeatTimer.setInterval(kHeartbeatIntervalMs);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, [this] {
        QJsonObject hb;
        hb[QStringLiteral("type")] = QStringLiteral("heartbeat");
        hb[QStringLiteral("t")] = QJsonValue(static_cast<qint64>(localNow()));
        send(hb);
        if (localNow() - m_lastRx > kHeartbeatTimeoutMs * 1000LL) {
            teardown(QStringLiteral("heartbeat timeout"));
        }
    });
    m_beatTimer.setSingleShot(true);
    m_beatTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_beatTimer, &QTimer::timeout, this, &Receiver::onBeatTimer);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &Receiver::connectNow);
}

Receiver::~Receiver() {
    stop();
}

void Receiver::setEndpoint(const QString& host, quint16 port) {
    m_cfg.host = host;
    m_cfg.port = port;
}

void Receiver::start() {
    if (m_running) {
        return;
    }
    m_running = true;
    if (m_cfg.udp && !m_udp) {
        m_udp = new QUdpSocket(this);
        if (!m_udp->bind(QHostAddress(QHostAddress::AnyIPv4), quint16(0))) {
            emit statusMessage(QStringLiteral("cannot open UDP socket: %1").arg(m_udp->errorString()));
            delete m_udp;
            m_udp = nullptr;
        } else {
            connect(m_udp, &QUdpSocket::readyRead, this, &Receiver::onDatagrams);
        }
    }
    connectNow();
}

void Receiver::stop() {
    if (!m_running) {
        return;
    }
    m_running = false;
    m_reconnectTimer.stop();
    if (m_ws && m_open) {
        QJsonObject bye;
        bye[QStringLiteral("type")] = QStringLiteral("bye");
        bye[QStringLiteral("reason")] = QStringLiteral("shutdown");
        m_ws->sendText(dumps(bye));
        m_ws->close(1000);
    }
    teardown(QStringLiteral("stopped"));
}

void Receiver::setLatencyUs(qint64 latencyUs) {
    m_cfg.latencyUs = latencyUs;
    QJsonObject m;
    m[QStringLiteral("type")] = QStringLiteral("latency.update");
    m[QStringLiteral("latency_us")] = QJsonValue(static_cast<qint64>(latencyUs));
    send(m);
    armBeat();
}

void Receiver::send(const QJsonObject& message) {
    if (m_ws && m_open) {
        m_ws->sendText(dumps(message));
    }
}

void Receiver::connectNow() {
    if (!m_running) {
        return;
    }
    m_gotWelcome = false;
    m_subscribed = false;
    m_ws = WsConnection::connectTo(m_cfg.host, m_cfg.port, QString::fromLatin1(kWsPath), this);
    m_ws->setMaxMessageSize(kMaxMsgEmitterToReceiver);
    connect(m_ws, &WsConnection::opened, this, &Receiver::onOpened);
    connect(m_ws, &WsConnection::textReceived, this, &Receiver::onText);
    connect(m_ws, &WsConnection::disconnected, this, &Receiver::onWsDisconnected, Qt::QueuedConnection);
    connect(m_ws, &WsConnection::messageTooLarge, this, [this] {
        fatal(QStringLiteral("message_too_large"), QStringLiteral("Message over 512 KB"));
    });
    // Handshake + welcome must complete in a few seconds.
    QTimer::singleShot(6000, m_ws, [this, ws = m_ws] {
        if (ws == m_ws && !m_gotWelcome) {
            ws->abort();
        }
    });
}

void Receiver::onOpened() {
    m_open = true;
    m_lastRx = localNow();
    QJsonObject hello;
    hello[QStringLiteral("type")] = QStringLiteral("hello");
    hello[QStringLiteral("protocol")] = QString::fromLatin1(kProtocol);
    hello[QStringLiteral("version")] = QString::fromLatin1(kVersion);
    hello[QStringLiteral("role")] = m_cfg.role;
    hello[QStringLiteral("name")] = m_cfg.name;
    hello[QStringLiteral("client_id")] = m_cfg.clientId;
    QJsonArray profiles;
    for (int p : std::as_const(m_cfg.profiles)) {
        profiles.append(p);
    }
    hello[QStringLiteral("profiles")] = profiles;
    hello[QStringLiteral("latency_us")] = QJsonValue(static_cast<qint64>(m_cfg.latencyUs));
    hello[QStringLiteral("realtime_over_control")] = false;
    if (m_udp) {
        hello[QStringLiteral("udp_port")] = m_udp->localPort();
    }
    m_ws->sendText(dumps(hello));
}

void Receiver::fatal(const QString& code, const QString& message) {
    if (m_ws && m_open) {
        QJsonObject err;
        err[QStringLiteral("type")] = QStringLiteral("error");
        err[QStringLiteral("code")] = code;
        err[QStringLiteral("message")] = message;
        err[QStringLiteral("fatal")] = true;
        m_ws->sendText(dumps(err));
        m_ws->close(1008);
    }
    emit statusMessage(QStringLiteral("session ended: %1: %2").arg(code, message));
    m_running = false; // fatal errors are not retried automatically
    teardown(code);
}

void Receiver::onText(const QByteArray& text) {
    if (jsonDepthExceeds(text)) {
        fatal(QStringLiteral("message_too_large"), QStringLiteral("JSON nesting too deep"));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(text);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject msg = doc.object();
    const QString type = msg.value(QStringLiteral("type")).toString();
    m_lastRx = localNow();
    emit messageReceived(msg);
    if (!m_gotWelcome) {
        if (type == QLatin1String("auth.challenge")) {
            if (m_cfg.psk.isEmpty()) {
                fatal(QStringLiteral("auth_required"), QStringLiteral("The emitter requires a pairing key"));
                return;
            }
            m_serverNonce = QByteArray::fromBase64(msg.value(QStringLiteral("server_nonce")).toString().toLatin1());
            m_clientNonce = security::randomBytes(security::kNonceLen);
            QJsonObject resp;
            resp[QStringLiteral("type")] = QStringLiteral("auth.response");
            resp[QStringLiteral("client_nonce")] = QString::fromLatin1(m_clientNonce.toBase64());
            resp[QStringLiteral("proof")] = QString::fromLatin1(
                    security::clientProof(m_cfg.psk, m_serverNonce, m_clientNonce, m_cfg.clientId).toBase64());
            m_ws->sendText(dumps(resp));
        } else if (type == QLatin1String("error")) {
            fatal(msg.value(QStringLiteral("code")).toString(), msg.value(QStringLiteral("message")).toString());
        } else if (type == QLatin1String("welcome")) {
            handleWelcome(msg);
        }
        return;
    }
    if (type == QLatin1String("error") && msg.value(QStringLiteral("fatal")).toBool()) {
        emit statusMessage(QStringLiteral("emitter error: %1").arg(msg.value(QStringLiteral("code")).toString()));
        teardown(msg.value(QStringLiteral("code")).toString());
        return;
    }
    if (type == QLatin1String("bye")) {
        teardown(QStringLiteral("bye"));
        return;
    }
    apply(msg);
}

void Receiver::handleWelcome(const QJsonObject& msg) {
    if (majorOf(msg.value(QStringLiteral("version")).toString()) != majorOf(QString::fromLatin1(kVersion))) {
        fatal(QStringLiteral("unsupported_version"), QStringLiteral("Unsupported version"));
        return;
    }
    const QJsonObject em = msg.value(QStringLiteral("emitter")).toObject();
    const QString emId = em.value(QStringLiteral("id")).toString();
    const QString mode = msg.value(QStringLiteral("security")).toString();
    if (!m_cfg.psk.isEmpty() && mode == QLatin1String("open")) {
        fatal(QStringLiteral("security_downgrade"), QStringLiteral("Paired emitter offers open mode"));
        return;
    }
    const QString prev = m_securitySeen.value(emId);
    if ((prev == QLatin1String("auth") || prev == QLatin1String("tls")) && mode == QLatin1String("open")) {
        fatal(QStringLiteral("security_downgrade"), QStringLiteral("Weaker security mode than before"));
        return;
    }
    if (mode == QLatin1String("auth") || mode == QLatin1String("tls")) {
        if (m_serverNonce.isEmpty()) {
            fatal(QStringLiteral("auth_failed"), QStringLiteral("welcome in auth mode without challenge"));
            return;
        }
        const QByteArray expected = security::serverProof(m_cfg.psk, m_clientNonce, m_serverNonce,
                msg.value(QStringLiteral("session_id")).toString());
        const QByteArray got = QByteArray::fromBase64(msg.value(QStringLiteral("server_proof")).toString().toLatin1());
        if (!security::constantTimeEquals(expected, got)) {
            fatal(QStringLiteral("auth_failed"), QStringLiteral("Wrong server_proof: emitter not authentic"));
            return;
        }
        m_macKey = security::macKey(m_cfg.psk, m_serverNonce, m_clientNonce);
    } else {
        m_macKey.clear();
    }
    m_securitySeen.insert(emId, mode);
    if (!m_cfg.ownId.isEmpty()) {
        for (const QJsonValue& v : em.value(QStringLiteral("path")).toArray()) {
            if (v.toString() == m_cfg.ownId) {
                fatal(QStringLiteral("loop_detected"), QStringLiteral("Our id is in emitter.path"));
                return;
            }
        }
    }
    const quint32 key = static_cast<quint32>(asInt(msg.value(QStringLiteral("session_key")), 0));
    if (!m_sessionKey || *m_sessionKey != key) {
        // New emitter session: forget sequences, clock and tempo (§8.3).
        m_windows.clear();
        m_sync = ClockSync();
        m_follower.reset();
        m_tempo.reset();
    }
    m_sessionKey = key;
    m_welcome = msg;
    m_gotWelcome = true;
    m_backoff = 0.5;
    m_lastT.clear();
    emit statusMessage(QStringLiteral("connected to %1 (%2), security %3")
                               .arg(em.value(QStringLiteral("name")).toString(), emId, mode));
    m_heartbeatTimer.start();
    m_initialPings = 0;
    m_pingTimer.start(30);
    if (!m_udp) {
        subscribe();
    }
    emit connected(msg);
}

void Receiver::subscribe() {
    if (m_subscribed) {
        return;
    }
    m_subscribed = true;
    QStringList topics = m_cfg.topics;
    if (topics.isEmpty()) {
        QSet<int> agreed;
        for (const QJsonValue& v : m_welcome.value(QStringLiteral("profiles")).toArray()) {
            agreed.insert(v.toInt());
        }
        const QList<QPair<QString, int>> all{{QStringLiteral("tempo"), 0}, {QStringLiteral("deck"), 1},
                {QStringLiteral("mix"), 1}, {QStringLiteral("track"), 1}, {QStringLiteral("event"), 1},
                {QStringLiteral("intent"), 1}, {QStringLiteral("trigger"), 1}, {QStringLiteral("structure"), 2}};
        for (const auto& t : all) {
            if (agreed.contains(t.second)) {
                topics.append(t.first);
            }
        }
    }
    QJsonObject sub;
    sub[QStringLiteral("type")] = QStringLiteral("subscribe");
    sub[QStringLiteral("topics")] = QJsonArray::fromStringList(topics);
    send(sub);
}

void Receiver::sendPing() {
    if (!m_sessionKey || !m_ws || !m_open) {
        return;
    }
    // 16 exchanges 30 ms apart at connection (up to ~1.4 s more if UDP is lossy), then every 1.5 s.
    ++m_initialPings;
    if (!m_subscribed && m_initialPings >= 16 && (m_sync.synced() || m_initialPings >= 64)) {
        if (!m_sync.synced()) {
            emit statusMessage(QStringLiteral("no clock.pong over UDP (firewall?)"));
        }
        subscribe();
    }
    if (m_subscribed && m_pingTimer.interval() != 1500) {
        m_pingTimer.setInterval(1500);
    }
    m_pingId += 1;
    const qint64 t0 = localNow();
    m_pings.insert(m_pingId, t0);
    if (m_pings.size() > 64) {
        m_pings.remove(m_pingId - 64);
    }
    if (m_udp) {
        PingPacket p;
        p.t0 = t0;
        p.pingId = m_pingId;
        const quint16 port = static_cast<quint16>(m_welcome.value(QStringLiteral("udp_port")).toInt(kRealtimePort));
        QHostAddress addr(m_cfg.host);
        if (addr.isNull()) {
            addr = m_ws->peerAddress();
        }
        m_udp->writeDatagram(packPacket(Kind::ClockPing, ++m_pingSeq, *m_sessionKey, 0, p.packBody(), m_macKey),
                addr, port);
    }
}

std::optional<qint64> Receiver::emitterNow() {
    if (!m_sync.hasOffset()) {
        return std::nullopt;
    }
    return m_sync.toEmitter(localNow());
}

void Receiver::onDatagrams() {
    while (m_udp && m_udp->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_udp->receiveDatagram(kUdpRecvBuffer);
        const qint64 t3 = localNow();
        if (!m_sessionKey) {
            continue;
        }
        const Validation v = validateDatagram(dg.data(), *m_sessionKey, m_macKey, !m_macKey.isEmpty());
        if (!v.ok) {
            ++m_packetErrors;
            continue;
        }
        if (m_sync.hasOffset() && std::llabs(v.header.t - m_sync.toEmitter(t3)) > 2000000) {
            ++m_packetErrors; // step 6
            continue;
        }
        if (v.header.kind != static_cast<quint8>(Kind::Audio) && !m_windows[v.header.kind].accept(v.header.seq)) {
            continue; // step 7
        }
        if (v.header.kind == static_cast<quint8>(Kind::ClockPong)) {
            PongPacket p;
            if (PongPacket::unpackBody(v.body, &p)) {
                const auto it = m_pings.find(p.pingId);
                if (it != m_pings.end() && it.value() == p.t0) {
                    m_pings.erase(it);
                    m_sync.add(p.t0, p.t1, p.t2, t3);
                }
            }
        } else if (v.header.kind == static_cast<quint8>(Kind::Tempo) && m_sync.hasOffset()) {
            TempoPacket p;
            if (!TempoPacket::unpackBody(v.body, &p)) {
                ++m_packetErrors;
                continue;
            }
            Tempo t = m_tempo.value_or(Tempo());
            t.bpm = p.bpm;
            t.anchorT = p.anchorT;
            t.anchorBeat = p.anchorBeat;
            t.beatsPerBar = std::max<int>(1, p.beatsPerBar);
            t.sourceDeck = p.sourceDeck;
            t.playing = p.playing;
            t.confidence = p.confidence;
            t.stability = p.stability;
            t.phaseReset = p.phaseReset;
            // bar_offset is not in the binary packet: derive it from next_downbeat_t.
            const double nd = t.beatAt(p.nextDownbeatT);
            t.barOffset = static_cast<double>(std::llround(nd) % t.beatsPerBar);
            const auto r = m_follower.update(t, m_sync.toEmitter(t3));
            if (r != TempoFollower::Result::Same) {
                emit tempoChanged();
                armBeat();
            }
        }
    }
}

bool Receiver::fresh(const QString& key, const QJsonValue& t) {
    const qint64 v = asInt(t);
    if (v < 0) {
        return true;
    }
    const auto it = m_lastT.constFind(key);
    if (it != m_lastT.cend() && v < it.value()) {
        return false; // older delta (§8.2)
    }
    m_lastT.insert(key, v);
    return true;
}

void Receiver::applyTempo(const QJsonObject& msg, bool fromSnapshot) {
    Q_UNUSED(fromSnapshot);
    Tempo t = Tempo::fromJson(msg, m_tempo.value_or(Tempo()));
    m_tempo = t;
    m_tempo->phaseReset = false;
    const auto now = emitterNow();
    m_follower.update(t, now ? *now : t.anchorT);
    emit tempoChanged();
    armBeat();
}

void Receiver::apply(const QJsonObject& msg) {
    const QString type = msg.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("snapshot")) {
        if (msg.value(QStringLiteral("tempo")).isObject()) {
            applyTempo(msg.value(QStringLiteral("tempo")).toObject(), true);
        }
        const qint64 t = asInt(msg.value(QStringLiteral("t")));
        m_decks.clear();
        for (const QJsonValue& d : msg.value(QStringLiteral("decks")).toArray()) {
            const int n = d.toObject().value(QStringLiteral("deck")).toInt();
            m_decks.insert(n, d.toObject());
            m_posT.insert(n, t);
        }
        m_mix = msg.value(QStringLiteral("mix")).toObject();
        m_tracks.clear();
        for (const QJsonValue& d : msg.value(QStringLiteral("tracks")).toArray()) {
            m_tracks.insert(d.toObject().value(QStringLiteral("deck")).toInt(), d.toObject());
        }
        m_structures.clear();
        for (const QJsonValue& d : msg.value(QStringLiteral("structures")).toArray()) {
            m_structures.insert(d.toObject().value(QStringLiteral("track_id")).toString(), d.toObject());
        }
        m_intent = msg.value(QStringLiteral("intent")).toObject();
        for (const QJsonValue& e : msg.value(QStringLiteral("events")).toArray()) {
            QJsonObject ev = e.toObject();
            ev[QStringLiteral("type")] = QStringLiteral("event");
            apply(ev);
        }
        emit snapshotReceived();
        emit mixChanged();
        emit intentChanged(m_intent);
        for (int d : m_decks.keys()) {
            emit deckChanged(d);
        }
        if (const auto sec = currentSection()) {
            emit sectionChanged(onAirDeck(), sec->role, sec->energy);
        }
    } else if (type == QLatin1String("tempo")) {
        if (fresh(QStringLiteral("tempo"), msg.value(QStringLiteral("t")))) {
            applyTempo(msg, false);
        }
    } else if (type == QLatin1String("deck.state")) {
        const int deck = msg.value(QStringLiteral("deck")).toInt();
        if (fresh(QStringLiteral("deck:%1").arg(deck), msg.value(QStringLiteral("t")))) {
            QJsonObject& st = m_decks[deck];
            deepMerge(st, msg);
            if (msg.contains(QStringLiteral("position_beat")) || msg.contains(QStringLiteral("position_s"))) {
                m_posT.insert(deck, asInt(msg.value(QStringLiteral("t"))));
            }
            emit deckChanged(deck);
        }
    } else if (type == QLatin1String("mix.state")) {
        if (fresh(QStringLiteral("mix"), msg.value(QStringLiteral("t")))) {
            deepMerge(m_mix, msg);
            emit mixChanged();
        }
    } else if (type == QLatin1String("intent")) {
        if (fresh(QStringLiteral("intent"), msg.value(QStringLiteral("t")))) {
            deepMerge(m_intent, msg);
            emit intentChanged(m_intent);
        }
    } else if (type == QLatin1String("track.loaded")) {
        const int deck = msg.value(QStringLiteral("deck")).toInt();
        m_tracks.insert(deck, msg);
        emit trackLoaded(deck);
    } else if (type == QLatin1String("track.unloaded")) {
        m_tracks.remove(msg.value(QStringLiteral("deck")).toInt());
    } else if (type == QLatin1String("track.structure")) {
        m_structures.insert(msg.value(QStringLiteral("track_id")).toString(), msg);
    } else if (type == QLatin1String("event")) {
        schedule(msg.value(QStringLiteral("event_id")).toString(), asInt(msg.value(QStringLiteral("at"))), msg, true);
    } else if (type == QLatin1String("event.cancel")) {
        const QString id = msg.value(QStringLiteral("event_id")).toString();
        if (QTimer* t = m_scheduled.take(id)) {
            t->stop();
            t->deleteLater();
        }
    } else if (type == QLatin1String("trigger")) {
        schedule(QStringLiteral("trigger-%1").arg(asInt(msg.value(QStringLiteral("seq")))),
                asInt(msg.value(QStringLiteral("at"))), msg, false);
    }
}

void Receiver::schedule(const QString& key, qint64 at, const QJsonObject& msg, bool isEvent) {
    auto fire = [this, key, msg, isEvent] {
        if (QTimer* t = m_scheduled.take(key)) {
            t->deleteLater();
        }
        if (!isEvent) {
            emit triggerFired(msg);
            return;
        }
        emit eventFired(msg);
        if (msg.value(QStringLiteral("name")).toString() == QLatin1String("section.change")) {
            const int deck = msg.value(QStringLiteral("deck")).toInt();
            const QJsonObject data = msg.value(QStringLiteral("data")).toObject();
            if (onAirDeck() == 0 || deck == onAirDeck()) {
                emit sectionChanged(deck, data.value(QStringLiteral("to")).toString(),
                        data.value(QStringLiteral("energy")).toDouble());
            }
        }
    };
    qint64 delayMs = 0;
    if (at >= 0 && m_sync.hasOffset()) {
        const qint64 localTarget = m_sync.toLocal(at) - m_cfg.latencyUs;
        delayMs = std::max<qint64>(0, (localTarget - localNow()) / 1000);
    }
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, fire);
    if (QTimer* old = m_scheduled.take(key)) {
        old->deleteLater();
    }
    m_scheduled.insert(key, timer);
    timer->start(static_cast<int>(delayMs));
}

void Receiver::armBeat() {
    m_beatTimer.stop();
    m_armedBeat = -1;
    const auto now = emitterNow();
    const qint64 lat = m_cfg.latencyUs;
    if (!now || !m_follower.hasModel() || !m_follower.beatsEnabled(*now + lat)) {
        m_lastBeat = -1;
        m_beatTimer.start(20); // check again soon
        return;
    }
    const double b = m_follower.beatAt(*now + lat);
    const qint64 idx = static_cast<qint64>(std::floor(b + 1e-6)) + 1;
    const qint64 targetEm = m_follower.timeOfBeat(idx, *now);
    const qint64 waitUs = targetEm - lat - *now;
    m_armedBeat = idx;
    // Sleep coarse first and re-evaluate (tempo may change), then the exact wait.
    m_beatTimer.start(static_cast<int>(waitUs > 30000 ? std::min<qint64>((waitUs - 20000) / 1000, 100)
                                                      : std::max<qint64>(0, waitUs / 1000)));
}

void Receiver::onBeatTimer() {
    const auto now = emitterNow();
    const qint64 lat = m_cfg.latencyUs;
    if (m_armedBeat < 0 || !now || !m_follower.beatsEnabled(*now + lat)) {
        armBeat();
        return;
    }
    const qint64 targetEm = m_follower.timeOfBeat(m_armedBeat, *now);
    const qint64 waitUs = targetEm - lat - *now;
    if (waitUs > 1500) {
        armBeat();
        return;
    }
    const double b = m_follower.beatAt(*now + lat);
    if (std::abs(b - m_armedBeat) <= 0.25) {
        const bool change = m_lastBeat < 0 || m_armedBeat != m_lastBeat + 1 || m_follower.jumps() != m_lastJumps;
        m_lastJumps = m_follower.jumps();
        m_lastBeat = m_armedBeat;
        emit beat(m_armedBeat, m_follower.model().beatInBar(m_armedBeat), m_follower.model().bpm, change);
    }
    // Next beat: start the search a little after this one.
    m_beatTimer.stop();
    m_armedBeat = -1;
    QTimer::singleShot(30, this, &Receiver::armBeat);
}

void Receiver::onWsDisconnected() {
    teardown(QStringLiteral("connection lost"));
}

void Receiver::teardown(const QString& reason) {
    const bool wasOpen = m_gotWelcome;
    m_open = false;
    m_gotWelcome = false;
    m_pingTimer.stop();
    m_heartbeatTimer.stop();
    m_beatTimer.stop();
    for (QTimer* t : std::as_const(m_scheduled)) {
        t->stop();
        t->deleteLater();
    }
    m_scheduled.clear();
    if (m_ws) {
        WsConnection* ws = m_ws;
        m_ws = nullptr;
        ws->disconnect(this);
        ws->abort();
        ws->deleteLater();
    }
    if (wasOpen) {
        emit disconnected(reason);
    }
    if (m_running && m_cfg.reconnect && !m_reconnectTimer.isActive()) {
        // 0.5 s, 1 s, 2 s... up to 5 s, each ± 25 % (§6.5)
        const double jitter = 0.75 + QRandomGenerator::global()->bounded(0.5);
        m_reconnectTimer.start(static_cast<int>(m_backoff * jitter * 1000));
        m_backoff = std::min(m_backoff * 2, 5.0);
    }
}

int Receiver::onAirDeck() const {
    return m_mix.value(QStringLiteral("on_air_deck")).toInt(0);
}

std::optional<SectionInfo> Receiver::currentSection(int deck) {
    if (deck <= 0) {
        deck = onAirDeck();
    }
    const auto st = m_decks.constFind(deck);
    const auto tr = m_tracks.constFind(deck);
    if (st == m_decks.cend() || tr == m_tracks.cend()) {
        return std::nullopt;
    }
    const QJsonObject structure = m_structures.value(tr->value(QStringLiteral("track_id")).toString());
    const auto now = emitterNow();
    if (structure.isEmpty() || !now || !st->value(QStringLiteral("position_beat")).isDouble()) {
        return std::nullopt;
    }
    double beat = st->value(QStringLiteral("position_beat")).toDouble();
    if (st->value(QStringLiteral("playing")).toBool() && m_tempo && m_posT.value(deck, -1) >= 0) {
        beat += (*now - m_posT.value(deck)) / 1e6 * m_tempo->bpm / 60.0;
    }
    const QJsonArray sections = structure.value(QStringLiteral("sections")).toArray();
    for (int i = 0; i < sections.size(); ++i) {
        const QJsonObject s = sections[i].toObject();
        const double a = s.value(QStringLiteral("start_beat")).toDouble();
        const double b = s.value(QStringLiteral("end_beat")).toDouble();
        if (a <= beat && beat < b) {
            SectionInfo info;
            info.index = i;
            info.role = s.value(QStringLiteral("role")).toString();
            info.energy = s.value(QStringLiteral("energy")).toDouble();
            info.startBeat = a;
            info.endBeat = b;
            return info;
        }
    }
    return std::nullopt;
}

} // namespace vertexav
