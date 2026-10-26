/*
  Q Light Controller Plus
  vertexavplugin.h

  Input plugin for VertexAV Link: beat (also as the QLC+ beat generator, so functions
  follow the DJ tempo without tapping), sections of the track on air, countdowns, energy,
  mood, palette colours, mix state and DJ triggers, as input channels.

  Licensed under the Apache License, Version 2.0
*/

#ifndef VERTEXAVPLUGIN_H
#define VERTEXAVPLUGIN_H

#include "qlcioplugin.h"
#include "vertexavbridge.h"

class QThread;

#define VERTEXAV_PARAM_HOST    "host"
#define VERTEXAV_PARAM_PORT    "port"
#define VERTEXAV_PARAM_CODE    "pairingCode"
#define VERTEXAV_PARAM_LATENCY "latencyMs"

class VertexAVPlugin final : public QLCIOPlugin
{
    Q_OBJECT
    Q_INTERFACES(QLCIOPlugin)
    Q_PLUGIN_METADATA(IID QLCIOPlugin_iid)

public:
    ~VertexAVPlugin() override;
    void init() override;
    QString name() const override;
    int capabilities() const override;
    QString pluginInfo() const override;

    bool openInput(quint32 input, quint32 universe) override;
    void closeInput(quint32 input, quint32 universe) override;
    QStringList inputs() override;
    QString inputInfo(quint32 input) override;

    void configure() override;
    bool canConfigure() const override;
    void setParameter(quint32 universe, quint32 line, Capability type, QString name, QVariant value) override;

    VertexAVSettings settings() const { return m_settings; }
    void applySettings(const VertexAVSettings &settings);
    QString status() const { return m_status; }
    bool isConnected() const { return m_connected; }
    QList<QStringList> discoveredEmitters() const { return m_emitters; }

signals:
    void startBridge(const VertexAVSettings &settings);
    void stopBridge();
    void statusUpdated();

private slots:
    void slotChannelValue(quint32 channel, uchar value, const QString &key);
    void slotStatus(const QString &text, bool connected);
    void slotEmitterFound(const QString &name, const QString &address, quint16 port, const QString &id);

private:
    void restart();
    void saveDefaults() const;
    void checkHostVersion();

    // Set when the running QLC+ has a different plugin interface than the one this
    // plugin was built against: the plugin then offers no input line.
    QString m_incompatible;

    quint32 m_universe = UINT_MAX;
    VertexAVSettings m_settings;
    QThread *m_thread = nullptr;
    VertexAVBridge *m_bridge = nullptr;
    QString m_status;
    bool m_connected = false;
    QList<QStringList> m_emitters; // name, address, port, id
};

#endif
