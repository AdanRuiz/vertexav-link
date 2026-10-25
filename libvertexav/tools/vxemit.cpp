// vxemit: test emitter built on libvertexav. Plays a simple endless two-deck session so the
// C++ emitter can be checked with the Python inspector:
//   vxemit [--port 9470] [--udp-port 9471] [--security open|auth --psk CODE] [--no-announce]
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "vertexav/vertexav.h"

using namespace vertexav;

namespace {

struct Section {
    const char* role;
    int beats;
    double energy;
};
const Section kSections[] = {{"intro", 16, 0.3}, {"build", 16, 0.6}, {"drop", 32, 0.95},
        {"breakdown", 16, 0.4}, {"outro", 48, 0.35}};
constexpr int kTrackBeats = 128;
constexpr int kOutroStart = 80;

struct TrackDef {
    QString id;
    QString title;
    double bpm;
    QString key;
    QStringList palette;
    QString mood;
};

int sectionAt(double beat, int* start) {
    int s = 0;
    for (int i = 0; i < 5; ++i) {
        if (beat >= s && beat < s + kSections[i].beats) {
            *start = s;
            return i;
        }
        s += kSections[i].beats;
    }
    return -1;
}

int sectionStart(int idx) {
    int s = 0;
    for (int i = 0; i < idx; ++i) {
        s += kSections[i].beats;
    }
    return s;
}

QJsonObject structureFor(const TrackDef& t) {
    QJsonArray sections;
    QJsonArray energy;
    int s = 0;
    for (const auto& sec : kSections) {
        QJsonObject o;
        o["start_beat"] = s;
        o["end_beat"] = s + sec.beats;
        o["role"] = sec.role;
        o["energy"] = sec.energy;
        o["confidence"] = 0.9;
        sections.append(o);
        for (int i = 0; i < sec.beats; ++i) {
            energy.append(sec.energy);
        }
        s += sec.beats;
    }
    QJsonObject bg{{"type", "constant"}, {"bpm", t.bpm}, {"first_beat_s", 0.05}, {"beats_per_bar", 4},
            {"first_downbeat_beat", 0}};
    return QJsonObject{{"track_id", t.id},
            {"analysis", QJsonObject{{"source", "vxemit"}, {"version", "1.0"}, {"confidence", 0.9}}},
            {"beatgrid", bg}, {"sections", sections},
            {"energy_curve", QJsonObject{{"resolution", "beat"}, {"values", energy}}},
            {"markers", QJsonArray{QJsonObject{{"kind", "hotcue"}, {"index", 1}, {"position_s", 0.05}}}},
            {"mood", QJsonObject{{"mood", t.mood}, {"valence", 0.5}, {"arousal", 0.7}}}};
}

class Session : public QObject {
  public:
    explicit Session(Emitter* em)
            : m_em(em) {
        m_tracks = {{"vx:0001", "Prism", 126.0, "8A", {"#3A7BFF", "#6A00FF"}, "dark"},
                {"vx:0002", "Horizon", 126.0, "9A", {"#FFAA00", "#FF5E00", "#FFFFFF"}, "euphoric"}};
        connect(&m_timer, &QTimer::timeout, this, &Session::tick);
        m_timer.setTimerType(Qt::PreciseTimer);
    }

    void start() {
        m_tempo.bpm = 126.0;
        m_tempo.anchorT = m_em->now();
        m_tempo.anchorBeat = 0;
        m_tempo.sourceDeck = 1;
        m_tempo.playing = true;
        m_tempo.phaseReset = true;
        m_em->setTempo(m_tempo);
        m_tempo.phaseReset = false;
        load(1);
        m_offset[1] = 0;
        m_playing[1] = true;
        m_fader[1] = 1.0;
        m_em->updateDeck(1, {{"playing", true}, {"fader", 1.0}});
        m_em->event("track.start", -1, 1);
        m_timer.start(5);
    }

  private:
    void load(int deck) {
        const TrackDef& t = m_tracks[m_next++ % m_tracks.size()];
        m_track[deck] = t;
        QJsonObject meta{{"title", t.title}, {"artist", "libvertexav"}, {"genre", "Test"},
                {"duration_s", kTrackBeats * 60.0 / t.bpm}, {"bpm", t.bpm}, {"key", t.key},
                {"key_notation", "camelot"}};
        m_em->trackLoaded(deck, t.id, meta, true);
        m_em->trackStructure(structureFor(t));
        m_em->updateDeck(deck, {{"position_s", 0.05}, {"position_beat", 0.0}, {"rate", 1.0}});
    }

