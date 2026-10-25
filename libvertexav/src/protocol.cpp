#include "vertexav/protocol.h"

#include <QtEndian>
#include <cstring>

#include "vertexav/security.h"

namespace vertexav {

namespace {

template<typename T>
void put(QByteArray& b, T v) {
    T le = qToLittleEndian(v);
    b.append(reinterpret_cast<const char*>(&le), sizeof(T));
}

void putDouble(QByteArray& b, double v) {
    quint64 bits;
    std::memcpy(&bits, &v, sizeof bits);
    put<quint64>(b, bits);
}

template<typename T>
T get(const QByteArray& b, int offset) {
    T v;
    std::memcpy(&v, b.constData() + offset, sizeof(T));
    return qFromLittleEndian(v);
}

double getDouble(const QByteArray& b, int offset) {
    quint64 bits = get<quint64>(b, offset);
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

constexpr int kTempoBody = 44;
constexpr int kPingBody = 12;
constexpr int kPongBody = 28;

} // namespace

const char* kindName(quint8 kind) {
    switch (kind) {
    case 0x01:
        return "tempo";
    case 0x02:
        return "clock.ping";
    case 0x03:
        return "clock.pong";
    case 0x10:
        return "features";
    case 0x20:
        return "audio";
    default:
        return "unknown";
    }
}

QString stabilityName(Stability s) {
    switch (s) {
    case Stability::Unstable:
        return QStringLiteral("unstable");
    case Stability::Freewheel:
        return QStringLiteral("freewheel");
    default:
        return QStringLiteral("stable");
    }
}

Stability stabilityFromName(const QString& s) {
    if (s == QLatin1String("unstable")) {
        return Stability::Unstable;
    }
    if (s == QLatin1String("freewheel")) {
        return Stability::Freewheel;
    }
    return Stability::Stable;
}

QByteArray TempoPacket::packBody() const {
    QByteArray b;
    b.reserve(kTempoBody);
    putDouble(b, bpm);
    put<qint64>(b, anchorT);
    putDouble(b, anchorBeat);
    put<qint64>(b, nextBeatT);
    put<qint64>(b, nextDownbeatT);
    put<quint8>(b, beatsPerBar);
    put<quint8>(b, sourceDeck);
    quint8 flags = (playing ? 1 : 0) | (phaseReset ? 2 : 0) |
            (static_cast<quint8>(stability) << 2) | (linkAligned ? 16 : 0);
    put<quint8>(b, flags);
    int conf = qBound(0, qRound(confidence * 255.0), 255);
    put<quint8>(b, static_cast<quint8>(conf));
    return b;
}

bool TempoPacket::unpackBody(const QByteArray& body, TempoPacket* out) {
    if (body.size() != kTempoBody) {
        return false;
    }
    out->bpm = getDouble(body, 0);
    out->anchorT = get<qint64>(body, 8);
    out->anchorBeat = getDouble(body, 16);
    out->nextBeatT = get<qint64>(body, 24);
    out->nextDownbeatT = get<qint64>(body, 32);
    out->beatsPerBar = static_cast<quint8>(body[40]);
    out->sourceDeck = static_cast<quint8>(body[41]);
    const quint8 flags = static_cast<quint8>(body[42]);
    const int stab = (flags >> 2) & 3;
    if (stab > 2) {
        return false;
    }
    out->playing = flags & 1;
    out->phaseReset = flags & 2;
    out->stability = static_cast<Stability>(stab);
    out->linkAligned = flags & 16;
    out->confidence = static_cast<quint8>(body[43]) / 255.0;
    return true;
}

QByteArray PingPacket::packBody() const {
    QByteArray b;
    put<qint64>(b, t0);
    put<quint32>(b, pingId);
    return b;
}

bool PingPacket::unpackBody(const QByteArray& body, PingPacket* out) {
    if (body.size() != kPingBody) {
        return false;
    }
    out->t0 = get<qint64>(body, 0);
    out->pingId = get<quint32>(body, 8);
    return true;
}

QByteArray PongPacket::packBody() const {
    QByteArray b;
    put<qint64>(b, t0);
    put<quint32>(b, pingId);
    put<qint64>(b, t1);
    put<qint64>(b, t2);
    return b;
}

bool PongPacket::unpackBody(const QByteArray& body, PongPacket* out) {
    if (body.size() != kPongBody) {
        return false;
    }
    out->t0 = get<qint64>(body, 0);
    out->pingId = get<quint32>(body, 8);
    out->t1 = get<qint64>(body, 12);
    out->t2 = get<qint64>(body, 20);
    return true;
}

QByteArray packPacket(Kind kind, quint32 seq, quint32 session, qint64 t, const QByteArray& body,
        const QByteArray& macKey) {
    QByteArray b;
    b.reserve(kHeaderSize + body.size() + kSigLen);
    b.append("VXAV", 4);
    put<quint8>(b, 0);
    put<quint8>(b, static_cast<quint8>(kind));
    put<quint16>(b, macKey.isEmpty() ? 0 : kFlagSigned);
    put<quint32>(b, seq);
    put<quint32>(b, session);
    put<qint64>(b, t);
    b.append(body);
    if (!macKey.isEmpty()) {
        b.append(security::sign(macKey, b));
    }
    return b;
}

namespace {

int expectedLength(quint8 kind, const QByteArray& body, int audioSampleBytes) {
    switch (kind) {
    case 0x01:
        return kHeaderSize + kTempoBody;
    case 0x02:
        return kHeaderSize + kPingBody;
    case 0x03:
        return kHeaderSize + kPongBody;
    case 0x10:
        return body.size() < 2 ? -1 : 40 + 4 * static_cast<quint8>(body[1]);
    case 0x20: {
        if (body.size() < 16) {
            return -1;
        }
        const int channels = static_cast<quint8>(body[1]);
        const int frames = get<quint16>(body, 2);
        return 40 + frames * channels * audioSampleBytes;
    }
    default:
        return -2;
    }
}

Validation fail(int step, const QString& msg) {
    Validation v;
    v.step = step;
    v.error = msg;
    return v;
}

} // namespace

Validation validateDatagram(const QByteArray& data, quint32 sessionKey, const QByteArray& macKey,
        bool requireSignature, int audioSampleBytes) {
    const int minLen = kHeaderSize + (requireSignature ? kSigLen : 0);
    if (data.size() < minLen) {
        return fail(1, QStringLiteral("length %1 < %2").arg(data.size()).arg(minLen));
    }
    Header h;
    if (std::memcmp(data.constData(), "VXAV", 4) != 0) {
        return fail(2, QStringLiteral("bad magic"));
    }
    h.version = static_cast<quint8>(data[4]);
    h.kind = static_cast<quint8>(data[5]);
    h.flags = get<quint16>(data, 6);
    h.seq = get<quint32>(data, 8);
    h.session = get<quint32>(data, 12);
    h.t = get<qint64>(data, 16);
    if (h.version != 0) {
        return fail(2, QStringLiteral("unsupported binary version %1").arg(h.version));
    }
    if (h.session != sessionKey) {
        return fail(3, QStringLiteral("unknown session %1").arg(h.session));
    }
    QByteArray payload = data;
    if (requireSignature) {
        if (!h.isSigned()) {
            return fail(4, QStringLiteral("missing signature"));
        }
        const QByteArray signedPart = data.left(data.size() - kSigLen);
        if (macKey.isEmpty() || !security::verify(macKey, signedPart, data.right(kSigLen))) {
            return fail(4, QStringLiteral("bad signature"));
        }
        payload = signedPart;
    } else if (h.isSigned() && data.size() >= kHeaderSize + kSigLen) {
        payload = data.left(data.size() - kSigLen);
    }
    if (h.flags & ~kFlagSigned) {
        return fail(5, QStringLiteral("reserved flags set"));
    }
    const QByteArray body = payload.mid(kHeaderSize);
    const int exp = expectedLength(h.kind, body, audioSampleBytes);
    if (exp == -2) {
        return fail(5, QStringLiteral("unknown kind %1").arg(h.kind));
    }
    if (payload.size() != exp) {
        return fail(5,
                QStringLiteral("length %1 != %2 for %3")
                        .arg(payload.size())
                        .arg(exp)
                        .arg(QLatin1String(kindName(h.kind))));
    }
    Validation v;
    v.ok = true;
    v.header = h;
    v.body = body;
    return v;
}

qint32 serialDiff(quint32 a, quint32 b) {
    return static_cast<qint32>(a - b);
}

bool SeqWindow::accept(quint32 seq) {
    if (!m_hasLast) {
        m_hasLast = true;
        m_last = seq;
        return true;
    }
    const qint32 d = serialDiff(seq, m_last);
    if (d >= 1 && d <= kAhead) {
        m_gaps += static_cast<quint64>(d - 1);
        m_last = seq;
        m_hasRun = false;
        m_runLen = 0;
        return true;
    }
    if (d >= -kBehind && d <= 0) {
        ++m_dup;
        return false;
    }
    if (d < -kBehind) {
        ++m_back; // never resync backwards within a session
        return false;
    }
    if (m_hasRun && serialDiff(seq, m_runLast) == 1) {
        ++m_runLen;
    } else {
        m_runLen = 1;
    }
    m_hasRun = true;
    m_runLast = seq;
    if (m_runLen >= kResyncRun) {
        m_last = seq;
        m_hasRun = false;
        m_runLen = 0;
        ++m_resyncs;
        return true;
    }
    return false;
}

bool jsonDepthExceeds(const QByteArray& json, int limit) {
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (const char ch : json) {
        if (inStr) {
            if (esc) {
                esc = false;
            } else if (ch == '\\') {
                esc = true;
            } else if (ch == '"') {
                inStr = false;
            }
        } else if (ch == '"') {
            inStr = true;
        } else if (ch == '[' || ch == '{') {
            if (++depth > limit) {
                return true;
            }
        } else if (ch == ']' || ch == '}') {
            --depth;
        }
    }
    return false;
}

} // namespace vertexav
