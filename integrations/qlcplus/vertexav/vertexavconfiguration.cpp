/*
  Q Light Controller Plus
  vertexavconfiguration.cpp

  Licensed under the Apache License, Version 2.0
*/

#include "vertexavconfiguration.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "vertexavchannels.h"
#include "vertexavplugin.h"

using namespace VertexAVChannels;

namespace
{
void addChannel(QTreeWidget *tree, quint32 ch, const QString &name, const QString &desc)
{
    auto *item = new QTreeWidgetItem(tree);
    item->setText(0, QString::number(ch));
    item->setText(1, name);
    item->setText(2, desc);
}
}

VertexAVConfiguration::VertexAVConfiguration(VertexAVPlugin *plugin, QWidget *parent)
    : QDialog(parent)
    , m_plugin(plugin)
{
    setWindowTitle(tr("VertexAV Link configuration"));
    resize(640, 560);
    const VertexAVSettings s = plugin->settings();

    m_host = new QComboBox(this);
    m_host->setEditable(true);
    m_host->setToolTip(tr("Leave empty to connect to the first emitter found on the network"));
    m_host->addItem(QString());
    m_host->setCurrentText(s.host);
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(s.port);
    m_code = new QLineEdit(s.pairingCode, this);
    m_code->setPlaceholderText(tr("Only if the emitter uses 'auth' mode (52 characters)"));
    m_latency = new QDoubleSpinBox(this);
    m_latency->setRange(0, 1000);
    m_latency->setDecimals(1);
    m_latency->setSuffix(QStringLiteral(" ms"));
    m_latency->setValue(s.latencyMs);
    m_latency->setToolTip(tr("Delay of the DMX interface and fixtures: beats and events are sent this much earlier"));
    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    auto *form = new QFormLayout();
    form->addRow(tr("Emitter address"), m_host);
    form->addRow(tr("Port"), m_port);
    form->addRow(tr("Pairing code"), m_code);
    form->addRow(tr("Output latency"), m_latency);
    form->addRow(tr("Status"), m_status);

    m_channels = new QTreeWidget(this);
    m_channels->setHeaderLabels(QStringList() << tr("Channel") << tr("Name") << tr("Meaning"));
    m_channels->setRootIsDecorated(false);
    addChannel(m_channels, Beat, QStringLiteral("beat"), tr("Pulse on every beat (also the QLC+ beat generator)"));
    addChannel(m_channels, Downbeat, QStringLiteral("downbeat"), tr("Pulse on the first beat of each bar"));
    addChannel(m_channels, Bpm, QStringLiteral("tempo.bpm"), tr("BPM - 50"));
    addChannel(m_channels, Playing, QStringLiteral("tempo.playing"), tr("Music with tempo is playing"));
    addChannel(m_channels, Energy, QStringLiteral("energy"), tr("Energy of the music"));
    addChannel(m_channels, TransitionProgress, QStringLiteral("transition.progress"), tr("Mix progress"));
    addChannel(m_channels, TransitionActive, QStringLiteral("transition.active"), tr("A mix is in progress"));
    addChannel(m_channels, MixCut, QStringLiteral("mix.cut"), tr("Pulse on a fast cut between decks"));
    for (int i = 0; i < sectionRoles().size(); ++i)
        addChannel(m_channels, SectionBase + quint32(i), QStringLiteral("section.") + sectionRoles()[i],
                   tr("On while the track on air is in this section"));
    for (int i = 0; i < countdownBeats().size(); ++i)
        addChannel(m_channels, CountdownBase + quint32(i), QStringLiteral("countdown.%1").arg(countdownBeats()[i]),
                   tr("Pulse %1 beats before a drop").arg(countdownBeats()[i]));
    addChannel(m_channels, StrobeOk, QStringLiteral("strobe_ok"), tr("Good moment for aggressive effects"));
    for (int i = 0; i < intensities().size(); ++i)
        addChannel(m_channels, IntensityBase + quint32(i), QStringLiteral("intensity.") + intensities()[i], QString());
    for (int i = 0; i < moods().size(); ++i)
        addChannel(m_channels, MoodBase + quint32(i), QStringLiteral("mood.") + moods()[i], QString());
    for (int i = 0; i < PaletteColors; ++i)
        addChannel(m_channels, PaletteBase + quint32(i * 3),
                   QStringLiteral("palette.%1.r/g/b").arg(i + 1), tr("Suggested colour %1 (3 channels)").arg(i + 1));
    for (int d = 1; d <= MaxDecks; ++d)
    {
        addChannel(m_channels, DeckAudibleBase + quint32(d - 1), QStringLiteral("deck.%1.audible").arg(d), QString());
        addChannel(m_channels, DeckPlayingBase + quint32(d - 1), QStringLiteral("deck.%1.playing").arg(d), QString());
        addChannel(m_channels, OnAirBase + quint32(d - 1), QStringLiteral("on_air.%1").arg(d), QString());
    }
    for (int i = 0; i < standardTriggers().size(); ++i)
        addChannel(m_channels, TriggerBase + quint32(i), standardTriggers()[i], tr("DJ trigger"));
    for (int i = 0; i < triggerColors().size(); ++i)
        addChannel(m_channels, ColorBase + quint32(i), QStringLiteral("color.") + triggerColors()[i], tr("DJ trigger"));
    addChannel(m_channels, FxBase, QStringLiteral("fx.1 … fx.16"), tr("Channels %1-%2").arg(FxBase).arg(FxBase + FxCount - 1));
    addChannel(m_channels, CustomBase, QStringLiteral("custom.*"), tr("Other triggers: use the auto-detect wizard"));
    m_channels->header()->setSectionResizeMode(QHeaderView::ResizeToContents);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &VertexAVConfiguration::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &VertexAVConfiguration::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(new QLabel(tr("Input channels:"), this));
    layout->addWidget(m_channels, 1);
    layout->addWidget(buttons);

    connect(plugin, &VertexAVPlugin::statusUpdated, this, &VertexAVConfiguration::refresh);
    refresh();
}

void VertexAVConfiguration::refresh()
{
    m_status->setText(m_plugin->status());
    for (const QStringList &e : m_plugin->discoveredEmitters())
    {
        const QString label = QStringLiteral("%1 (%2:%3)").arg(e.value(0), e.value(1), e.value(2));
        if (m_host->findData(e.value(1)) < 0)
            m_host->addItem(label, e.value(1));
    }
}

void VertexAVConfiguration::accept()
{
    VertexAVSettings s = m_plugin->settings();
    const int idx = m_host->findText(m_host->currentText());
    s.host = (idx > 0 && m_host->itemData(idx).isValid()) ? m_host->itemData(idx).toString()
                                                          : m_host->currentText().trimmed();
    s.port = quint16(m_port->value());
    s.pairingCode = m_code->text().trimmed();
    s.latencyMs = m_latency->value();
    m_plugin->applySettings(s);
    QDialog::accept();
}
