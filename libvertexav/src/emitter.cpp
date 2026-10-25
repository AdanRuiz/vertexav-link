#include "vertexav/emitter.h"

#include <QElapsedTimer>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QThread>
#include <QUdpSocket>
#include <cmath>

#include "vertexav/mdns.h"
#include "vertexav/security.h"
#include "vertexav/websocket.h"

namespace vertexav {

namespace {

QByteArray dumps(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QJsonValue i64(qint64 v) {
    return QJsonValue(static_cast<qint64>(v));
}

// Topic of each message type (§6.4). Empty = session message, always delivered.
QString topicOf(const QString& type) {
    static const QHash<QString, QString> map{
            {QStringLiteral("tempo"), QStringLiteral("tempo")},
            {QStringLiteral("deck.state"), QStringLiteral("deck")},
            {QStringLiteral("mix.state"), QStringLiteral("mix")},
            {QStringLiteral("track.loaded"), QStringLiteral("track")},
            {QStringLiteral("track.unloaded"), QStringLiteral("track")},
            {QStringLiteral("intent"), QStringLiteral("intent")},
            {QStringLiteral("trigger"), QStringLiteral("trigger")},
            {QStringLiteral("event"), QStringLiteral("event")},
            {QStringLiteral("event.cancel"), QStringLiteral("event")},
            {QStringLiteral("track.structure"), QStringLiteral("structure")},
    };
    return map.value(type);
}

const QHash<QString, int>& topicProfiles() {
    static const QHash<QString, int> map{
            {QStringLiteral("tempo"), 0}, {QStringLiteral("deck"), 1}, {QStringLiteral("mix"), 1},
            {QStringLiteral("track"), 1}, {QStringLiteral("event"), 1}, {QStringLiteral("intent"), 1},
            {QStringLiteral("trigger"), 1}, {QStringLiteral("structure"), 2},
            {QStringLiteral("features"), 3}, {QStringLiteral("audio"), 3}};
    return map;
}

QSet<QString> continuousFields(const QString& type) {
    if (type == QLatin1String("deck.state")) {
        return {QStringLiteral("audible"), QStringLiteral("fader"), QStringLiteral("filter"),
                QStringLiteral("eq.low"), QStringLiteral("eq.mid"), QStringLiteral("eq.high")};
    }
    if (type == QLatin1String("mix.state")) {
        return {QStringLiteral("crossfader"), QStringLiteral("transition.progress")};
    }
    if (type == QLatin1String("intent")) {
        return {QStringLiteral("energy"), QStringLiteral("valence"), QStringLiteral("arousal")};
    }
    return {};
}

void flatten(const QJsonObject& o, const QString& prefix, QMap<QString, QJsonValue>* out) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString key = prefix + it.key();
        if (it.value().isObject() && !it.value().toObject().isEmpty()) {
            flatten(it.value().toObject(), key + QLatin1Char('.'), out);
        } else {
            out->insert(key, it.value());
        }
    }
}

void setPath(QJsonObject& root, const QStringList& parts, int i, const QJsonValue& v) {
    if (i == parts.size() - 1) {
        root[parts[i]] = v;
        return;
    }
    QJsonObject child = root.value(parts[i]).toObject();
    setPath(child, parts, i + 1, v);
    root[parts[i]] = child;
}

QJsonObject unflatten(const QMap<QString, QJsonValue>& flat) {
    QJsonObject out;
    for (auto it = flat.cbegin(); it != flat.cend(); ++it) {
        setPath(out, it.key().split(QLatin1Char('.')), 0, it.value());
    }
    return out;
}

void deepMerge(QJsonObject& dst, const QJsonObject& src) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it.value().isObject() && dst.value(it.key()).isObject()) {
            QJsonObject child = dst.value(it.key()).toObject();
            deepMerge(child, it.value().toObject());
            dst[it.key()] = child;
        } else {
            dst[it.key()] = it.value();
        }
    }
}

double monoSeconds() {
    static QElapsedTimer t = [] {
        QElapsedTimer x;
        x.start();
        return x;
    }();
    return t.nsecsElapsed() / 1e9;
}

QString randomHex(int bytes) {
    return QString::fromLatin1(security::randomBytes(bytes).toHex());
}

QHostAddress normalized(const QHostAddress& a) {
    bool ok = false;
    const quint32 v4 = a.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : a;
}

} // namespace

// Batching, dead zone, edges and final value for continuous controls (§8.4).
class ObjectThrottle {
  public:
    static constexpr double kInterval = 0.05;
    static constexpr double kFinalQuiet = 0.05;
    static constexpr double kDeadzone = 0.01;

    explicit ObjectThrottle(QSet<QString> continuous)
            : m_continuous(std::move(continuous)) {
    }

    std::optional<QMap<QString, QJsonValue>> offer(const QMap<QString, QJsonValue>& flat, double now) {
        bool immediate = false;
        for (auto it = flat.cbegin(); it != flat.cend(); ++it) {
            const auto sent = m_sent.constFind(it.key());
            if (m_continuous.contains(it.key())) {
                if (sent == m_sent.cend() || isEdge(it.key(), *sent, it.value())) {
                    immediate = true;
                }
                m_pending.insert(it.key(), it.value());
            } else if (sent == m_sent.cend() || *sent != it.value()) {
                m_pending.insert(it.key(), it.value());
                immediate = true;
            }
        }
        m_lastChange = now;
        if (immediate) {
            return take(now, false);
        }
        if (now - m_lastSend >= kInterval) {
            return take(now, true);
        }
        return std::nullopt;
    }

    std::optional<QMap<QString, QJsonValue>> poll(double now) {
        if (m_pending.isEmpty()) {
            return std::nullopt;
        }
        if (now - m_lastChange >= kFinalQuiet) {
            return take(now, false);
        }
        if (now - m_lastSend >= kInterval) {
            return take(now, true);
        }
        return std::nullopt;
    }

