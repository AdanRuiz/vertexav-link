/*
  Q Light Controller Plus
  vertexavbridge.cpp

  Licensed under the Apache License, Version 2.0
*/

#include "vertexavbridge.h"

#include <QColor>
#include <QJsonArray>
#include <QTimer>
#include <QtMath>

#include "vertexav/security.h"
#include "vertexavchannels.h"

using namespace VertexAVChannels;

namespace
{
uchar toByte(double v01)
{
    return uchar(qBound(0, qRound(v01 * 255.0), 255));
}

// CRC-16/CCITT-FALSE, so custom trigger channels are stable across Qt versions.
quint16 crc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (char c : data)
    {
        crc ^= quint16(quint8(c)) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
    }
    return crc;
}
}

VertexAVBridge::VertexAVBridge(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<VertexAVSettings>();
}

VertexAVBridge::~VertexAVBridge()
{
    stop();
}

quint32 VertexAVBridge::triggerChannel(const QString &name)
{
    const int std = standardTriggers().indexOf(name);
    if (std >= 0)
        return TriggerBase + quint32(std);
    if (name.startsWith(QLatin1String("color.")))
    {
        const int idx = triggerColors().indexOf(name.mid(6));
        if (idx >= 0)
            return ColorBase + quint32(idx);
    }
    if (name.startsWith(QLatin1String("fx.")))
    {
        bool ok = false;
        const int n = name.mid(3).toInt(&ok);
        if (ok && n >= 1 && n <= FxCount)
            return FxBase + quint32(n - 1);
    }
    return CustomBase + crc16(name.toUtf8()) % CustomRange;
}

void VertexAVBridge::start(const VertexAVSettings &settings)
{
    stop();
    m_settings = settings;
    vertexav::ReceiverConfig cfg;
    cfg.name = QStringLiteral("QLC+ (VertexAV plugin)");
    cfg.clientId = settings.clientId;
    cfg.profiles = { 0, 1, 2 };
    cfg.latencyUs = qint64(settings.latencyMs * 1000.0);
    cfg.port = settings.port;
    if (!settings.pairingCode.trimmed().isEmpty())
    {
        bool ok = false;
        cfg.psk = vertexav::security::codeToPsk(settings.pairingCode, &ok);
        if (!ok)
        {
            emit statusChanged(tr("Invalid pairing code"), false);
            return;
        }
    }
    m_receiver = new vertexav::Receiver(cfg, this);
    connect(m_receiver, &vertexav::Receiver::beat, this, &VertexAVBridge::onBeat);
    connect(m_receiver, &vertexav::Receiver::sectionChanged, this, &VertexAVBridge::onSection);
    connect(m_receiver, &vertexav::Receiver::intentChanged, this, &VertexAVBridge::onIntent);
    connect(m_receiver, &vertexav::Receiver::mixChanged, this, &VertexAVBridge::onMix);
    connect(m_receiver, &vertexav::Receiver::deckChanged, this, &VertexAVBridge::onDeck);
    connect(m_receiver, &vertexav::Receiver::eventFired, this, &VertexAVBridge::onEvent);
    connect(m_receiver, &vertexav::Receiver::triggerFired, this, &VertexAVBridge::onTrigger);
    connect(m_receiver, &vertexav::Receiver::tempoChanged, this, &VertexAVBridge::onTempo);
    connect(m_receiver, &vertexav::Receiver::connected, this, &VertexAVBridge::onConnected);
    connect(m_receiver, &vertexav::Receiver::disconnected, this, &VertexAVBridge::onDisconnected);
    connect(m_receiver, &vertexav::Receiver::statusMessage, this,
            [this](const QString &text) { emit statusChanged(text, m_receiver && m_receiver->isConnected()); });

    m_browser = new vertexav::MdnsBrowser(this);
    connect(m_browser, &vertexav::MdnsBrowser::serviceUpdated, this, &VertexAVBridge::onService);
    m_browser->start();

    if (!settings.host.trimmed().isEmpty())
    {
        m_receiver->setEndpoint(settings.host.trimmed(), settings.port);
        m_receiver->start();
        emit statusChanged(tr("Connecting to %1:%2").arg(settings.host).arg(settings.port), false);
    }
    else
    {
        emit statusChanged(tr("Looking for VertexAV emitters on the network..."), false);
    }
}

