/*
  Q Light Controller Plus
  vertexavconfiguration.h

  Licensed under the Apache License, Version 2.0
*/

#ifndef VERTEXAVCONFIGURATION_H
#define VERTEXAVCONFIGURATION_H

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTreeWidget;
class VertexAVPlugin;

class VertexAVConfiguration : public QDialog
{
    Q_OBJECT

public:
    explicit VertexAVConfiguration(VertexAVPlugin *plugin, QWidget *parent = nullptr);

public slots:
    void accept() override;

private slots:
    void refresh();

private:
    VertexAVPlugin *m_plugin;
    QComboBox *m_host;
    QSpinBox *m_port;
    QLineEdit *m_code;
    QDoubleSpinBox *m_latency;
    QLabel *m_status;
    QTreeWidget *m_channels;
};

#endif