  private:
    bool isEdge(const QString& key, const QJsonValue& oldV, const QJsonValue& newV) const {
        if (!oldV.isDouble() || !newV.isDouble()) {
            return true;
        }
        const double o = oldV.toDouble();
        const double n = newV.toDouble();
        const bool signedRange = key == QLatin1String("filter") || key == QLatin1String("crossfader");
        const bool atEdge = n == 0.0 || n == 1.0 || (signedRange && n == -1.0);
        if (atEdge && n != o) {
            return true;
        }
        return (o == 0.0) != (n == 0.0);
    }

    std::optional<QMap<QString, QJsonValue>> take(double now, bool deadzone) {
        QMap<QString, QJsonValue> out;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            const auto sent = m_sent.constFind(it.key());
            if (sent != m_sent.cend() && *sent == it.value()) {
                it = m_pending.erase(it);
                continue;
            }
            if (deadzone && m_continuous.contains(it.key()) && sent != m_sent.cend() &&
                    sent->isDouble() && it.value().isDouble() &&
                    std::abs(it.value().toDouble() - sent->toDouble()) < kDeadzone) {
                ++it;
                continue;
            }
            out.insert(it.key(), it.value());
            m_sent.insert(it.key(), it.value());
            it = m_pending.erase(it);
        }
        if (out.isEmpty()) {
            return std::nullopt;
        }
        m_lastSend = now;
        return out;
    }

    QSet<QString> m_continuous;
    QMap<QString, QJsonValue> m_sent;
    QMap<QString, QJsonValue> m_pending;
    double m_lastSend = -1e9;
    double m_lastChange = 0;
};

struct Emitter::Session {
    enum class Stage { AwaitHello, AwaitAuth, Open };
    WsConnection* ws = nullptr;
    QHostAddress ip;
    Stage stage = Stage::AwaitHello;
    QString clientId;
    QString name;
    QString role;
    QList<int> profiles;
    QSet<QString> topics;
    qint64 latencyUs = 0;
    quint16 udpPort = 0;
    bool rtOverControl = false;
    QByteArray psk;
    QByteArray serverNonce;
    QByteArray macKey;
    bool ready = false;
    quint64 jsonSeq = 0;
    QHash<int, quint32> udpSeq;
    SeqWindow pingWindow;
    double lastRx = 0;
    double lastTx = 0;
    double openedAt = 0;
    QList<double> rxTimes;
    QTimer* deadline = nullptr;
    QTimer* autoSnapshot = nullptr;
    bool counted = true; // still in the unauthenticated quota
};

template<typename F>
bool Emitter::post(F&& f) {
    if (QThread::currentThread() == thread()) {
        return false;
    }
    QMetaObject::invokeMethod(this, std::forward<F>(f), Qt::QueuedConnection);
    return true;
}

Emitter::Emitter(const EmitterConfig& config, QObject* parent)
        : QObject(parent), m_cfg(config), m_rtTimer(this), m_hkTimer(this) {
    // The timers are children so they follow the emitter when it is moved to another thread.
    m_id = m_cfg.id.isEmpty() ? QStringLiteral("vx-") + randomHex(4) : m_cfg.id;
    m_path = m_cfg.upstreamPath;
    m_path.append(m_id);
    m_origin = m_path.first();
    m_sessionId = randomHex(3);
    do {
        m_sessionKey = QRandomGenerator::system()->generate();
    } while (m_sessionKey == 0);
    m_tempo.playing = false;
    for (int d = 1; d <= m_cfg.decks; ++d) {
        m_decks.insert(d, emptyDeck(d));
    }
    QJsonObject tr;
    tr[QStringLiteral("active")] = false;
    tr[QStringLiteral("kind")] = QJsonValue();
    tr[QStringLiteral("from_deck")] = QJsonValue();
    tr[QStringLiteral("to_deck")] = QJsonValue();
    tr[QStringLiteral("progress")] = QJsonValue();
    m_mix[QStringLiteral("crossfader")] = 0.0;
    m_mix[QStringLiteral("on_air_deck")] = QJsonValue();
    m_mix[QStringLiteral("transition")] = tr;
    m_rtTimer.setTimerType(Qt::PreciseTimer);
    m_rtTimer.setInterval(4);
    connect(&m_rtTimer, &QTimer::timeout, this, &Emitter::realtimeTick);
    m_hkTimer.setTimerType(Qt::PreciseTimer);
    m_hkTimer.setInterval(10);
    connect(&m_hkTimer, &QTimer::timeout, this, &Emitter::housekeeping);
}

Emitter::~Emitter() {
    if (m_started) {
        stop();
    }
}

QJsonObject Emitter::emptyDeck(int deck) {
    QJsonObject d;
    d[QStringLiteral("deck")] = deck;
    d[QStringLiteral("track_id")] = QJsonValue();
    d[QStringLiteral("loaded")] = false;
    d[QStringLiteral("playing")] = false;
    d[QStringLiteral("position_s")] = 0.0;
    d[QStringLiteral("position_beat")] = 0.0;
    d[QStringLiteral("rate")] = 1.0;
    d[QStringLiteral("bpm")] = QJsonValue();
    d[QStringLiteral("key")] = QJsonValue();
    QJsonObject loop;
    loop[QStringLiteral("active")] = false;
    loop[QStringLiteral("in_s")] = QJsonValue();
    loop[QStringLiteral("out_s")] = QJsonValue();
    d[QStringLiteral("loop")] = loop;
    d[QStringLiteral("on_air")] = false;
    d[QStringLiteral("audible")] = 0.0;
    d[QStringLiteral("fader")] = 0.0;
    QJsonObject eq;
    eq[QStringLiteral("low")] = 0.5;
    eq[QStringLiteral("mid")] = 0.5;
    eq[QStringLiteral("high")] = 0.5;
    eq[QStringLiteral("low_kill")] = false;
    eq[QStringLiteral("mid_kill")] = false;
    eq[QStringLiteral("high_kill")] = false;
    d[QStringLiteral("eq")] = eq;
    d[QStringLiteral("filter")] = 0.0;
    d[QStringLiteral("pfl")] = false;
    d[QStringLiteral("scratching")] = false;
    QJsonObject slip;
    slip[QStringLiteral("active")] = false;
    slip[QStringLiteral("position_s")] = QJsonValue();
    d[QStringLiteral("slip")] = slip;
    return d;
}