    double deckBeat(int deck, double mb) const { return mb - m_offset[deck]; }

    void tick() {
        const qint64 now = m_em->now();
        const double mb = m_tempo.beatAt(now);
        const int a = m_onAir;
        const int b = 3 - a;
        // Script: at the outro of A, start B on the next bar and blend with faders.
        if (!m_mixing && m_playing[a] && deckBeat(a, mb) >= kOutroStart) {
            load(b);
            m_mixStart = std::ceil(mb / 4.0) * 4.0;
            m_offset[b] = m_mixStart;
            m_mixing = true;
        }
        if (m_mixing) {
            const double rel = mb - m_mixStart;
            if (rel >= 0 && !m_playing[b]) {
                m_playing[b] = true;
                m_em->updateDeck(b, {{"playing", true}, {"position_beat", 0.0}, {"position_s", 0.05}});
                m_em->event("track.start", -1, b);
            }
            m_fader[b] = std::clamp(rel / 16.0, 0.0, 1.0);
            m_fader[a] = std::clamp(1.0 - (rel - 16.0) / 16.0, 0.0, 1.0);
            if (rel >= 36 && m_playing[a]) {
                m_playing[a] = false;
                m_em->updateDeck(a, {{"playing", false}});
                m_em->event("track.stop", -1, a, {{"kind", "pause"}});
                m_onAir = b;
                m_mixing = false;
            }
        }
        // Audible, mix analysis
        QMap<int, double> aud;
        for (int d = 1; d <= 2; ++d) {
            aud[d] = m_playing[d] ? m_fader[d] : 0.0;
            m_em->updateDeck(d, {{"fader", m_fader[d]}, {"audible", aud[d]}});
        }
        const MixResult r = m_mix.update(aud, now, m_tempo.beatUs());
        if (!r.mix.isEmpty()) {
            m_em->updateMix(r.mix);
        }
        for (const MixEvent& e : r.events) {
            m_em->event(e.name, -1, e.deck, e.data);
        }
        if (r.onAirChanged) {
            m_em->updateDeck(r.onAirOld, {{"on_air", false}});
            m_em->updateDeck(r.onAirNew, {{"on_air", true}});
            m_tempo = m_tempo.rebased(now);
            m_tempo.sourceDeck = r.onAirNew;
            m_em->setTempo(m_tempo);
        } else if (m_mix.onAir() > 0 && !m_onAirSent) {
            m_em->updateDeck(m_mix.onAir(), {{"on_air", true}});
            m_onAirSent = true;
        }
        scheduleEvents(now, mb);
        sections(now, mb);
    }

    // Announce section.change and countdowns of the deck on air lookahead_us in advance.
    void scheduleEvents(qint64 now, double mb) {
        const int d = m_mix.onAir();
        if (d <= 0 || !m_playing[d]) {
            return;
        }
        const double db = deckBeat(d, mb);
        for (int idx = 1; idx < 5; ++idx) {
            const int start = sectionStart(idx);
            QList<int> ns{0};
            if (QString::fromLatin1(kSections[idx].role) == QLatin1String("drop")) {
                ns << 16 << 8 << 4 << 2 << 1;
            }
            for (int n : ns) {
                const double beat = start - n;
                if (beat <= db) {
                    continue;
                }
                const qint64 at = m_tempo.timeOfBeat(beat + m_offset[d]);
                if (at - now > m_em->config().lookaheadUs) {
                    continue;
                }
                const QString key = QStringLiteral("%1/%2/%3/%4").arg(m_track[d].id).arg(m_offset[d]).arg(idx).arg(n);
                if (m_announced.contains(key)) {
                    continue;
                }
                m_announced.insert(key);
                if (n == 0) {
                    m_em->event("section.change", at, d,
                            {{"from", kSections[idx - 1].role}, {"to", kSections[idx].role},
                                    {"section_index", idx}, {"energy", kSections[idx].energy}});
                } else {
                    m_em->event("countdown", at, d,
                            {{"target", kSections[idx].role}, {"beats_remaining", n},
                                    {"target_at", QJsonValue(static_cast<qint64>(
                                                          m_tempo.timeOfBeat(start + m_offset[d])))}});
                }
            }
        }
    }

