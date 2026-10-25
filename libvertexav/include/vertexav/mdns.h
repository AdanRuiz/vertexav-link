// Minimal mDNS / DNS-SD (RFC 6762 / 6763) responder and browser for _vertexav._tcp (§5.1).
//
// Enough to announce one service and to discover others without depending on Bonjour or
// Avahi. Not a general purpose resolver.
#pragma once

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QMap>
#include <QNetworkAddressEntry>
#include <QObject>
#include <QTimer>

class QUdpSocket;

namespace vertexav {

struct MdnsService {
    QString instance;          // "Mixxx (Cabina 1)"
    QString type = QStringLiteral("_vertexav._tcp");
    QString host;              // target host name without ".local"
    quint16 port = 0;
    QList<QHostAddress> addresses;
    QMap<QString, QString> txt;
    QString fullName() const { return instance + QLatin1Char('.') + type + QStringLiteral(".local"); }
};

class MdnsResponder : public QObject {
    Q_OBJECT
  public:
    explicit MdnsResponder(QObject* parent = nullptr);
    ~MdnsResponder() override;
    // localOnly: announce only on the loopback interface with 127.0.0.1 (scope=local).
    bool start(const MdnsService& service, bool localOnly = false);
    void stop(); // sends goodbye packets (TTL 0)
    bool isActive() const { return m_active; }
    const MdnsService& service() const { return m_service; }

  private slots:
    void onReadyRead();

  private:
    QByteArray buildResponse(quint16 id, bool ptr, bool srvTxt, bool addr, quint32 ttlScale,
            const QHostAddress& querier, const QByteArray& questions = QByteArray(), int qdcount = 0) const;
    void announce(quint32 ttlScale);
    QList<QHostAddress> addressesFor(const QHostAddress& querier) const;

    QUdpSocket* m_socket = nullptr;
    MdnsService m_service;
    QList<QNetworkAddressEntry> m_entries;
    bool m_active = false;
    bool m_localOnly = false;
    QTimer m_announceTimer;
    int m_announceCount = 0;
};

class MdnsBrowser : public QObject {
    Q_OBJECT
  public:
    explicit MdnsBrowser(QObject* parent = nullptr);
    ~MdnsBrowser() override;
    bool start(const QString& type = QStringLiteral("_vertexav._tcp"));
    void stop();
    QList<MdnsService> services() const;

  signals:
    void serviceUpdated(const vertexav::MdnsService& service);
    void serviceRemoved(const QString& instance);

  private slots:
    void onReadyRead();
    void query();

  private:
    void maybeComplete(const QString& fullNameLower);

    struct Entry {
        MdnsService svc;
        QString target;
        bool hasSrv = false;
        bool hasTxt = false;
        bool reported = false;
        qint64 expires = 0;
        quint16 lastPort = 0;
        QMap<QString, QString> lastTxt;
    };
    QUdpSocket* m_socket = nullptr;
    QString m_type;
    QHash<QString, Entry> m_entries;            // key: full instance name, lower case
    QHash<QString, QList<QHostAddress>> m_hosts; // key: host name, lower case
    QTimer m_queryTimer;
    int m_queries = 0;
};

} // namespace vertexav

Q_DECLARE_METATYPE(vertexav::MdnsService)