// ============================================================================ lifecycle

bool Emitter::start(QString* error) {
    m_server = new WsServer(this);
    if (!m_server->listen(m_cfg.host, m_cfg.port)) {
        if (error) {
            *error = QStringLiteral("cannot listen on TCP %1: %2").arg(m_cfg.port).arg(m_server->errorString());
        }
        delete m_server;
        m_server = nullptr;
        return false;
    }
    m_cfg.port = m_server->serverPort();
    connect(m_server, &WsServer::newConnection, this, &Emitter::onNewConnection);
    m_udp = new QUdpSocket(this);
    if (!m_udp->bind(m_cfg.host, m_cfg.udpPort)) {
        if (error) {
            *error = QStringLiteral("cannot bind UDP %1: %2").arg(m_cfg.udpPort).arg(m_udp->errorString());
        }
        delete m_server;
        m_server = nullptr;
        delete m_udp;
        m_udp = nullptr;
        return false;
    }
    m_cfg.udpPort = m_udp->localPort();
    connect(m_udp, &QUdpSocket::readyRead, this, &Emitter::onDatagrams);
    m_rtTimer.start();
    m_hkTimer.start();
    m_started = true;
    if (m_cfg.announce) {
        const bool local = m_cfg.host.isLoopback();
        MdnsService svc;
        svc.instance = m_cfg.name;
        svc.host = QStringLiteral("vertexav-") + QString(m_id).remove(QStringLiteral("vx-"));
        svc.port = m_cfg.port;
        if (!m_cfg.host.isNull() && m_cfg.host != QHostAddress::AnyIPv4 && m_cfg.host != QHostAddress::Any) {
            svc.addresses.append(m_cfg.host);
        }
        QStringList profiles;
        for (int p : m_cfg.profiles) {
            profiles.append(QString::number(p));
        }
        svc.txt.insert(QStringLiteral("v"), QString::fromLatin1(kVersion));
        svc.txt.insert(QStringLiteral("role"), m_cfg.role);
        svc.txt.insert(QStringLiteral("id"), m_id);
        svc.txt.insert(QStringLiteral("origin"), m_origin);
        svc.txt.insert(QStringLiteral("hops"), QString::number(m_path.size() - 1));
        svc.txt.insert(QStringLiteral("path"), m_path.join(QLatin1Char(',')));
        svc.txt.insert(QStringLiteral("name"), m_cfg.name);
        svc.txt.insert(QStringLiteral("profiles"), profiles.join(QLatin1Char(',')));
        svc.txt.insert(QStringLiteral("security"), m_cfg.security);
        svc.txt.insert(QStringLiteral("scope"), local ? QStringLiteral("local") : QStringLiteral("lan"));
        m_mdns = new MdnsResponder(this);
        if (!m_mdns->start(svc, local)) {
            qWarning("vertexav: DNS-SD announcement failed (port 5353 unavailable?)");
        }
    }
    return true;
}

void Emitter::stop(const QString& reason) {
    if (!m_started) {
        return;
    }
    m_started = false;
    if (m_mdns) {
        m_mdns->stop();
    }
    const QList<Session*> sessions = m_sessions;
    for (Session* s : sessions) {
        if (s->stage == Session::Stage::Open) {
            QJsonObject bye;
            bye[QStringLiteral("type")] = QStringLiteral("bye");
            bye[QStringLiteral("reason")] = reason;
            s->ws->sendText(dumps(bye));
        }
        s->ws->close(1001, "shutdown");
    }
    m_rtTimer.stop();
    m_hkTimer.stop();
    if (m_server) {
        m_server->close();
    }
    if (m_udp) {
        m_udp->close();
    }
}

// ============================================================================ publishing API

void Emitter::setTempo(const Tempo& tempo) {
    if (post([this, tempo] { setTempo(tempo); })) {
        return;
    }
    const qint64 t = now();
    m_tempo = tempo;
    if (tempo.phaseReset) {
        m_phaseResetUntil = t + 250000;
    }
    QJsonObject msg = tempo.toJson();
    msg[QStringLiteral("type")] = QStringLiteral("tempo");
    msg[QStringLiteral("t")] = i64(t);
    publish(msg);
    sendUdpTempo(t);
    m_tempo.phaseReset = false;
}

void Emitter::throttled(const QString& key, const QString& type, const QJsonObject& fields, int deck) {
    auto& th = m_throttles[key];
    if (!th) {
        th = std::make_shared<ObjectThrottle>(continuousFields(type));
    }
    QMap<QString, QJsonValue> flat;
    flatten(fields, QString(), &flat);
    if (auto out = th->offer(flat, monoSeconds())) {
        QJsonObject msg = unflatten(*out);
        msg[QStringLiteral("type")] = type;
        msg[QStringLiteral("t")] = i64(now());
        if (deck > 0) {
            msg[QStringLiteral("deck")] = deck;
        }
        publish(msg);
    }
}

void Emitter::updateDeck(int deck, const QJsonObject& fields) {
    if (post([this, deck, fields] { updateDeck(deck, fields); })) {
        return;
    }
    if (!m_decks.contains(deck)) {
        m_decks.insert(deck, emptyDeck(deck));
    }
    deepMerge(m_decks[deck], fields);
    throttled(QStringLiteral("deck:%1").arg(deck), QStringLiteral("deck.state"), fields, deck);
}

