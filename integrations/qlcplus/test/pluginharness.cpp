// Loads the VertexAV QLC+ plugin with QPluginLoader (as QLC+ does), opens its input line,
// points it at an emitter and records the channel values for some seconds.
//   vertexav_plugin_harness [host] [port] [seconds] [latency-ms]
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMap>
#include <QPluginLoader>
#include <QTimer>
#include <algorithm>
#include <cstdio>

#include "qlcioplugin.h"

class Recorder : public QObject {
    Q_OBJECT
  public:
    QMap<QString, int> onCount;
    QMap<QString, int> lastValue;
    QList<qint64> beatTimes;
    QElapsedTimer clock;

  public slots:
    void slotValue(quint32 universe, quint32 input, quint32 channel, uchar value, const QString& key) {
        Q_UNUSED(universe);
        Q_UNUSED(input);
        const QString k = key.isEmpty() ? QStringLiteral("ch%1").arg(channel) : key;
        lastValue[k] = value;
        if (value > 0) {
            onCount[k] += 1;
        }
        if (k == QLatin1String("beat") && value > 0) {
            beatTimes.append(clock.nsecsElapsed() / 1000);
        }
        if (k.startsWith(QLatin1String("section.")) || k.startsWith(QLatin1String("countdown.")) ||
                k == QLatin1String("strobe") || k == QLatin1String("fog") || k.startsWith(QLatin1String("on_air"))) {
            printf("[%7.3f] ch %3u %-22s %3d\n", clock.nsecsElapsed() / 1e9, channel, qPrintable(k), value);
            fflush(stdout);
        }
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("vertexav-harness"));
    const QString host = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("127.0.0.1");
    const quint16 port = argc > 2 ? quint16(atoi(argv[2])) : 9470;
    const int seconds = argc > 3 ? atoi(argv[3]) : 20;
    const double latency = argc > 4 ? atof(argv[4]) : 0.0;

    QPluginLoader loader(QStringLiteral(VERTEXAV_PLUGIN_PATH));
    QObject* instance = loader.instance();
    QLCIOPlugin* plugin = qobject_cast<QLCIOPlugin*>(instance);
    if (!plugin) {
        fprintf(stderr, "cannot load plugin: %s\n", qPrintable(loader.errorString()));
        return 2;
    }
    plugin->init();
    printf("plugin '%s' capabilities=%d inputs=%s\n", qPrintable(plugin->name()), plugin->capabilities(),
            qPrintable(plugin->inputs().join(QLatin1Char(','))));
    if (!(plugin->capabilities() & QLCIOPlugin::Beats)) {
        fprintf(stderr, "plugin does not declare the Beats capability\n");
        return 3;
    }
    Recorder rec;
    rec.clock.start();
    QObject::connect(plugin, SIGNAL(valueChanged(quint32, quint32, quint32, uchar, const QString&)), &rec,
            SLOT(slotValue(quint32, quint32, quint32, uchar, const QString&)));
    plugin->openInput(0, 0);
    plugin->setParameter(0, 0, QLCIOPlugin::Input, QStringLiteral("host"), host);
    plugin->setParameter(0, 0, QLCIOPlugin::Input, QStringLiteral("port"), port);
    plugin->setParameter(0, 0, QLCIOPlugin::Input, QStringLiteral("latencyMs"), latency);

    int result = 0;
    QTimer::singleShot(seconds * 1000, &app, [&] {
        printf("\n== channels seen (times > 0) ==\n");
        for (auto it = rec.onCount.cbegin(); it != rec.onCount.cend(); ++it) {
            printf("  %-24s %5d  (last %d)\n", qPrintable(it.key()), it.value(), rec.lastValue.value(it.key()));
        }
        // Beat regularity: intervals should match the tempo with little jitter.
        QList<double> intervals;
        for (int i = 1; i < rec.beatTimes.size(); ++i) {
            intervals.append((rec.beatTimes[i] - rec.beatTimes[i - 1]) / 1000.0);
        }
        if (intervals.size() > 4) {
            std::sort(intervals.begin(), intervals.end());
            printf("beat interval ms: min %.2f median %.2f max %.2f (%d beats)\n", intervals.first(),
                    intervals[intervals.size() / 2], intervals.last(), int(rec.beatTimes.size()));
        }
        const bool ok = rec.onCount.value(QStringLiteral("beat")) > seconds && rec.onCount.value(QStringLiteral("tempo.playing")) > 0;
        bool sections = false;
        for (auto it = rec.onCount.cbegin(); it != rec.onCount.cend(); ++it) {
            sections |= it.key().startsWith(QLatin1String("section."));
        }
        printf("result: beats %s, sections %s\n", ok ? "OK" : "MISSING", sections ? "OK" : "MISSING");
        result = (ok && sections) ? 0 : 1;
        plugin->closeInput(0, 0);
        QTimer::singleShot(200, &app, &QCoreApplication::quit);
    });
    app.exec();
    delete instance;
    return result;
}

#include "pluginharness.moc"
