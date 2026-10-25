// Emitter-side mix analysis (§10.2): deck on air, transitions, cuts and gates.
// Port of tools/vertexav/mixlogic.py; both must behave the same.
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

namespace vertexav {

struct MixEvent {
    QString name;
    int deck = 0;
    QJsonObject data;
};

struct MixResult {
    QVector<MixEvent> events;
    QJsonObject mix; // mix.state fields that changed
    bool onAirChanged = false;
    int onAirOld = 0;
    int onAirNew = 0;
};

class MixAnalyzer {
  public:
    static constexpr double kTransitionStart = 0.15;
    static constexpr double kSilent = 0.05;
    static constexpr double kOnAirMargin = 0.02;
    static constexpr qint64 kGateFastUs = 50000;

    // audible: deck number -> 0..1. now and beatUs in microseconds.
    MixResult update(const QMap<int, double>& audible, qint64 now, double beatUs);
    int onAir() const { return m_onAir; }
    QJsonObject transitionJson() const;
    bool transitionActive() const { return m_active; }

  private:
    struct Gate {
        bool open = false;
        qint64 lastHigh = -1;
        bool gated = false;
    };
    void endTransition(MixResult& r, int finalDeck);

    int m_onAir = 0;
    bool m_active = false;
    QString m_kind;
    int m_from = 0;
    int m_to = 0;
    double m_progress = 0;
    qint64 m_trStart = -1;
    qint64 m_fromLowSince = -1;
    QMap<int, Gate> m_gates;
};

} // namespace vertexav
