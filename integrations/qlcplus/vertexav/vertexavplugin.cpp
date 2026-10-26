/*
  Q Light Controller Plus
  vertexavplugin.cpp

  Licensed under the Apache License, Version 2.0
*/

#include "vertexavplugin.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSettings>
#include <QThread>
#include <QUuid>
#include <QVersionNumber>

#include "vertexavchannels.h"
#include "vertexavconfiguration.h"

#define SETTINGS_GROUP "VertexAVPlugin"

// QLC+ 5.3.0 is the oldest supported version. The versions after it change the plugin
// interface (new RDM methods), so a plugin only loads safely in the QLC+ it was built for.
// CMake sets VERTEXAV_QLC_NEW_IFACE when the interface headers have the new RDM methods.
#ifndef VERTEXAV_QLC_NEW_IFACE
#define VERTEXAV_QLC_NEW_IFACE 0
#endif
// Normalized (trailing zeros dropped) like the parsed version, so 5.3.0 == 5.3.
static const QVersionNumber kMinQlcVersion = QVersionNumber(5, 3, 0).normalized();

VertexAVPlugin::~VertexAVPlugin()
{
    if (m_thread)
    {
        // Stop in the bridge's thread and wait, so sockets close before the thread ends.
        QMetaObject::invokeMethod(m_bridge, "stop", Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait(2000);
        delete m_thread;
        m_thread = nullptr;
    }
}

void VertexAVPlugin::init()
{
    qRegisterMetaType<VertexAVSettings>();
    QSettings s;
    s.beginGroup(SETTINGS_GROUP);
    m_settings.host = s.value(VERTEXAV_PARAM_HOST).toString();
    m_settings.port = quint16(s.value(VERTEXAV_PARAM_PORT, 9470).toUInt());
    m_settings.pairingCode = s.value(VERTEXAV_PARAM_CODE).toString();
    m_settings.latencyMs = s.value(VERTEXAV_PARAM_LATENCY, 0.0).toDouble();
    m_settings.clientId = s.value("clientId").toString();
    if (m_settings.clientId.isEmpty())
    {
        // Stable id so the emitter recognises this QLC+ installation (§6.2, §15.3)
        m_settings.clientId = QStringLiteral("qlcplus-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        s.setValue("clientId", m_settings.clientId);
    }
    s.endGroup();
    m_status = tr("Not active");
    checkHostVersion();
}

void VertexAVPlugin::checkHostVersion()
{
    // QLC+ 5 sets the application version (for example "5.3.0" or "5.3.1 GIT abc123").
    // QLC+ 4 leaves it empty; it shares the plugin interface of its own source tree.
    const QString text = QCoreApplication::applicationVersion();
    if (text.isEmpty())
        return;
    const QVersionNumber v = QVersionNumber::fromString(text).normalized();
    if (v.isNull())
        return;
    const bool newInterface = VERTEXAV_QLC_NEW_IFACE;
    if (v < kMinQlcVersion)
        m_incompatible = tr("QLC+ %1 is too old: the VertexAV Link plugin needs QLC+ 5.3.0 or newer.").arg(text);
    else if (!newInterface && v > kMinQlcVersion)
        m_incompatible = tr("This VertexAV Link plugin was built for QLC+ 5.3.0 and this is QLC+ %1. "
                            "Use the plugin built for this version.").arg(text);
    else if (newInterface && v == kMinQlcVersion)
        m_incompatible = tr("This VertexAV Link plugin was built for a newer QLC+ than %1. "
                            "Use the plugin built for QLC+ 5.3.0.").arg(text);
    if (!m_incompatible.isEmpty())
    {
        qWarning() << "[VertexAV]" << m_incompatible;
        m_status = m_incompatible;
    }
}

QString VertexAVPlugin::name() const
{
    return QStringLiteral("VertexAV Link");
}

int VertexAVPlugin::capabilities() const
{
    return QLCIOPlugin::Input | QLCIOPlugin::Beats;
}

QString VertexAVPlugin::pluginInfo() const
{
    QString str;
    str += QStringLiteral("<HTML><HEAD><TITLE>%1</TITLE></HEAD><BODY>").arg(name());
    str += QStringLiteral("<P><H3>%1</H3>").arg(name());
    str += tr("Receives musical information from DJ software (for example Mixxx) with the "
              "VertexAV Link protocol: beats with the exact timing, sections of the track "
              "(intro, build, drop...), countdowns, energy, mood, colours and DJ triggers.");
    str += QStringLiteral("</P><P>");
    str += tr("Select <b>Plugin</b> as the beat generator in the Input/Output settings so "
              "functions in Beats tempo follow the DJ without tapping.");
    str += QStringLiteral("</P>");
    if (!m_incompatible.isEmpty())
        str += QStringLiteral("<P><b>%1</b></P>").arg(m_incompatible.toHtmlEscaped());
    return str;
}

bool VertexAVPlugin::openInput(quint32 input, quint32 universe)
{
    if (input != 0 || !m_incompatible.isEmpty())
        return false;
    m_universe = universe;
    addToMap(universe, input, Input);
    if (!m_thread)
    {
        m_thread = new QThread();
        m_thread->setObjectName(QStringLiteral("VertexAV"));
        m_bridge = new VertexAVBridge();
        m_bridge->moveToThread(m_thread);
        connect(m_thread, &QThread::finished, m_bridge, &QObject::deleteLater);
        connect(this, &VertexAVPlugin::startBridge, m_bridge, &VertexAVBridge::start);
        connect(this, &VertexAVPlugin::stopBridge, m_bridge, &VertexAVBridge::stop);
        connect(m_bridge, &VertexAVBridge::channelValue, this, &VertexAVPlugin::slotChannelValue);
        connect(m_bridge, &VertexAVBridge::statusChanged, this, &VertexAVPlugin::slotStatus);
        connect(m_bridge, &VertexAVBridge::emitterFound, this, &VertexAVPlugin::slotEmitterFound);
        m_thread->start();
    }
    restart();
    return true;
}

void VertexAVPlugin::closeInput(quint32 input, quint32 universe)
{
    if (input != 0)
        return;
    emit stopBridge();
    removeFromMap(input, universe, Input);
    m_universe = UINT_MAX;
    m_connected = false;
    m_status = tr("Not active");
    emit statusUpdated();
}

QStringList VertexAVPlugin::inputs()
{
    if (!m_incompatible.isEmpty())
        return QStringList();
    return QStringList() << QStringLiteral("VertexAV Link");
}

QString VertexAVPlugin::inputInfo(quint32 input)
{
    QString str;
    if (input != QLCIOPlugin::invalidLine())
        str += QStringLiteral("<H3>%1</H3>").arg(inputs().value(int(input)));
    str += QStringLiteral("<P>%1</P>").arg(m_status.toHtmlEscaped());
    str += QStringLiteral("</BODY></HTML>");
    return str;
}

void VertexAVPlugin::restart()
{
    if (m_universe != UINT_MAX && m_thread)
        emit startBridge(m_settings);
}

void VertexAVPlugin::saveDefaults() const
{
    QSettings s;
    s.beginGroup(SETTINGS_GROUP);
    s.setValue(VERTEXAV_PARAM_HOST, m_settings.host);
    s.setValue(VERTEXAV_PARAM_PORT, m_settings.port);
    s.setValue(VERTEXAV_PARAM_CODE, m_settings.pairingCode);
    s.setValue(VERTEXAV_PARAM_LATENCY, m_settings.latencyMs);
    s.endGroup();
}

void VertexAVPlugin::applySettings(const VertexAVSettings &settings)
{
    // Stored in the project through setParameter, and as defaults for new projects.
    if (m_universe != UINT_MAX)
    {
        QLCIOPlugin::setParameter(m_universe, 0, Input, VERTEXAV_PARAM_HOST, settings.host);
        QLCIOPlugin::setParameter(m_universe, 0, Input, VERTEXAV_PARAM_PORT, settings.port);
        QLCIOPlugin::setParameter(m_universe, 0, Input, VERTEXAV_PARAM_CODE, settings.pairingCode);
        QLCIOPlugin::setParameter(m_universe, 0, Input, VERTEXAV_PARAM_LATENCY, settings.latencyMs);
    }
    const QString clientId = m_settings.clientId;
    m_settings = settings;
    m_settings.clientId = clientId;
    saveDefaults();
    restart();
}

void VertexAVPlugin::configure()
{
    VertexAVConfiguration conf(this);
    conf.exec();
}

bool VertexAVPlugin::canConfigure() const
{
    return true;
}

void VertexAVPlugin::setParameter(quint32 universe, quint32 line, Capability type, QString name, QVariant value)
{
    // Called when a project is loaded, after the input line was opened.
    bool changed = true;
    if (name == QLatin1String(VERTEXAV_PARAM_HOST))
        m_settings.host = value.toString();
    else if (name == QLatin1String(VERTEXAV_PARAM_PORT))
        m_settings.port = quint16(value.toUInt());
    else if (name == QLatin1String(VERTEXAV_PARAM_CODE))
        m_settings.pairingCode = value.toString();
    else if (name == QLatin1String(VERTEXAV_PARAM_LATENCY))
        m_settings.latencyMs = value.toDouble();
    else
        changed = false;
    QLCIOPlugin::setParameter(universe, line, type, name, value);
    if (changed)
        restart();
}

void VertexAVPlugin::slotChannelValue(quint32 channel, uchar value, const QString &key)
{
    if (m_universe != UINT_MAX)
        emit valueChanged(m_universe, 0, channel, value, key);
}

void VertexAVPlugin::slotStatus(const QString &text, bool connected)
{
    m_status = text;
    m_connected = connected;
    emit statusUpdated();
}

void VertexAVPlugin::slotEmitterFound(const QString &name, const QString &address, quint16 port, const QString &id)
{
    for (const QStringList &e : std::as_const(m_emitters))
        if (e.value(3) == id && e.value(1) == address)
            return;
    m_emitters.append(QStringList() << name << address << QString::number(port) << id);
    emit statusUpdated();
}
