#include "vertexav/tempo.h"

#include <QJsonValue>
#include <algorithm>
#include <cmath>

namespace vertexav {

// ------------------------------------------------------------------ ClockSync
void ClockSync::add(qint64 t0, qint64 t1, qint64 t2, qint64 t3) {
    const qint64 rtt = (t3 - t0) - (t2 - t1);
    const double off = ((t1 - t0) + (t2 - t3)) / 2.0;
    m_samples.append({rtt, off});
    if (m_samples.size() > kWindow) {
        m_samples.removeFirst();
    }
    ++m_total;
    const qint64 rmin = rttMin();
    const double limit = std::max(1.5 * rmin, static_cast<double>(rmin + kRttFloorUs));
    QVector<double> good;
    for (const auto& s : m_samples) {
        if (s.rtt <= limit) {
            good.append(s.offset);
        }
    }
    std::sort(good.begin(), good.end());
    const int n = good.size();
    m_target = (n % 2) ? good[n / 2] : (good[n / 2 - 1] + good[n / 2]) / 2.0;
    if (!m_hasOffset || !synced()) {
        m_offset = m_target;
        m_hasOffset = true;
        m_lastLocal = t3;
    }
    if (rmin > 20000) {
        if (m_badSince < 0) {
            m_badSince = t3;
        }
    } else {
        m_badSince = -1;
    }
}

qint64 ClockSync::rttMin() const {
    qint64 m = -1;
    for (const auto& s : m_samples) {
        if (m < 0 || s.rtt < m) {
            m = s.rtt;
        }
    }
    return m;
}

qint64 ClockSync::toEmitter(qint64 localT) {
    if (m_lastLocal >= 0) {
        const qint64 elapsed = std::max<qint64>(0, localT - m_lastLocal);
        const double step = elapsed * kMaxSlewUsPerS / 1e6;
        const double diff = m_target - m_offset;
        if (std::abs(diff) <= step) {
            m_offset = m_target;
        } else {
            m_offset += diff > 0 ? step : -step;
        }
    }
    m_lastLocal = localT;
    return static_cast<qint64>(localT + m_offset);
}

// ------------------------------------------------------------------ Tempo
qint64 Tempo::timeOfBeat(double beat) const {
    return static_cast<qint64>(std::llround(anchorT + (beat - anchorBeat) * 60e6 / bpm));
}

qint64 Tempo::nextBeatIndex(qint64 t) const {
    return static_cast<qint64>(std::floor(beatAt(t) + 1e-9)) + 1;
}

qint64 Tempo::nextBeatT(qint64 t) const {
    return timeOfBeat(static_cast<double>(nextBeatIndex(t)));
}

double Tempo::nextDownbeatIndex(qint64 t) const {
    const int bpb = std::max(1, beatsPerBar);
    const double m = std::floor((beatAt(t) - barOffset) / bpb + 1e-9) + 1;
    return barOffset + m * bpb;
}

qint64 Tempo::nextDownbeatT(qint64 t) const {
    return timeOfBeat(nextDownbeatIndex(t));
}

int Tempo::beatInBar(qint64 beatIndex) const {
    const int bpb = std::max(1, beatsPerBar);
    qint64 r = static_cast<qint64>(std::llround(beatIndex - barOffset)) % bpb;
    if (r < 0) {
        r += bpb;
    }
    return static_cast<int>(r) + 1;
}

Tempo Tempo::rebased(qint64 t) const {
    Tempo r = *this;
    r.anchorBeat = beatAt(t);
    r.anchorT = t;
    return r;
}

QJsonObject Tempo::toJson() const {
    QJsonObject o;
    o[QStringLiteral("bpm")] = bpm;
    o[QStringLiteral("anchor_t")] = QJsonValue(static_cast<qint64>(anchorT));
    o[QStringLiteral("anchor_beat")] = anchorBeat;
    o[QStringLiteral("beats_per_bar")] = beatsPerBar;
    o[QStringLiteral("bar_offset")] = barOffset;
    o[QStringLiteral("source_deck")] = sourceDeck > 0 ? QJsonValue(sourceDeck) : QJsonValue();
    o[QStringLiteral("playing")] = playing;
    o[QStringLiteral("confidence")] = confidence;
    o[QStringLiteral("stability")] = stabilityName(stability);
    o[QStringLiteral("phase_reset")] = phaseReset;
    o[QStringLiteral("link_aligned")] = linkAligned ? QJsonValue(*linkAligned) : QJsonValue();
    return o;
}

Tempo Tempo::fromJson(const QJsonObject& o, const Tempo& base) {
    Tempo t = base;
    if (o.contains(QStringLiteral("bpm"))) {
        t.bpm = o.value(QStringLiteral("bpm")).toDouble(t.bpm);
    }
    if (o.contains(QStringLiteral("anchor_t"))) {
        t.anchorT = static_cast<qint64>(o.value(QStringLiteral("anchor_t")).toDouble());
    }
    if (o.contains(QStringLiteral("anchor_beat"))) {
        t.anchorBeat = o.value(QStringLiteral("anchor_beat")).toDouble();
    }
    if (o.contains(QStringLiteral("beats_per_bar"))) {
        t.beatsPerBar = o.value(QStringLiteral("beats_per_bar")).toInt(4);
    }
    if (o.contains(QStringLiteral("bar_offset"))) {
        t.barOffset = o.value(QStringLiteral("bar_offset")).toDouble();
    }
    if (o.contains(QStringLiteral("source_deck"))) {
        t.sourceDeck = o.value(QStringLiteral("source_deck")).toInt(0);
    }
    if (o.contains(QStringLiteral("playing"))) {
        t.playing = o.value(QStringLiteral("playing")).toBool();
    }
    if (o.contains(QStringLiteral("confidence"))) {
        t.confidence = o.value(QStringLiteral("confidence")).toDouble(1.0);
    }
    if (o.contains(QStringLiteral("stability"))) {
        t.stability = stabilityFromName(o.value(QStringLiteral("stability")).toString());
    }
    t.phaseReset = o.value(QStringLiteral("phase_reset")).toBool(false);
    if (o.contains(QStringLiteral("link_aligned"))) {
        const QJsonValue v = o.value(QStringLiteral("link_aligned"));
        t.linkAligned = v.isBool() ? std::optional<bool>(v.toBool()) : std::nullopt;
    }
    return t;
}

TempoPacket Tempo::toPacket(qint64 now, bool phaseResetFlag) const {
    TempoPacket p;
    p.bpm = bpm;
    p.anchorT = anchorT;
    p.anchorBeat = anchorBeat;
    p.nextBeatT = nextBeatT(now);
    p.nextDownbeatT = nextDownbeatT(now);
    p.beatsPerBar = static_cast<quint8>(beatsPerBar);
    p.sourceDeck = static_cast<quint8>(std::max(0, sourceDeck));
    p.playing = playing;
    p.phaseReset = phaseResetFlag;
    p.stability = stability;
    p.linkAligned = linkAligned.value_or(false);
    p.confidence = confidence;
    return p;
}

// ------------------------------------------------------------------ TempoFollower
double TempoFollower::corrAt(qint64 t) const {
    if (m_corr == 0.0 || !m_has) {
        return 0.0;
    }
    const double elapsedBeats = std::max(0.0, (t - m_corrT) / m_model.beatUs());
    const double budget = elapsedBeats * kMaxCorrPerBeat;
    if (std::abs(m_corr) <= budget) {
        return 0.0;
    }
    return m_corr - std::copysign(budget, m_corr);
}

double TempoFollower::beatAt(qint64 t) const {
    return m_has ? m_model.beatAt(t) - corrAt(t) : 0.0;
}

qint64 TempoFollower::timeOfBeat(qint64 beatIndex, qint64 now) const {
    return m_model.timeOfBeat(static_cast<double>(beatIndex) + corrAt(now));
}

bool TempoFollower::beatsEnabled(qint64 t) const {
    if (!m_has || !m_model.playing) {
        return false;
    }
    if (m_unstable && beatAt(t) - m_unstableSince > kUnstableMaxBeats) {
        return false;
    }
    return true;
}

TempoFollower::Result TempoFollower::update(const Tempo& t, qint64 now) {
    QByteArray key;
    key.append(reinterpret_cast<const char*>(&t.bpm), sizeof t.bpm);
    key.append(reinterpret_cast<const char*>(&t.anchorT), sizeof t.anchorT);
    key.append(reinterpret_cast<const char*>(&t.anchorBeat), sizeof t.anchorBeat);
    key.append(static_cast<char>(t.stability));
    key.append(static_cast<char>(t.playing));
    key.append(static_cast<char>(t.beatsPerBar));
    key.append(reinterpret_cast<const char*>(&t.barOffset), sizeof t.barOffset);
    if (key == m_lastKey) { // also repeated phase_reset flags for the same anchor
        return Result::Same;
    }
    m_lastKey = key;
    if (t.stability == Stability::Unstable) {
        // Freeze the phase: keep following the last stable model.
        if (!m_unstable) {
            m_unstable = true;
            m_unstableSince = m_has ? beatAt(now) : 0.0;
        }
        if (!m_has) {
            m_model = t;
            m_has = true;
        }
        return Result::Frozen;
    }
    m_unstable = false;
    if (!m_has) {
        m_model = t;
        m_model.phaseReset = false;
        m_has = true;
        return Result::Init;
    }
    const double err = t.beatAt(now) - beatAt(now);
    if (t.phaseReset || std::abs(err) > kJumpThreshold || !t.playing) {
        m_model = t;
        m_model.phaseReset = false;
        m_corr = 0;
        if (t.phaseReset || std::abs(err) > kJumpThreshold) {
            ++m_jumps;
            return Result::Jump;
        }
        return Result::Stop;
    }
    m_model = t;
    m_model.phaseReset = false;
    m_corr = err;
    m_corrT = now;
    return Result::Smooth;
}

} // namespace vertexav
