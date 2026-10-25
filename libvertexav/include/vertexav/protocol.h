// VertexAV Link 0.6 wire format: constants, binary UDP packets and sequence windows.
// Section references (§x.y) point to spec/vertexav-link-spec.md.
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace vertexav {

inline constexpr const char* kProtocol = "vertexav";
inline constexpr const char* kVersion = "0.6";
inline constexpr quint16 kControlPort = 9470;
inline constexpr quint16 kRealtimePort = 9471;
inline constexpr const char* kWsPath = "/vertexav";
inline constexpr const char* kServiceType = "_vertexav._tcp";
inline constexpr int kMaxHops = 4;
inline constexpr qint64 kMaxSafeInt = Q_INT64_C(9007199254740992); // 2^53 (§7.1)

// §15.7 connection limits
inline constexpr int kHelloTimeoutMs = 2000;
inline constexpr int kAuthResponseTimeoutMs = 3000;
inline constexpr int kWelcomeTimeoutMs = 5000;
inline constexpr int kMaxUnauthPerIp = 4;
inline constexpr int kMaxUnauthTotal = 32;
inline constexpr qint64 kMaxMsgPreAuth = 4 * 1024;
inline constexpr qint64 kMaxMsgReceiverToEmitter = 64 * 1024;
inline constexpr qint64 kMaxMsgEmitterToReceiver = 512 * 1024;
inline constexpr int kMaxJsonDepth = 32;
inline constexpr int kMaxReceiversDefault = 64;
inline constexpr int kHeartbeatIntervalMs = 1000; // §6.5
inline constexpr int kHeartbeatTimeoutMs = 3000;

// §8.3 binary format
inline constexpr int kHeaderSize = 24;
inline constexpr int kSigLen = 8;
inline constexpr quint16 kFlagSigned = 0x0001;
inline constexpr int kUdpRecvBuffer = 1500;

enum class Kind : quint8 {
    Tempo = 0x01,
    ClockPing = 0x02,
    ClockPong = 0x03,
    Features = 0x10,
    Audio = 0x20,
};

const char* kindName(quint8 kind);

enum class Stability : quint8 { Stable = 0, Unstable = 1, Freewheel = 2 };
QString stabilityName(Stability s);
Stability stabilityFromName(const QString& s);

struct Header {
    quint8 version = 0;
    quint8 kind = 0;
    quint16 flags = 0;
    quint32 seq = 0;
    quint32 session = 0;
    qint64 t = 0;
    bool isSigned() const { return flags & kFlagSigned; }
};

struct TempoPacket { // §9.2, 44 bytes after the header
    double bpm = 120.0;
    qint64 anchorT = 0;
    double anchorBeat = 0.0;
    qint64 nextBeatT = 0;
    qint64 nextDownbeatT = 0;
    quint8 beatsPerBar = 4;
    quint8 sourceDeck = 0;
    bool playing = false;
    bool phaseReset = false;
    Stability stability = Stability::Stable;
    bool linkAligned = false;
    double confidence = 1.0;

    QByteArray packBody() const;
    static bool unpackBody(const QByteArray& body, TempoPacket* out);
};

struct PingPacket { // §7.2
    qint64 t0 = 0;
    quint32 pingId = 0;
    QByteArray packBody() const;
    static bool unpackBody(const QByteArray& body, PingPacket* out);
};

struct PongPacket {
    qint64 t0 = 0;
    quint32 pingId = 0;
    qint64 t1 = 0;
    qint64 t2 = 0;
    QByteArray packBody() const;
    static bool unpackBody(const QByteArray& body, PongPacket* out);
};

QByteArray packPacket(Kind kind, quint32 seq, quint32 session, qint64 t, const QByteArray& body,
        const QByteArray& macKey = QByteArray());

struct Validation {
    bool ok = false;
    int step = 0; // §8.3 step that failed
    QString error;
    Header header;
    QByteArray body; // without header and signature
};

// Steps 1-5 of §8.3. audioSampleBytes: 2 for s16le, 4 for f32le.
Validation validateDatagram(const QByteArray& data, quint32 sessionKey, const QByteArray& macKey,
        bool requireSignature, int audioSampleBytes = 2);

// RFC 1982 serial arithmetic: signed distance a - b on 32 bits.
qint32 serialDiff(quint32 a, quint32 b);

// Acceptance windows for seq (§8.3).
class SeqWindow {
  public:
    static constexpr int kAhead = 256;
    static constexpr int kBehind = 64;
    static constexpr int kResyncRun = 8;

    bool accept(quint32 seq);
    void reset() { *this = SeqWindow(); }
    bool hasLast() const { return m_hasLast; }
    quint32 last() const { return m_last; }
    quint64 gaps() const { return m_gaps; }
    quint64 droppedDuplicates() const { return m_dup; }
    quint64 droppedBackwards() const { return m_back; }
    quint64 resyncs() const { return m_resyncs; }

  private:
    bool m_hasLast = false;
    quint32 m_last = 0;
    bool m_hasRun = false;
    quint32 m_runLast = 0;
    int m_runLen = 0;
    quint64 m_gaps = 0;
    quint64 m_dup = 0;
    quint64 m_back = 0;
    quint64 m_resyncs = 0;
};

// Cheap scan of bracket nesting outside strings (§15.7).
bool jsonDepthExceeds(const QByteArray& json, int limit = kMaxJsonDepth);

} // namespace vertexav