void VertexAVBridge::stop()
{
    if (m_receiver)
    {
        m_receiver->stop();
        delete m_receiver;
        m_receiver = nullptr;
    }
    if (m_browser)
    {
        m_browser->stop();
        delete m_browser;
        m_browser = nullptr;
    }
    resetAll();
}

void VertexAVBridge::onService(const vertexav::MdnsService &service)
{
    const QString address = service.addresses.value(0).toString();
    emit emitterFound(service.instance, address, service.port, service.txt.value(QStringLiteral("id")));
    if (!m_receiver || !m_settings.host.trimmed().isEmpty() || m_receiver->isConnected())
        return;
    if (service.txt.value(QStringLiteral("hops")).toInt() > vertexav::kMaxHops)
        return;
    // Automatic choice (§5.5): the first announcement found. Pick one by hand to override.
    m_receiver->setEndpoint(address, service.port);
    m_receiver->start();
}

void VertexAVBridge::onConnected()
{
    const QJsonObject em = m_receiver->welcome().value(QStringLiteral("emitter")).toObject();
    emit statusChanged(tr("Connected to %1").arg(em.value(QStringLiteral("name")).toString()), true);
}

void VertexAVBridge::onDisconnected(const QString &reason)
{
    emit statusChanged(tr("Disconnected (%1)").arg(reason), false);
    resetAll();
}

void VertexAVBridge::set(quint32 channel, uchar value, const QString &key)
{
    auto it = m_values.find(channel);
    if (it != m_values.end() && it.value() == value)
        return;
    m_values[channel] = value;
    emit channelValue(channel, value, key);
}

void VertexAVBridge::pulse(quint32 channel, const QString &key, uchar value)
{
    m_values[channel] = value;
    emit channelValue(channel, value, key);
    QTimer::singleShot(PulseMs, this, [this, channel, key] { set(channel, 0, key); });
}

void VertexAVBridge::exclusive(quint32 base, const QStringList &names, const QString &prefix, const QString &active)
{
    for (int i = 0; i < names.size(); ++i)
        set(base + quint32(i), names[i] == active ? 255 : 0, prefix + names[i]);
}

void VertexAVBridge::resetAll()
{
    const auto values = m_values;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (it.value() != 0)
            set(it.key(), 0, QString());
}

void VertexAVBridge::onBeat(qint64 index, int beatInBar, double bpm, bool change)
{
    Q_UNUSED(index)
    Q_UNUSED(change)
    Q_UNUSED(bpm)
    // The key must be exactly "beat" for the QLC+ beat generator ("Plugin" mode).
    pulse(Beat, QStringLiteral("beat"));
    if (beatInBar == 1)
        pulse(Downbeat, QStringLiteral("downbeat"));
}

void VertexAVBridge::onTempo()
{
    if (!m_receiver || !m_receiver->follower().hasModel())
        return;
    const vertexav::Tempo &t = m_receiver->follower().model();
    set(Bpm, uchar(qBound(0, qRound(t.bpm - 50.0), 255)), QStringLiteral("tempo.bpm"));
    set(Playing, t.playing ? 255 : 0, QStringLiteral("tempo.playing"));
}

void VertexAVBridge::onSection(int deck, const QString &role, double energy)
{
    Q_UNUSED(deck)
    Q_UNUSED(energy)
    exclusive(SectionBase, sectionRoles(), QStringLiteral("section."), role);
}