    void sections(qint64 now, double mb) {
        Q_UNUSED(now);
        const int d = m_mix.onAir();
        if (d <= 0 || !m_playing[d]) {
            return;
        }
        int start = 0;
        const int idx = sectionAt(deckBeat(d, mb), &start);
        if (idx == m_section[d]) {
            return;
        }
        m_section[d] = idx;
        if (idx < 0) {
            return;
        }
        const Section& s = kSections[idx];
        const QString role = QString::fromLatin1(s.role);
        QJsonArray palette;
        for (const QString& c : m_track[d].palette) {
            palette.append(c);
        }
        m_em->updateIntent({{"energy", s.energy}, {"mood", m_track[d].mood}, {"palette", palette},
                {"intensity_hint", s.energy > 0.9 ? "peak" : (s.energy > 0.5 ? "medium" : "low")},
                {"strobe_ok", role == QLatin1String("drop")}});
        if (role == QLatin1String("drop")) {
            m_em->trigger("strobe", "momentary", 1.0, d);
        }
    }

    Emitter* m_em;
    QTimer m_timer;
    Tempo m_tempo;
    MixAnalyzer m_mix;
    QList<TrackDef> m_tracks;
    TrackDef m_track[3];
    int m_next = 0;
    double m_offset[3] = {0, 0, 0};
    bool m_playing[3] = {false, false, false};
    double m_fader[3] = {0, 0, 0};
    int m_section[3] = {-1, -1, -1};
    int m_onAir = 1;
    bool m_mixing = false;
    bool m_onAirSent = false;
    double m_mixStart = 0;
    QSet<QString> m_announced;
};

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({"name", "Instance name", "name", "vxemit"});
    p.addOption({"host", "Listen address", "host", "0.0.0.0"});
    p.addOption({"port", "Control port", "port", "9470"});
    p.addOption({"udp-port", "Real-time port", "port", "9471"});
    p.addOption({"security", "open or auth", "mode", "open"});
    p.addOption({"psk", "Pairing code", "code"});
    p.addOption({"gen-psk", "Print a new pairing code and exit"});
    p.addOption({"no-announce", "Do not announce with DNS-SD"});
    p.addOption({"duration", "Exit after N seconds", "s", "0"});
    p.process(app);
    if (p.isSet("gen-psk")) {
        printf("%s\n", qPrintable(security::pskToCode(security::generatePsk())));
        return 0;
    }
    EmitterConfig cfg;
    cfg.name = p.value("name");
    cfg.host = QHostAddress(p.value("host"));
    cfg.port = static_cast<quint16>(p.value("port").toUInt());
    cfg.udpPort = static_cast<quint16>(p.value("udp-port").toUInt());
    cfg.security = p.value("security");
    cfg.announce = !p.isSet("no-announce");
    cfg.decks = 2;
    if (p.isSet("psk")) {
        bool ok = false;
        cfg.psk = security::codeToPsk(p.value("psk"), &ok);
        if (!ok) {
            fprintf(stderr, "invalid pairing code\n");
            return 2;
        }
    }
    Emitter em(cfg);
    QString err;
    if (!em.start(&err)) {
        fprintf(stderr, "%s\n", qPrintable(err));
        return 1;
    }
    printf("vxemit %s on ws://%s:%d/vertexav udp %d\n", qPrintable(em.id()), qPrintable(p.value("host")), em.port(),
            em.udpPort());
    fflush(stdout);
    QObject::connect(&em, &Emitter::receiverConnected, [](const QString& id, const QString& name) {
        printf("receiver connected: %s (%s)\n", qPrintable(name), qPrintable(id));
        fflush(stdout);
    });
    Session session(&em);
    session.start();
    const int duration = p.value("duration").toInt();
    if (duration > 0) {
        QTimer::singleShot(duration * 1000, &app, [&] {
            em.stop();
            QTimer::singleShot(100, &app, &QCoreApplication::quit);
        });
    }
    return app.exec();
}
