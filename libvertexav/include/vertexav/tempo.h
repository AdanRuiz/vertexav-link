// Show clock (§7.1), clock synchronisation (§7.2), tempo model (§9.1) and tempo follower (§9.4).
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QVector>
#include <optional>

#include "vertexav/protocol.h"

namespace vertexav {

// Monotonic microsecond clock that starts at 0 when created.
class ShowClock {
  public:
    ShowClock() { m_timer.start(); }
    qint64 now() const { return m_timer.nsecsElapsed() / 1000; }

  private:
    QElapsedTimer m_timer;
};

// NTP-like filter: window of 32 samples, rtt_min filter, median offset, slewed corrections.
class ClockSync {
  public:
    static constexpr int kWindow = 32;
    static constexpr int kInitial = 16;
    static constexpr double kMaxSlewUsPerS = 1000.0;
    static constexpr qint64 kRttFloorUs = 200;

    void add(qint64 t0, qint64 t1, qint64 t2, qint64 t3);
    bool synced() const { return m_total >= kInitial; }
    bool hasOffset() const { return m_hasOffset; }
    qint64 rttMin() const;
    int samples() const { return m_total; }
    double offset() const { return m_offset; }
    bool networkUnfit(qint64 localNow) const { return m_badSince >= 0 && localNow - m_badSince > 10000000; }

    qint64 toEmitter(qint64 localT); // applies slew
    qint64 toLocal(qint64 emitterT) const { return static_cast<qint64>(emitterT - m_offset); }

  private:
    struct Sample {
        qint64 rtt;
        double offset;
    };
    QVector<Sample> m_samples;
    int m_total = 0;
    bool m_hasOffset = false;
    double m_offset = 0;
    double m_target = 0;
    qint64 m_lastLocal = -1;
    qint64 m_badSince = -1;
};

struct Tempo {
    double bpm = 120.0;
    qint64 anchorT = 0;
    double anchorBeat = 0.0;
    int beatsPerBar = 4;
    double barOffset = 0.0;
    int sourceDeck = 0; // 0 = none
    bool playing = false;
    double confidence = 1.0;
    Stability stability = Stability::Stable;
    bool phaseReset = false;
    std::optional<bool> linkAligned;

    double beatUs() const { return bpm > 0 ? 60e6 / bpm : 1e18; }
    double beatAt(qint64 t) const { return anchorBeat + (t - anchorT) / 1e6 * bpm / 60.0; }
    qint64 timeOfBeat(double beat) const;
    qint64 nextBeatIndex(qint64 t) const;
    qint64 nextBeatT(qint64 t) const;
    double nextDownbeatIndex(qint64 t) const;
    qint64 nextDownbeatT(qint64 t) const;
    int beatInBar(qint64 beatIndex) const; // 1-based
    Tempo rebased(qint64 t) const;

    QJsonObject toJson() const;
    static Tempo fromJson(const QJsonObject& o, const Tempo& base);
    static Tempo fromJson(const QJsonObject& o) { return fromJson(o, Tempo()); }
    TempoPacket toPacket(qint64 now, bool phaseResetFlag) const;
};

// Receiver-side smoothing rules of §9.4 (works in emitter show time).
class TempoFollower {
  public:
    static constexpr double kMaxCorrPerBeat = 0.02;
    static constexpr double kJumpThreshold = 0.25;
    static constexpr double kUnstableMaxBeats = 8.0;

    enum class Result { Init, Same, Jump, Smooth, Frozen, Stop };

    Result update(const Tempo& t, qint64 now);
    bool hasModel() const { return m_has; }
    const Tempo& model() const { return m_model; }
    double beatAt(qint64 t) const;
    // Emitter instant of an integer beat on the smoothed timeline.
    qint64 timeOfBeat(qint64 beatIndex, qint64 now) const;
    bool beatsEnabled(qint64 t) const;
    int jumps() const { return m_jumps; }
    void reset() { *this = TempoFollower(); }

  private:
    double corrAt(qint64 t) const;

    bool m_has = false;
    Tempo m_model;
    double m_corr = 0;
    qint64 m_corrT = 0;
    bool m_unstable = false;
    double m_unstableSince = 0;
    int m_jumps = 0;
    QByteArray m_lastKey;
};

} // namespace vertexav