void Emitter::updateMix(const QJsonObject& fields) {
    if (post([this, fields] { updateMix(fields); })) {
        return;
    }
    deepMerge(m_mix, fields);
    throttled(QStringLiteral("mix"), QStringLiteral("mix.state"), fields, 0);
}

void Emitter::updateIntent(const QJsonObject& fields) {
    if (post([this, fields] { updateIntent(fields); })) {
        return;
    }
    deepMerge(m_intent, fields);
    throttled(QStringLiteral("intent"), QStringLiteral("intent"), fields, 0);
}

void Emitter::trackLoaded(int deck, const QString& trackId, const QJsonObject& meta, bool structureAvailable,
        const QJsonValue& artwork) {
    if (post([=] { trackLoaded(deck, trackId, meta, structureAvailable, artwork); })) {
        return;
    }
    QJsonObject body;
    body[QStringLiteral("deck")] = deck;
    body[QStringLiteral("track_id")] = trackId;
    body[QStringLiteral("meta")] = meta;
    body[QStringLiteral("artwork")] = artwork.isUndefined() ? QJsonValue() : artwork;
    body[QStringLiteral("structure_available")] = structureAvailable;
    m_tracks.insert(deck, body);
    QJsonObject msg = body;
    msg[QStringLiteral("type")] = QStringLiteral("track.loaded");
    msg[QStringLiteral("t")] = i64(now());
    publish(msg);
    QJsonObject upd;
    upd[QStringLiteral("track_id")] = trackId;
    upd[QStringLiteral("loaded")] = true;
    upd[QStringLiteral("bpm")] = meta.value(QStringLiteral("bpm"));
    upd[QStringLiteral("key")] = meta.value(QStringLiteral("key"));
    updateDeck(deck, upd);
}

void Emitter::trackUnloaded(int deck) {
    if (post([this, deck] { trackUnloaded(deck); })) {
        return;
    }
    m_tracks.remove(deck);
    QJsonObject msg;
    msg[QStringLiteral("type")] = QStringLiteral("track.unloaded");
    msg[QStringLiteral("t")] = i64(now());
    msg[QStringLiteral("deck")] = deck;
    publish(msg);
    QJsonObject upd;
    upd[QStringLiteral("track_id")] = QJsonValue();
    upd[QStringLiteral("loaded")] = false;
    upd[QStringLiteral("playing")] = false;
    updateDeck(deck, upd);
}

void Emitter::trackStructure(const QJsonObject& structure) {
    if (post([this, structure] { trackStructure(structure); })) {
        return;
    }
    const QString tid = structure.value(QStringLiteral("track_id")).toString();
    m_structures.insert(tid, structure);
    QSet<QString> loaded;
    for (const QJsonObject& t : std::as_const(m_tracks)) {
        loaded.insert(t.value(QStringLiteral("track_id")).toString());
    }
    for (auto it = m_structures.begin(); it != m_structures.end();) {
        if (!loaded.contains(it.key()) && it.key() != tid) {
            it = m_structures.erase(it);
        } else {
            ++it;
        }
    }
    QJsonObject msg = structure;
    msg[QStringLiteral("type")] = QStringLiteral("track.structure");
    msg[QStringLiteral("t")] = i64(now());
    publish(msg);
}

QString Emitter::emitEvent(const QString& name, qint64 at, int deck, const QJsonObject& data) {
    const QString id = QStringLiteral("e-%1").arg(++m_eventCounter);
    const qint64 when = at < 0 ? now() : at;
    if (!post([=] { publishEvent(id, name, when, deck, data); })) {
        publishEvent(id, name, when, deck, data);
    }
    return id;
}

void Emitter::publishEvent(const QString& id, const QString& name, qint64 at, int deck, const QJsonObject& data) {
    const qint64 t = now();
    QJsonObject msg;
    msg[QStringLiteral("type")] = QStringLiteral("event");
    msg[QStringLiteral("t")] = i64(t);
    msg[QStringLiteral("at")] = i64(at);
    msg[QStringLiteral("event_id")] = id;
    msg[QStringLiteral("name")] = name;
    msg[QStringLiteral("deck")] = deck > 0 ? QJsonValue(deck) : QJsonValue();
    msg[QStringLiteral("data")] = data;
    if (at > t) {
        m_events.insert(id, msg);
    }
    publish(msg);
}

void Emitter::cancelEvent(const QString& eventId, const QString& reason) {
    if (post([=] { cancelEvent(eventId, reason); })) {
        return;
    }
    m_events.remove(eventId);
    QJsonObject msg;
    msg[QStringLiteral("type")] = QStringLiteral("event.cancel");
    msg[QStringLiteral("t")] = i64(now());
    msg[QStringLiteral("event_id")] = eventId;
    msg[QStringLiteral("reason")] = reason;
    publish(msg);
}

void Emitter::trigger(const QString& name, const QString& state, std::optional<double> value, int deck, qint64 at) {
    if (post([=] { trigger(name, state, value, deck, at); })) {
        return;
    }
    const qint64 t = now();
    QJsonObject msg;
    msg[QStringLiteral("type")] = QStringLiteral("trigger");
    msg[QStringLiteral("t")] = i64(t);
    msg[QStringLiteral("at")] = i64(at < 0 ? t : at);
    msg[QStringLiteral("name")] = name;
    msg[QStringLiteral("state")] = state;
    msg[QStringLiteral("value")] = value ? QJsonValue(*value) : QJsonValue();
    msg[QStringLiteral("deck")] = deck > 0 ? QJsonValue(deck) : QJsonValue();
    publish(msg);
}

void Emitter::setArtwork(const QString& key, const QByteArray& data, const QByteArray& mimeType) {
    if (post([=] { setArtwork(key, data, mimeType); })) {
        return;
    }
    if (data.size() > 2 * 1024 * 1024) {
        return; // §15.7
    }
    m_artwork.insert(key, qMakePair(data, mimeType));
    if (m_artwork.size() > 16) {
        m_artwork.erase(m_artwork.begin());
    }
}

