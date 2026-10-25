/*
  Q Light Controller Plus
  vertexavbridge.h

  Runs a libvertexav Receiver in its own thread (so clock synchronisation is not delayed
  by the GUI) and turns what it receives into QLC+ input channel values.

  Licensed under the Apache License, Version 2.0
*/

#ifndef VERTEXAVBRIDGE_H
#define VERTEXAVBRIDGE_H

#include <QJsonObject>
#include <QMap>
#include <QObject>

#include "vertexav/mdns.h"
#include "vertexav/receiver.h"

struct VertexAVSettings
{
    QString host;            // empty = find the emitter with DNS-SD
    quint16 port = 9470;
    QString pairingCode;     // empty = open mode
    double latencyMs = 0;    // delay of the DMX interface and fixtures
    QString clientId;
};

class VertexAVBridge : public QObject
{
    Q_OBJECT

public:
    explicit VertexAVBridge(QObject *parent = nullptr);
    ~VertexAVBridge() override;

    /** Maps a trigger name to its channel (see vertexavchannels.h) */
    static quint32 triggerChannel(const QString &name);

public slots:
    void start(const VertexAVSettings &settings);
    void stop();

signals:
    /** A channel changed. key is a readable name ("beat", "section.drop"...). */
    void channelValue(quint32 channel, uchar value, const QString &key);
    void statusChanged(const QString &text, bool connected);
    void emitterFound(const QString &name, const QString &address, quint16 port, const QString &id);

private slots:
    void onBeat(qint64 index, int beatInBar, double bpm, bool change);
    void onSection(int deck, const QString &role, double energy);
    void onIntent(const QJsonObject &intent);
    void onMix();
    void onDeck(int deck);
    void onEvent(const QJsonObject &event);
    void onTrigger(const QJsonObject &trigger);
    void onTempo();
    void onConnected();
    void onDisconnected(const QString &reason);
    void onService(const vertexav::MdnsService &service);

private:
    void set(quint32 channel, uchar value, const QString &key);
    void pulse(quint32 channel, const QString &key, uchar value = 255);
    void exclusive(quint32 base, const QStringList &names, const QString &prefix, const QString &active);
    void resetAll();

    vertexav::Receiver *m_receiver = nullptr;
    vertexav::MdnsBrowser *m_browser = nullptr;
    VertexAVSettings m_settings;
    QMap<quint32, uchar> m_values;
};

Q_DECLARE_METATYPE(VertexAVSettings)

#endif
