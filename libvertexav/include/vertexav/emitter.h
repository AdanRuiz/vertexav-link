// VertexAV Link emitter: control channel (WebSocket), real-time channel (UDP) and DNS-SD.
//
// The emitter keeps the current state, sends a snapshot to each new receiver and then
// deltas, applying the rate rules of §8.4. The publishing methods are thread safe: when they
// are called from another thread they are queued to the emitter's thread. Run the emitter in
// a thread that is not the GUI or audio thread so clock.pong replies are fast (§7.2).
#pragma once

#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <atomic>
#include <memory>
#include <optional>

#include "vertexav/tempo.h"

class QUdpSocket;

namespace vertexav {

class MdnsResponder;
class WsConnection;
class WsServer;
class ObjectThrottle;

struct EmitterConfig {
    QString name = QStringLiteral("VertexAV Emitter");
    QString id;                // stable id (§5.5); generated if empty
    QString role = QStringLiteral("emitter");
    QStringList upstreamPath;  // bridges: ids from the primary source to the upstream emitter
    QList<int> profiles{0, 1, 2};
    QString security = QStringLiteral("open"); // "open" or "auth"
    QByteArray psk;                           // shared key for every client
    QHash<QString, QByteArray> keys;          // per client_id keys (§15.3)
    QHostAddress host = QHostAddress::AnyIPv4;
    quint16 port = kControlPort;
    quint16 udpPort = kRealtimePort;
    bool announce = true;
    int decks = 4;
    qint64 lookaheadUs = 150000;
    int maxReceivers = kMaxReceiversDefault;
    bool linkEnabled = false;
};

class Emitter : public QObject {
    Q_OBJECT
  public:
    explicit Emitter(const EmitterConfig& config, QObject* parent = nullptr);
    ~Emitter() override;

    // Call from the emitter's thread.
    bool start(QString* error = nullptr);
    void stop(const QString& reason = QStringLiteral("shutdown"));

    QString id() const { return m_id; }
    QString sessionId() const { return m_sessionId; }
    quint32 sessionKey() const { return m_sessionKey; }
    quint16 port() const { return m_cfg.port; }
    quint16 udpPort() const { return m_cfg.udpPort; }
    const EmitterConfig& config() const { return m_cfg; }
    qint64 now() const { return m_clock.now(); }
    int receiverCount() const { return m_receiverCount.load(); }

    // ---- publishing API (thread safe) ----
    void setTempo(const Tempo& tempo);
    void updateDeck(int deck, const QJsonObject& fields);
    void updateMix(const QJsonObject& fields);
    void updateIntent(const QJsonObject& fields);
    void trackLoaded(int deck, const QString& trackId, const QJsonObject& meta, bool structureAvailable,
            const QJsonValue& artwork = QJsonValue());
    void trackUnloaded(int deck);
    void trackStructure(const QJsonObject& structure);
    // Returns the event_id. at < 0 means "now".
    QString emitEvent(const QString& name, qint64 at = -1, int deck = 0, const QJsonObject& data = QJsonObject());
    void cancelEvent(const QString& eventId, const QString& reason);
    void trigger(const QString& name, const QString& state, std::optional<double> value = std::nullopt,
            int deck = 0, qint64 at = -1);
    // Serves data at http://host:port/artwork/<key> (§10.3), max 2 MB.
    void setArtwork(const QString& key, const QByteArray& data, const QByteArray& mimeType);

    QJsonObject snapshot() const; // emitter thread only

  signals:
    void receiverConnected(const QString& clientId, const QString& name);
    void receiverDisconnected(const QString& clientId, const QString& name);
    void receiverMessage(const QString& clientId, const QJsonObject& message);

  private:
    struct Session;
    template<typename F>
    bool post(F&& f);

    void onNewConnection(WsConnection* ws);
    void onText(Session* s, const QByteArray& text);
    void handleHello(Session* s, const QJsonObject& msg);
    void handleAuthResponse(Session* s, const QJsonObject& msg);
    void sendWelcome(Session* s, const QString& serverProof);
    void handleSessionMessage(Session* s, const QJsonObject& msg);
    void dropSession(Session* s);
    void fatal(Session* s, const QString& code, const QString& message);
    void sendTo(Session* s, QJsonObject msg);
    void sendSnapshot(Session* s);
    void publish(const QJsonObject& msg);
    void throttled(const QString& key, const QString& type, const QJsonObject& fields, int deck);
    void sendUdpTempo(qint64 now);
    void realtimeTick();
    void housekeeping();
    void onDatagrams();
    bool lockedOut(const QHostAddress& ip);
    void publishEvent(const QString& id, const QString& name, qint64 at, int deck, const QJsonObject& data);
    static QJsonObject emptyDeck(int deck);

    EmitterConfig m_cfg;
    QString m_id;
    QStringList m_path;
    QString m_origin;
    QString m_sessionId;
    quint32 m_sessionKey = 0;
    ShowClock m_clock;
    WsServer* m_server = nullptr;
    QUdpSocket* m_udp = nullptr;
    MdnsResponder* m_mdns = nullptr;
    QTimer m_rtTimer;
    QTimer m_hkTimer;
    QList<Session*> m_sessions;
    QHash<QString, int> m_unauth; // ip -> pending connections
    QHash<QString, QList<qint64>> m_authFailures;
    std::atomic<int> m_receiverCount{0};
    std::atomic<quint64> m_eventCounter{10000};

    // state
    Tempo m_tempo;
    qint64 m_phaseResetUntil = 0;
    qint64 m_lastUdpTempo = -1000000000;
    qint64 m_lastBeatAnnounced = -1;
    QMap<int, QJsonObject> m_decks;
    QJsonObject m_mix;
    QMap<int, QJsonObject> m_tracks;
    QHash<QString, QJsonObject> m_structures;
    QJsonObject m_intent;
    QHash<QString, QJsonObject> m_events;
    QHash<QString, std::shared_ptr<ObjectThrottle>> m_throttles;
    QHash<QString, QPair<QByteArray, QByteArray>> m_artwork;
    bool m_started = false;
};

} // namespace vertexav