QJsonObject Emitter::snapshot() const {
    const qint64 t = now();
    QJsonObject s;
    s[QStringLiteral("type")] = QStringLiteral("snapshot");
    s[QStringLiteral("t")] = i64(t);
    QJsonObject tempo = m_tempo.toJson();
    tempo[QStringLiteral("phase_reset")] = false;
    s[QStringLiteral("tempo")] = tempo;
    QJsonArray decks;
    for (const QJsonObject& d : m_decks) {
        decks.append(d);
    }
    s[QStringLiteral("decks")] = decks;
    s[QStringLiteral("mix")] = m_mix;
    QJsonArray tracks;
    for (const QJsonObject& tr : m_tracks) {
        tracks.append(tr);
    }
    s[QStringLiteral("tracks")] = tracks;
    QJsonArray structures;
    for (const QJsonObject& st : m_structures) {
        structures.append(st);
    }
    s[QStringLiteral("structures")] = structures;
    s[QStringLiteral("intent")] = m_intent.isEmpty() ? QJsonValue() : QJsonValue(m_intent);
    QJsonArray events;
    for (const QJsonObject& e : m_events) {
        if (static_cast<qint64>(e.value(QStringLiteral("at")).toDouble()) >= t) {
            events.append(e);
        }
    }
    s[QStringLiteral("events")] = events;
    return s;
}

// ============================================================================ sending

void Emitter::publish(const QJsonObject& msg) {
    const QString topic = topicOf(msg.value(QStringLiteral("type")).toString());
    for (Session* s : std::as_const(m_sessions)) {
        if (s->stage == Session::Stage::Open && s->ready && (topic.isEmpty() || s->topics.contains(topic))) {
            sendTo(s, msg);
        }
    }
}

void Emitter::sendTo(Session* s, QJsonObject msg) {
    const QString type = msg.value(QStringLiteral("type")).toString();
    if (type != QLatin1String("error") && type != QLatin1String("bye")) {
        msg[QStringLiteral("seq")] = i64(static_cast<qint64>(++s->jsonSeq));
    }
    const QByteArray text = dumps(msg);
    if (text.size() > kMaxMsgEmitterToReceiver) {
        qWarning("vertexav: %s message of %d bytes exceeds 512 KB, not sent", qPrintable(type),
                static_cast<int>(text.size()));
        return;
    }
    if (s->ws->bytesToWrite() > 4 * 1024 * 1024) {
        s->ws->close(1008, "receiver too slow");
        return;
    }
    s->ws->sendText(text);
    s->lastTx = monoSeconds();
}

void Emitter::sendSnapshot(Session* s) {
    if (s->ready) {
        return;
    }
    s->ready = true;
    sendTo(s, snapshot());
}

void Emitter::sendUdpTempo(qint64 t) {
    m_lastUdpTempo = t;
    QByteArray body;
    TempoPacket pkt;
    bool built = false;
    for (Session* s : std::as_const(m_sessions)) {
        if (s->stage != Session::Stage::Open || !s->ready || !s->topics.contains(QStringLiteral("tempo"))) {
            continue;
        }
        if (!built) {
            pkt = m_tempo.toPacket(t, t < m_phaseResetUntil);
            body = pkt.packBody();
            built = true;
        }
        if (s->rtOverControl) {
            QJsonObject m;
            m[QStringLiteral("type")] = QStringLiteral("tempo.rt");
            m[QStringLiteral("t")] = i64(t);
            m[QStringLiteral("bpm")] = pkt.bpm;
            m[QStringLiteral("anchor_t")] = i64(pkt.anchorT);
            m[QStringLiteral("anchor_beat")] = pkt.anchorBeat;
            m[QStringLiteral("next_beat_t")] = i64(pkt.nextBeatT);
            m[QStringLiteral("next_downbeat_t")] = i64(pkt.nextDownbeatT);
            m[QStringLiteral("playing")] = pkt.playing;
            m[QStringLiteral("stability")] = stabilityName(pkt.stability);
            m[QStringLiteral("phase_reset")] = pkt.phaseReset;
            sendTo(s, m);
        } else if (s->udpPort && m_udp) {
            const quint32 seq = ++s->udpSeq[static_cast<int>(Kind::Tempo)];
            m_udp->writeDatagram(packPacket(Kind::Tempo, seq, m_sessionKey, t, body, s->macKey), s->ip, s->udpPort);
        }
    }
}

void Emitter::realtimeTick() {
    // UDP tempo at 20 Hz while playing (2 Hz when stopped) plus one packet per beat sent
    // lookahead_us before it (§9.3).
    const qint64 t = now();
    const qint64 period = m_tempo.playing ? 50000 : 500000;
    bool due = t - m_lastUdpTempo >= period;
    if (m_tempo.playing && m_tempo.stability != Stability::Unstable) {
        const qint64 nb = m_tempo.nextBeatT(t);
        if (nb - t <= m_cfg.lookaheadUs && nb != m_lastBeatAnnounced) {
            m_lastBeatAnnounced = nb;
            due = true;
        }
    }
    if (due) {
        sendUdpTempo(t);
    }
}

