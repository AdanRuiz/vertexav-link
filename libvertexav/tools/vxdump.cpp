// vxdump: connects to a VertexAV emitter and prints messages, beats and fired events.
//   vxdump [--host H] [--port P] [--psk CODE] [--latency-ms N] [--discover] [--duration S] [--quiet]
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vertexav/vertexav.h"

using namespace vertexav;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({"host", "Emitter host", "host", "127.0.0.1"});
    p.addOption({"port", "Control port", "port", "9470"});
    p.addOption({"psk", "Pairing code (auth mode)", "code"});
    p.addOption({"latency-ms", "Receiver latency", "ms", "0"});
    p.addOption({"discover", "Find the emitter with DNS-SD"});
    p.addOption({"duration", "Exit after N seconds", "s", "0"});
    p.addOption({"quiet", "Only beats, events and status"});
    p.process(app);

    QTextStream out(stdout);
    ReceiverConfig cfg;
    cfg.name = QStringLiteral("vxdump");
    cfg.host = p.value("host");
    cfg.port = static_cast<quint16>(p.value("port").toUInt());
    cfg.latencyUs = static_cast<qint64>(p.value("latency-ms").toDouble() * 1000);
    if (p.isSet("psk")) {
        bool ok = false;
        cfg.psk = security::codeToPsk(p.value("psk"), &ok);
        if (!ok) {
            fprintf(stderr, "invalid pairing code\n");
            return 2;
        }
    }
    auto* rx = new Receiver(cfg, &app);
    const bool quiet = p.isSet("quiet");
    int beats = 0;
    int events = 0;
    QObject::connect(rx, &Receiver::statusMessage, [&](const QString& s) { out << "[status] " << s << Qt::endl; });
    QObject::connect(rx, &Receiver::messageReceived, [&](const QJsonObject& m) {
        const QString type = m.value("type").toString();
        if (!quiet && type != QLatin1String("heartbeat")) {
            out << "[msg] " << QJsonDocument(m).toJson(QJsonDocument::Compact).left(300) << Qt::endl;
        }
    });
    QList<double> beatErrorsMs;
    QObject::connect(rx, &Receiver::beat, [&](qint64 idx, int inBar, double bpm, bool change) {
        ++beats;
        // How far from the beat instant (minus latency) the signal was delivered.
        if (const auto now = rx->emitterNow()) {
            const qint64 target = rx->follower().timeOfBeat(idx, *now) - rx->config().latencyUs;
            beatErrorsMs.append((*now - target) / 1000.0);
        }
        out << "[beat] " << idx << " (" << inBar << ") " << QString::number(bpm, 'f', 2) << " BPM"
            << (change ? " change" : "") << Qt::endl;
    });
    QObject::connect(rx, &Receiver::eventFired, [&](const QJsonObject& e) {
        ++events;
        out << "[event] " << e.value("name").toString() << " "
            << QJsonDocument(e.value("data").toObject()).toJson(QJsonDocument::Compact) << Qt::endl;
    });
    QObject::connect(rx, &Receiver::triggerFired, [&](const QJsonObject& t) {
        out << "[trigger] " << t.value("name").toString() << " " << t.value("state").toString() << Qt::endl;
    });
    QObject::connect(rx, &Receiver::sectionChanged, [&](int deck, const QString& role, double energy) {
        out << "[section] deck " << deck << " " << role << " energy " << energy << Qt::endl;
    });

    auto startRx = [&] { rx->start(); };
    MdnsBrowser browser;
    if (p.isSet("discover")) {
        QObject::connect(&browser, &MdnsBrowser::serviceUpdated, [&](const MdnsService& s) {
            out << "[dns-sd] " << s.instance << " " << s.addresses.value(0).toString() << ":" << s.port
                << " id=" << s.txt.value("id") << " hops=" << s.txt.value("hops") << Qt::endl;
            if (!rx->isConnected()) {
                rx->setEndpoint(s.addresses.value(0).toString(), s.port);
                startRx();
            }
        });
        browser.start();
    } else {
        startRx();
    }
    const int duration = p.value("duration").toInt();
    if (duration > 0) {
        QTimer::singleShot(duration * 1000, &app, [&] {
            std::sort(beatErrorsMs.begin(), beatErrorsMs.end(), [](double a, double b) { return std::abs(a) < std::abs(b); });
            if (!beatErrorsMs.isEmpty()) {
                out << "[timing] beat signal error |median| " << std::abs(beatErrorsMs[beatErrorsMs.size() / 2])
                    << " ms, p95 " << std::abs(beatErrorsMs[static_cast<int>(beatErrorsMs.size() * 0.95)])
                    << " ms, max " << std::abs(beatErrorsMs.last()) << " ms" << Qt::endl;
            }
            out << "[summary] beats=" << beats << " events=" << events
                << " clock_samples=" << rx->clockSync().samples()
                << " rtt_min_us=" << rx->clockSync().rttMin() << " packet_errors=" << rx->packetErrors()
                << Qt::endl;
            rx->stop();
            app.quit();
        });
    }
    return app.exec();
}