void VertexAVBridge::onIntent(const QJsonObject &intent)
{
    if (intent.value(QStringLiteral("energy")).isDouble())
        set(Energy, toByte(intent.value(QStringLiteral("energy")).toDouble()), QStringLiteral("energy"));
    if (intent.value(QStringLiteral("strobe_ok")).isBool())
        set(StrobeOk, intent.value(QStringLiteral("strobe_ok")).toBool() ? 255 : 0, QStringLiteral("strobe_ok"));
    if (intent.value(QStringLiteral("intensity_hint")).isString())
        exclusive(IntensityBase, intensities(), QStringLiteral("intensity."),
                  intent.value(QStringLiteral("intensity_hint")).toString());
    if (intent.value(QStringLiteral("mood")).isString())
        exclusive(MoodBase, moods(), QStringLiteral("mood."), intent.value(QStringLiteral("mood")).toString());
    const QJsonArray palette = intent.value(QStringLiteral("palette")).toArray();
    for (int i = 0; i < PaletteColors; ++i)
    {
        QColor c;
        if (i < palette.size())
            c = QColor(palette[i].toString());
        if (!c.isValid())
            c = palette.isEmpty() ? QColor(Qt::black) : QColor(palette[0].toString());
        const quint32 base = PaletteBase + quint32(i * 3);
        const QString k = QStringLiteral("palette.%1.").arg(i + 1);
        set(base, uchar(c.red()), k + QLatin1Char('r'));
        set(base + 1, uchar(c.green()), k + QLatin1Char('g'));
        set(base + 2, uchar(c.blue()), k + QLatin1Char('b'));
    }
}

void VertexAVBridge::onMix()
{
    if (!m_receiver)
        return;
    const QJsonObject mix = m_receiver->mix();
    const QJsonObject tr = mix.value(QStringLiteral("transition")).toObject();
    const bool active = tr.value(QStringLiteral("active")).toBool();
    set(TransitionActive, active ? 255 : 0, QStringLiteral("transition.active"));
    set(TransitionProgress, active ? toByte(tr.value(QStringLiteral("progress")).toDouble()) : 0,
        QStringLiteral("transition.progress"));
    const int onAir = mix.value(QStringLiteral("on_air_deck")).toInt();
    for (int d = 1; d <= MaxDecks; ++d)
        set(OnAirBase + quint32(d - 1), d == onAir ? 255 : 0, QStringLiteral("on_air.%1").arg(d));
}

void VertexAVBridge::onDeck(int deck)
{
    if (!m_receiver || deck < 1 || deck > MaxDecks)
        return;
    const QJsonObject st = m_receiver->decks().value(deck);
    set(DeckAudibleBase + quint32(deck - 1), toByte(st.value(QStringLiteral("audible")).toDouble()),
        QStringLiteral("deck.%1.audible").arg(deck));
    set(DeckPlayingBase + quint32(deck - 1), st.value(QStringLiteral("playing")).toBool() ? 255 : 0,
        QStringLiteral("deck.%1.playing").arg(deck));
}

void VertexAVBridge::onEvent(const QJsonObject &event)
{
    const QString name = event.value(QStringLiteral("name")).toString();
    const QJsonObject data = event.value(QStringLiteral("data")).toObject();
    if (name == QLatin1String("countdown"))
    {
        const int n = data.value(QStringLiteral("beats_remaining")).toInt();
        const int idx = countdownBeats().indexOf(n);
        if (idx >= 0)
            pulse(CountdownBase + quint32(idx), QStringLiteral("countdown.%1").arg(n));
    }
    else if (name == QLatin1String("mix.cut"))
    {
        pulse(MixCut, QStringLiteral("mix.cut"));
    }
    else if (name == QLatin1String("on_air.change") && m_receiver)
    {
        if (const auto sec = m_receiver->currentSection(data.value(QStringLiteral("to_deck")).toInt()))
            exclusive(SectionBase, sectionRoles(), QStringLiteral("section."), sec->role);
    }
}

void VertexAVBridge::onTrigger(const QJsonObject &trigger)
{
    const QString name = trigger.value(QStringLiteral("name")).toString();
    if (name.isEmpty())
        return;
    const QString state = trigger.value(QStringLiteral("state")).toString();
    const QJsonValue v = trigger.value(QStringLiteral("value"));
    const uchar value = v.isDouble() ? qMax<uchar>(1, toByte(v.toDouble())) : 255;
    const quint32 ch = triggerChannel(name);
    if (state == QLatin1String("momentary"))
        pulse(ch, name, value);
    else if (state == QLatin1String("on"))
        set(ch, value, name);
    else if (state == QLatin1String("off"))
        set(ch, 0, name);
}