void Emitter::housekeeping() {
    const double mono = monoSeconds();
    for (auto it = m_throttles.begin(); it != m_throttles.end(); ++it) {
        if (auto out = it.value()->poll(mono)) {
            QJsonObject msg = unflatten(*out);
            const QString& key = it.key();
            if (key.startsWith(QLatin1String("deck:"))) {
                msg[QStringLiteral("type")] = QStringLiteral("deck.state");
                msg[QStringLiteral("deck")] = key.mid(5).toInt();
            } else if (key == QLatin1String("mix")) {
                msg[QStringLiteral("type")] = QStringLiteral("mix.state");
            } else {
                msg[QStringLiteral("type")] = QStringLiteral("intent");
            }
            msg[QStringLiteral("t")] = i64(now());
            publish(msg);
        }
    }
    const qint64 t = now();
    for (auto it = m_events.begin(); it != m_events.end();) {
        if (static_cast<qint64>(it->value(QStringLiteral("at")).toDouble()) < t - 1000000) {
            it = m_events.erase(it);
        } else {
            ++it;
        }
    }
    const QList<Session*> sessions = m_sessions;
    for (Session* s : sessions) {
        if (s->stage != Session::Stage::Open) {
            continue;
        }
        if (mono - s->lastTx >= kHeartbeatIntervalMs / 1000.0) {
            QJsonObject hb;
            hb[QStringLiteral("type")] = QStringLiteral("heartbeat");
            hb[QStringLiteral("t")] = i64(t);
            sendTo(s, hb);
        }
        if (mono - s->lastRx > kHeartbeatTimeoutMs / 1000.0) {
            s->lastRx = mono;
            s->ws->close(1001, "heartbeat timeout");
        }
    }
}

// ============================================================================ control channel

bool Emitter::lockedOut(const QHostAddress& ip) {
    const qint64 nowMs = static_cast<qint64>(monoSeconds() * 1000);
    QList<qint64>& fails = m_authFailures[ip.toString()];
    fails.erase(std::remove_if(fails.begin(), fails.end(), [&](qint64 t) { return nowMs - t > 30000; }),
            fails.end());
    return fails.size() >= 5;
}

void Emitter::onNewConnection(WsConnection* ws) {
    const QHostAddress ip = normalized(ws->peerAddress());
    const QString ipKey = ip.toString();
    int total = 0;
    for (int n : std::as_const(m_unauth)) {
        total += n;
    }
    if (m_unauth.value(ipKey) >= kMaxUnauthPerIp || total >= kMaxUnauthTotal) {
        ws->abort();
        ws->deleteLater();
        return;
    }
    m_unauth[ipKey] += 1;
    auto* s = new Session();
    s->ws = ws;
    s->ip = ip;
    s->openedAt = monoSeconds();
    ws->setExpectedPath(QString::fromLatin1(kWsPath));
    ws->setMaxMessageSize(kMaxMsgPreAuth);
    ws->setHttpHandler([this](const QString& path, QByteArray* body, QByteArray* type) {
        if (!path.startsWith(QLatin1String("/artwork/"))) {
            return false;
        }
        QString key = path.mid(9);
        const int dot = key.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) {
            key = key.left(dot);
        }
        const auto it = m_artwork.constFind(key);
        if (it == m_artwork.cend()) {
            return false;
        }
        *body = it->first;
        *type = it->second;
        return true;
    });
    s->deadline = new QTimer(ws);
    s->deadline->setSingleShot(true);
    connect(s->deadline, &QTimer::timeout, ws, [ws] { ws->abort(); }); // no answer (§15.7)
    s->deadline->start(kHelloTimeoutMs);
    m_sessions.append(s);
    connect(ws, &WsConnection::textReceived, this, [this, s](const QByteArray& text) { onText(s, text); });
    connect(ws, &WsConnection::messageTooLarge, this, [this, s] {
        if (s->stage == Session::Stage::Open) {
            fatal(s, QStringLiteral("message_too_large"), QStringLiteral("Message over the size limit"));
        } else {
            s->ws->abort();
        }
    });
    // Queued so a session is never deleted in the middle of a call that is still using it.
    connect(ws, &WsConnection::disconnected, this, [this, s] { dropSession(s); }, Qt::QueuedConnection);
}

void Emitter::dropSession(Session* s) {
    if (!m_sessions.removeOne(s)) {
        return;
    }
    if (s->counted) {
        const QString key = s->ip.toString();
        m_unauth[key] = std::max(0, m_unauth.value(key) - 1);
        if (m_unauth[key] == 0) {
            m_unauth.remove(key);
        }
    }
    if (s->stage == Session::Stage::Open) {
        m_receiverCount.fetch_sub(1);
        emit receiverDisconnected(s->clientId, s->name);
    }
    s->ws->deleteLater();
    delete s;
}

void Emitter::fatal(Session* s, const QString& code, const QString& message) {
    QJsonObject err;
    err[QStringLiteral("type")] = QStringLiteral("error");
    err[QStringLiteral("code")] = code;
    err[QStringLiteral("message")] = message;
    err[QStringLiteral("fatal")] = true;
    s->ws->sendText(dumps(err));
    s->ws->close(1008, code.toUtf8());
}

