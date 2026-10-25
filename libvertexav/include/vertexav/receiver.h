// VertexAV Link receiver: connection with reconnection, authentication, clock sync over UDP,
// packet validation (§8.3), state (snapshot + deltas), events fired at their exact instant
// and a beat clock that emits beat() on every beat minus the receiver latency (§7.3).
#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <optional>

#include "vertexav/tempo.h"

class QUdpSocket;

namespace vertexav {

class WsConnection;

struct ReceiverConfig {
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = kControlPort;
    QString name = QStringLiteral("VertexAV Receiver");
    QString clientId; // stable id; generated if empty
    QString role = QStringLiteral("receiver");
    QString ownId; // bridges: own id for loop detection (§5.5)
    QList<int> profiles{0, 1, 2};
    QStringList topics; // empty = everything in the agreed profiles (up to 2)
    qint64 latencyUs = 0;
    QByteArray psk; // pairing key; when set, "open" emitters are refused (no downgrade)
    bool udp = true;
    bool reconnect = true;
};

struct SectionInfo {
    int index = -1;
    QString role;
    double energy = 0;
    double startBeat = 0;
    double endBeat = 0;
};

class Receiver : public QObject {
    Q_OBJECT
  public:
    explicit Receiver(const ReceiverConfig& config, QObject* parent = nullptr);
    ~Receiver() override;

    void start();
    void stop();
    void setLatencyUs(qint64 latencyUs);
    void setEndpoint(const QString& host, quint16 port);
    void send(const QJsonObject& message); // receiver.state, receiver.capabilities...

    const ReceiverConfig& config() const { return m_cfg; }
    bool isConnected() const { return m_open; }
    const QJsonObject& welcome() const { return m_welcome; }
    const ClockSync& clockSync() const { return m_sync; }
    const TempoFollower& follower() const { return m_follower; }
    qint64 localNow() const { return m_clock.now(); }
    std::optional<qint64> emitterNow();
    const QMap<int, QJsonObject>& decks() const { return m_decks; }
    const QJsonObject& mix() const { return m_mix; }
    const QJsonObject& intent() const { return m_intent; }
    const QMap<int, QJsonObject>& tracks() const { return m_tracks; }
    QJsonObject structure(const QString& trackId) const { return m_structures.value(trackId); }
    int onAirDeck() const;
    std::optional<SectionInfo> currentSection(int deck = 0);
    quint64 packetErrors() const { return m_packetErrors; }

  signals:
    void connected(const QJsonObject& welcome);
    void disconnected(const QString& reason);
    void statusMessage(const QString& text);
    void messageReceived(const QJsonObject& message);
    void snapshotReceived();
    void tempoChanged();
    // At the beat instant minus latency. change = first beat or after a jump.
    void beat(qint64 index, int beatInBar, double bpm, bool change);
    void eventFired(const QJsonObject& event);
    void triggerFired(const QJsonObject& trigger);
    void sectionChanged(int deck, const QString& role, double energy);
    void intentChanged(const QJsonObject& intent);
    void deckChanged(int deck);
    void mixChanged();
    void trackLoaded(int deck);

  private:
    void connectNow();
    void onOpened();
    void onText(const QByteArray& text);
    void onWsDisconnected();
    void fatal(const QString& code, const QString& message);
    void handleWelcome(const QJsonObject& msg);
    void apply(const QJsonObject& msg);
    void applyTempo(const QJsonObject& msg, bool fromSnapshot);
    bool fresh(const QString& key, const QJsonValue& t);
    void schedule(const QString& key, qint64 at, const QJsonObject& msg, bool isEvent);
    void sendPing();
    void onDatagrams();
    void armBeat();
    void onBeatTimer();
    void subscribe();
    void teardown(const QString& reason);

    ReceiverConfig m_cfg;
    ShowClock m_clock;
    ClockSync m_sync;
    TempoFollower m_follower;
    WsConnection* m_ws = nullptr;
    QUdpSocket* m_udp = nullptr;
    bool m_running = false;
    bool m_open = false;
    bool m_gotWelcome = false;
    bool m_subscribed = false;
    QByteArray m_serverNonce;
    QByteArray m_clientNonce;
    QByteArray m_macKey;
    QJsonObject m_welcome;
    QHash<QString, QString> m_securitySeen;
    std::optional<quint32> m_sessionKey;
    QHash<int, SeqWindow> m_windows;
    quint32 m_pingSeq = 0;
    quint32 m_pingId = 0;
    QHash<quint32, qint64> m_pings;
    int m_initialPings = 0;
    QTimer m_pingTimer;
    QTimer m_heartbeatTimer;
    QTimer m_beatTimer;
    QTimer m_reconnectTimer;
    double m_backoff = 0.5;
    qint64 m_lastRx = 0;
    quint64 m_packetErrors = 0;
    QHash<QString, QTimer*> m_scheduled;
    // state
    std::optional<Tempo> m_tempo;
    QMap<int, QJsonObject> m_decks;
    QMap<int, qint64> m_posT;
    QJsonObject m_mix;
    QMap<int, QJsonObject> m_tracks;
    QHash<QString, QJsonObject> m_structures;
    QJsonObject m_intent;
    QHash<QString, qint64> m_lastT;
    qint64 m_lastBeat = -1;
    int m_lastJumps = 0;
    qint64 m_armedBeat = -1;
};

} // namespace vertexav
