#include "vertexav/mixanalyzer.h"

#include <QJsonValue>
#include <cmath>

namespace vertexav {

namespace {
QJsonValue deckOrNull(int d) {
    return d > 0 ? QJsonValue(d) : QJsonValue();
}
} // namespace

QJsonObject MixAnalyzer::transitionJson() const {
    QJsonObject t;
    t[QStringLiteral("active")] = m_active;
    t[QStringLiteral("kind")] = m_active ? QJsonValue(m_kind) : QJsonValue();
    t[QStringLiteral("from_deck")] = m_active ? deckOrNull(m_from) : QJsonValue();
    t[QStringLiteral("to_deck")] = m_active ? deckOrNull(m_to) : QJsonValue();
    t[QStringLiteral("progress")] = m_active ? QJsonValue(m_progress) : QJsonValue();
    return t;
}

void MixAnalyzer::endTransition(MixResult& r, int finalDeck) {
    QJsonObject data;
    data[QStringLiteral("deck")] = deckOrNull(finalDeck);
    r.events.append({QStringLiteral("transition.end"), finalDeck, data});
    m_active = false;
    m_kind.clear();
    m_from = m_to = 0;
    m_progress = 0;
    m_trStart = -1;
    m_fromLowSince = -1;
    r.mix[QStringLiteral("transition")] = transitionJson();
}

MixResult MixAnalyzer::update(const QMap<int, double>& audible, qint64 now, double beatUs) {
    MixResult r;
    // Gates (chops, crossfader cuts)
    for (auto it = audible.cbegin(); it != audible.cend(); ++it) {
        const int deck = it.key();
        const double a = it.value();
        auto g = m_gates.find(deck);
        if (g == m_gates.end()) {
            Gate ng;
            ng.open = a > kTransitionStart;
            ng.lastHigh = ng.open ? now : -1;
            m_gates.insert(deck, ng);
            continue;
        }
        if (a > kTransitionStart) {
            if (!g->open && g->gated) {
                QJsonObject d;
                d[QStringLiteral("state")] = QStringLiteral("open");
                r.events.append({QStringLiteral("deck.gate"), deck, d});
                g->gated = false;
            }
            g->open = true;
            g->lastHigh = now;
        } else if (a < kSilent && g->open) {
            if (g->lastHigh >= 0 && now - g->lastHigh <= kGateFastUs) {
                QJsonObject d;
                d[QStringLiteral("state")] = QStringLiteral("closed");
                r.events.append({QStringLiteral("deck.gate"), deck, d});
                g->gated = true;
            }
            g->open = false;
        }
    }

    // Transition start (blend) relative to the deck on air
    if (!m_active && m_onAir > 0 && audible.value(m_onAir, 0.0) > kSilent) {
        int to = 0;
        double best = -1;
        for (auto it = audible.cbegin(); it != audible.cend(); ++it) {
            if (it.key() != m_onAir && it.value() > kTransitionStart && it.value() > best) {
                best = it.value();
                to = it.key();
            }
        }
        if (to > 0) {
            m_active = true;
            m_kind = QStringLiteral("blend");
            m_from = m_onAir;
            m_to = to;
            m_progress = 0;
            m_trStart = now;
            QJsonObject d;
            d[QStringLiteral("from_deck")] = m_from;
            d[QStringLiteral("to_deck")] = m_to;
            r.events.append({QStringLiteral("transition.start"), to, d});
        }
    }

    // Deck on air with hysteresis
    if (!audible.isEmpty()) {
        int cand = 0;
        double best = -1;
        for (auto it = audible.cbegin(); it != audible.cend(); ++it) {
            if (it.value() > best) {
                best = it.value();
                cand = it.key();
            }
        }
        const double cur = m_onAir > 0 ? audible.value(m_onAir, 0.0) : -1.0;
        if (cand != m_onAir && best >= kSilent && best > cur + kOnAirMargin) {
            const int old = m_onAir;
            m_onAir = cand;
            r.mix[QStringLiteral("on_air_deck")] = cand;
            if (old > 0) {
                r.onAirChanged = true;
                r.onAirOld = old;
                r.onAirNew = cand;
                QJsonObject d;
                d[QStringLiteral("from_deck")] = old;
                d[QStringLiteral("to_deck")] = cand;
                r.events.append({QStringLiteral("on_air.change"), cand, d});
                const bool blendLong = m_active && m_kind == QLatin1String("blend") && m_trStart >= 0 &&
                        now - m_trStart >= beatUs / 2;
                if (!blendLong) {
                    m_active = true;
                    m_kind = QStringLiteral("cut");
                    m_from = old;
                    m_to = cand;
                    m_progress = 1.0;
                    m_trStart = now;
                    m_fromLowSince = -1;
                    r.events.append({QStringLiteral("mix.cut"), cand, d});
                }
            }
        }
    }

    // Progress and end
    if (m_active) {
        const double af = audible.value(m_from, 0.0);
        const double at = audible.value(m_to, 0.0);
        if (m_kind == QLatin1String("blend")) {
            m_progress = af + at > 0 ? std::round(at / (af + at) * 10000.0) / 10000.0 : 1.0;
        }
        if (at < kSilent && m_kind == QLatin1String("blend")) {
            endTransition(r, m_from); // aborted mix
        } else if (af < kSilent) {
            if (m_fromLowSince < 0) {
                m_fromLowSince = now;
            } else if (now - m_fromLowSince >= beatUs) {
                endTransition(r, m_to);
            }
        } else {
            m_fromLowSince = -1;
        }
        if (m_active) {
            r.mix[QStringLiteral("transition")] = transitionJson();
        }
    }
    return r;
}

} // namespace vertexav