void Emitter::onText(Session* s, const QByteArray& text) {
    if (jsonDepthExceeds(text)) {
        if (s->stage == Session::Stage::Open) {
            fatal(s, QStringLiteral("message_too_large"), QStringLiteral("JSON nesting too deep"));
        } else {
            s->ws->abort();
        }
        return;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(text, &perr);
    const QJsonObject msg = doc.object();
    const QString type = msg.value(QStringLiteral("type")).toString();
    switch (s->stage) {
    case Session::Stage::AwaitHello:
        if (!doc.isObject() || type != QLatin1String("hello")) {
            s->ws->abort(); // before auth only hello and auth.response are accepted
            return;
        }
        handleHello(s, msg);
        return;
    case Session::Stage::AwaitAuth:
        if (!doc.isObject() || type != QLatin1String("auth.response")) {
            s->ws->abort();
            return;
        }
        handleAuthResponse(s, msg);
        return;
    case Session::Stage::Open:
        break;
    }
    const double mono = monoSeconds();
    s->lastRx = mono;
    s->rxTimes.append(mono);
    while (!s->rxTimes.isEmpty() && mono - s->rxTimes.first() > 1.0) {
        s->rxTimes.removeFirst();
    }
    if (s->rxTimes.size() > 50) {
        if (s->rxTimes.size() == 51) {
            QJsonObject err;
            err[QStringLiteral("type")] = QStringLiteral("error");
            err[QStringLiteral("code")] = QStringLiteral("rate_limited");
            err[QStringLiteral("message")] = QStringLiteral("More than 50 messages per second");
            err[QStringLiteral("fatal")] = false;
            sendTo(s, err);
        }
        return;
    }
    if (!doc.isObject() || type.isEmpty()) {
        QJsonObject err;
        err[QStringLiteral("type")] = QStringLiteral("error");
        err[QStringLiteral("code")] = QStringLiteral("bad_message");
        err[QStringLiteral("message")] = QStringLiteral("Malformed JSON or missing type");
        err[QStringLiteral("fatal")] = false;
        sendTo(s, err);
        return;
    }
    handleSessionMessage(s, msg);
}

void Emitter::handleHello(Session* s, const QJsonObject& msg) {
    const QString version = msg.value(QStringLiteral("version")).toString();
    if (version.section(QLatin1Char('.'), 0, 0) != QString::fromLatin1(kVersion).section(QLatin1Char('.'), 0, 0) ||
            msg.value(QStringLiteral("protocol")).toString() != QLatin1String(kProtocol)) {
        fatal(s, QStringLiteral("unsupported_version"), QStringLiteral("Version %1 not supported").arg(version));
        return;
    }
    if (!msg.value(QStringLiteral("client_id")).isString() || !msg.value(QStringLiteral("profiles")).isArray() ||
            !msg.value(QStringLiteral("name")).isString()) {
        s->ws->abort();
        return;
    }
    s->clientId = msg.value(QStringLiteral("client_id")).toString();
    s->name = msg.value(QStringLiteral("name")).toString();
    s->role = msg.value(QStringLiteral("role")).toString(QStringLiteral("receiver"));
    QSet<int> wanted;
    for (const QJsonValue& v : msg.value(QStringLiteral("profiles")).toArray()) {
        wanted.insert(v.toInt(-1));
    }
    for (int p : std::as_const(m_cfg.profiles)) {
        if (wanted.contains(p)) {
            s->profiles.append(p);
        }
    }
    std::sort(s->profiles.begin(), s->profiles.end());
    s->latencyUs = static_cast<qint64>(msg.value(QStringLiteral("latency_us")).toDouble(0));
    s->rtOverControl = msg.value(QStringLiteral("realtime_over_control")).toBool(false);
    const int udpPort = msg.value(QStringLiteral("udp_port")).toInt(0);
    s->udpPort = udpPort > 0 && udpPort < 65536 ? static_cast<quint16>(udpPort) : 0;
    if (m_cfg.security == QLatin1String("open")) {
        sendWelcome(s, QString());
        return;
    }
    if (lockedOut(s->ip)) {
        s->ws->abort();
        return;
    }
    s->psk = m_cfg.keys.value(s->clientId, m_cfg.psk);
    if (s->psk.isEmpty()) {
        fatal(s, QStringLiteral("auth_required"), QStringLiteral("Receiver not paired"));
        return;
    }
    s->serverNonce = security::randomBytes(security::kNonceLen);
    QJsonObject ch;
    ch[QStringLiteral("type")] = QStringLiteral("auth.challenge");
    ch[QStringLiteral("server_nonce")] = QString::fromLatin1(s->serverNonce.toBase64());
    s->ws->sendText(dumps(ch));
    s->stage = Session::Stage::AwaitAuth;
    const int elapsed = static_cast<int>((monoSeconds() - s->openedAt) * 1000);
    s->deadline->start(std::max(1, std::min(kAuthResponseTimeoutMs, kWelcomeTimeoutMs - elapsed)));
}

void Emitter::handleAuthResponse(Session* s, const QJsonObject& msg) {
    const QByteArray clientNonce = QByteArray::fromBase64(msg.value(QStringLiteral("client_nonce")).toString().toLatin1());
    const QByteArray proof = QByteArray::fromBase64(msg.value(QStringLiteral("proof")).toString().toLatin1());
    const QByteArray expected = security::clientProof(s->psk, s->serverNonce, clientNonce, s->clientId);
    if (clientNonce.size() != security::kNonceLen || !security::constantTimeEquals(expected, proof)) {
        m_authFailures[s->ip.toString()].append(static_cast<qint64>(monoSeconds() * 1000));
        fatal(s, QStringLiteral("auth_failed"), QStringLiteral("Wrong authentication proof"));
        return;
    }
    s->macKey = security::macKey(s->psk, s->serverNonce, clientNonce);
    const QByteArray sp = security::serverProof(s->psk, clientNonce, s->serverNonce, m_sessionId);
    sendWelcome(s, QString::fromLatin1(sp.toBase64()));
}

void Emitter::sendWelcome(Session* s, const QString& serverProof) {
    if ((monoSeconds() - s->openedAt) * 1000 > kWelcomeTimeoutMs) {
        s->ws->abort();
        return;
    }
    if (m_receiverCount.load() >= m_cfg.maxReceivers) {
        fatal(s, QStringLiteral("internal"), QStringLiteral("Too many receivers"));
        return;
    }
    s->deadline->stop();
    s->stage = Session::Stage::Open;
    s->counted = false;
    const QString key = s->ip.toString();
    m_unauth[key] = std::max(0, m_unauth.value(key) - 1);
    if (m_unauth[key] == 0) {
        m_unauth.remove(key);
    }
    s->ws->setMaxMessageSize(kMaxMsgReceiverToEmitter);
    for (auto it = topicProfiles().cbegin(); it != topicProfiles().cend(); ++it) {
        if (s->profiles.contains(it.value())) {
            s->topics.insert(it.key());
        }
    }
    QJsonObject em;
    em[QStringLiteral("id")] = m_id;
    em[QStringLiteral("name")] = m_cfg.name;
    em[QStringLiteral("role")] = m_cfg.role;
    em[QStringLiteral("path")] = QJsonArray::fromStringList(m_path);
    QJsonArray profiles;
    for (int p : std::as_const(s->profiles)) {
        profiles.append(p);
    }
    QJsonObject link;
    link[QStringLiteral("enabled")] = m_cfg.linkEnabled;
    QJsonObject w;
    w[QStringLiteral("type")] = QStringLiteral("welcome");
    w[QStringLiteral("version")] = QString::fromLatin1(kVersion);
    w[QStringLiteral("session_id")] = m_sessionId;
    w[QStringLiteral("session_key")] = i64(m_sessionKey);
    w[QStringLiteral("emitter")] = em;
    w[QStringLiteral("security")] = m_cfg.security;
    w[QStringLiteral("server_proof")] = serverProof.isEmpty() ? QJsonValue() : QJsonValue(serverProof);
    w[QStringLiteral("profiles")] = profiles;
    w[QStringLiteral("udp_port")] = m_cfg.udpPort;
    w[QStringLiteral("multicast")] = QJsonValue();
    w[QStringLiteral("decks")] = m_decks.size();
    w[QStringLiteral("lookahead_us")] = i64(m_cfg.lookaheadUs);
    w[QStringLiteral("link")] = link;
    s->ws->sendText(dumps(w));
    s->lastRx = s->lastTx = monoSeconds();
    m_receiverCount.fetch_add(1);
    s->autoSnapshot = new QTimer(s->ws);
    s->autoSnapshot->setSingleShot(true);
    connect(s->autoSnapshot, &QTimer::timeout, this, [this, s] { sendSnapshot(s); });
    s->autoSnapshot->start(1000);
    emit receiverConnected(s->clientId, s->name);
}

void Emitter::handleSessionMessage(Session* s, const QJsonObject& msg) {
    const QString type = msg.value(QStringLiteral("type")).toString();
    auto error = [&](const QString& code, const QString& text) {
        QJsonObject err;
        err[QStringLiteral("type")] = QStringLiteral("error");
        err[QStringLiteral("code")] = code;
        err[QStringLiteral("message")] = text;
        err[QStringLiteral("fatal")] = false;
        sendTo(s, err);
    };
    if (type == QLatin1String("subscribe")) {
        if (!msg.value(QStringLiteral("topics")).isArray()) {
            error(QStringLiteral("bad_message"), QStringLiteral("topics must be a list"));
            return;
        }
        QSet<QString> allowed;
        for (const QJsonValue& v : msg.value(QStringLiteral("topics")).toArray()) {
            const QString t = v.toString();
            const auto it = topicProfiles().constFind(t);
            if (it == topicProfiles().cend()) {
                continue;
            }
            if (s->profiles.contains(it.value())) {
                allowed.insert(t);
            } else {
                error(QStringLiteral("unsupported_profile"),
                        QStringLiteral("Topic %1 needs profile %2").arg(t).arg(it.value()));
            }
        }
        s->topics = allowed;
        if (s->autoSnapshot) {
            s->autoSnapshot->stop();
        }
        s->ready = false;
        sendSnapshot(s);
    } else if (type == QLatin1String("clock.ping")) {
        QJsonObject pong;
        pong[QStringLiteral("type")] = QStringLiteral("clock.pong");
        pong[QStringLiteral("t0")] = msg.value(QStringLiteral("t0"));
        pong[QStringLiteral("ping_id")] = msg.value(QStringLiteral("ping_id"));
        pong[QStringLiteral("t1")] = i64(now());
        pong[QStringLiteral("t2")] = i64(now());
        sendTo(s, pong);
    } else if (type == QLatin1String("latency.update")) {
        s->latencyUs = static_cast<qint64>(msg.value(QStringLiteral("latency_us")).toDouble(s->latencyUs));
    } else if (type == QLatin1String("audio.subscribe")) {
        error(QStringLiteral("unsupported_transport"), QStringLiteral("This emitter has no audio (profile 3)"));
    } else if (type == QLatin1String("bye")) {
        s->ws->close(1000);
        return;
    } else if (type == QLatin1String("error") && msg.value(QStringLiteral("fatal")).toBool()) {
        s->ws->close(1000);
        return;
    } else if (type == QLatin1String("hello") || type == QLatin1String("auth.response")) {
        error(QStringLiteral("bad_message"), QStringLiteral("Unexpected %1").arg(type));
    }
    emit receiverMessage(s->clientId, msg);
}

// ============================================================================ UDP

void Emitter::onDatagrams() {
    while (m_udp && m_udp->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_udp->receiveDatagram(kUdpRecvBuffer);
        const qint64 t1 = now(); // as early as possible (§7.2)
        const QHostAddress from = normalized(dg.senderAddress());
        // The receiver pings from the socket it announced in hello: (ip, port) identifies the
        // session and therefore its mac_key.
        Session* s = nullptr;
        for (Session* c : std::as_const(m_sessions)) {
            if (c->stage == Session::Stage::Open && c->udpPort == dg.senderPort() && c->ip == from) {
                s = c;
                break;
            }
        }
        if (!s) {
            continue;
        }
        const Validation v = validateDatagram(dg.data(), m_sessionKey, s->macKey, !s->macKey.isEmpty());
        if (!v.ok || v.header.kind != static_cast<quint8>(Kind::ClockPing) || !s->pingWindow.accept(v.header.seq)) {
            continue;
        }
        PingPacket ping;
        if (!PingPacket::unpackBody(v.body, &ping)) {
            continue;
        }
        PongPacket pong;
        pong.t0 = ping.t0;
        pong.pingId = ping.pingId;
        pong.t1 = t1;
        pong.t2 = now();
        const quint32 seq = ++s->udpSeq[static_cast<int>(Kind::ClockPong)];
        m_udp->writeDatagram(packPacket(Kind::ClockPong, seq, m_sessionKey, pong.t2, pong.packBody(), s->macKey),
                dg.senderAddress(), dg.senderPort());
    }
}

} // namespace vertexav
