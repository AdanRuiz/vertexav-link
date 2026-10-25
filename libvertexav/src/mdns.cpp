#include "vertexav/mdns.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QtEndian>

namespace vertexav {

namespace {

const QHostAddress kGroup(QStringLiteral("224.0.0.251"));
constexpr quint16 kPort = 5353;
constexpr quint16 kTypeA = 1;
constexpr quint16 kTypePtr = 12;
constexpr quint16 kTypeTxt = 16;
constexpr quint16 kTypeSrv = 33;
constexpr quint16 kTypeAny = 255;
constexpr quint16 kClassIn = 1;
constexpr quint16 kCacheFlush = 0x8000;
constexpr quint32 kTtlHost = 120;
constexpr quint32 kTtlOther = 4500;

void put16(QByteArray& b, quint16 v) {
    char d[2];
    qToBigEndian<quint16>(v, reinterpret_cast<uchar*>(d));
    b.append(d, 2);
}

void put32(QByteArray& b, quint32 v) {
    char d[4];
    qToBigEndian<quint32>(v, reinterpret_cast<uchar*>(d));
    b.append(d, 4);
}

quint16 get16(const QByteArray& b, int pos) {
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + pos));
}

quint32 get32(const QByteArray& b, int pos) {
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + pos));
}

// Names as label lists: the instance label may contain dots or spaces.
QByteArray encodeName(const QStringList& labels) {
    QByteArray out;
    for (const QString& l : labels) {
        QByteArray u = l.toUtf8().left(63);
        out.append(static_cast<char>(u.size()));
        out.append(u);
    }
    out.append('\0');
    return out;
}

QStringList splitType(const QString& dotted) {
    return dotted.split(QLatin1Char('.'));
}

bool readName(const QByteArray& msg, int& pos, QStringList* labels) {
    int p = pos;
    bool jumped = false;
    int guard = 0;
    while (true) {
        if (p >= msg.size() || ++guard > 128) {
            return false;
        }
        const quint8 len = static_cast<quint8>(msg[p]);
        if (len == 0) {
            ++p;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= msg.size()) {
                return false;
            }
            const int target = ((len & 0x3F) << 8) | static_cast<quint8>(msg[p + 1]);
            if (!jumped) {
                pos = p + 2;
            }
            jumped = true;
            p = target;
            continue;
        }
        if (p + 1 + len > msg.size()) {
            return false;
        }
        labels->append(QString::fromUtf8(msg.mid(p + 1, len)));
        p += 1 + len;
    }
    if (!jumped) {
        pos = p;
    }
    return true;
}

QString joinLower(const QStringList& labels) {
    return labels.join(QLatin1Char('.')).toLower();
}

void writeRecord(QByteArray& out, const QStringList& name, quint16 type, bool unique, quint32 ttl,
        const QByteArray& rdata) {
    out += encodeName(name);
    put16(out, type);
    put16(out, kClassIn | (unique ? kCacheFlush : 0));
    put32(out, ttl);
    put16(out, static_cast<quint16>(rdata.size()));
    out += rdata;
}

QUdpSocket* openSocket(QObject* parent, bool localOnly, bool* sharedPort) {
    auto* s = new QUdpSocket(parent);
    *sharedPort = s->bind(QHostAddress(QHostAddress::AnyIPv4), kPort,
            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!*sharedPort && !s->bind(QHostAddress(QHostAddress::AnyIPv4), quint16(0))) {
        delete s;
        return nullptr;
    }
    s->setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    s->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    if (*sharedPort) {
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            const auto flags = iface.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)) {
                continue;
            }
            const bool loop = flags & QNetworkInterface::IsLoopBack;
            if (localOnly != loop || (!loop && !(flags & QNetworkInterface::CanMulticast))) {
                continue;
            }
            s->joinMulticastGroup(kGroup, iface);
        }
    }
    return s;
}

} // namespace

// ============================================================================ responder

MdnsResponder::MdnsResponder(QObject* parent)
        : QObject(parent) {
    connect(&m_announceTimer, &QTimer::timeout, this, [this] {
        announce(1);
        if (++m_announceCount >= 3) {
            m_announceTimer.stop();
        }
    });
}

MdnsResponder::~MdnsResponder() {
    stop();
}

bool MdnsResponder::start(const MdnsService& service, bool localOnly) {
    stop();
    m_service = service;
    m_localOnly = localOnly;
    bool shared = false;
    m_socket = openSocket(this, localOnly, &shared);
    if (!m_socket || !shared) {
        delete m_socket;
        m_socket = nullptr;
        return false; // a responder must listen on 5353
    }
    m_entries.clear();
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const bool loop = iface.flags() & QNetworkInterface::IsLoopBack;
        if (!(iface.flags() & QNetworkInterface::IsUp) || loop != localOnly) {
            continue;
        }
        for (const QNetworkAddressEntry& e : iface.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            if (!m_service.addresses.isEmpty() && !m_service.addresses.contains(e.ip())) {
                continue;
            }
            if (e.ip().isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16)) {
                continue;
            }
            m_entries.append(e);
        }
    }
    if (m_entries.isEmpty()) {
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &MdnsResponder::onReadyRead);
    m_active = true;
    announce(1);
    m_announceCount = 0;
    m_announceTimer.start(1000);
    return true;
}

void MdnsResponder::stop() {
    if (!m_active) {
        return;
    }
    m_announceTimer.stop();
    announce(0); // goodbye
    m_socket->flush();
    m_active = false;
    m_socket->close();
    m_socket->deleteLater();
    m_socket = nullptr;
}

QList<QHostAddress> MdnsResponder::addressesFor(const QHostAddress& querier) const {
    QList<QHostAddress> out;
    for (const QNetworkAddressEntry& e : m_entries) {
        if (!querier.isNull() && querier.isInSubnet(e.ip(), e.prefixLength())) {
            out.append(e.ip());
        }
    }
    if (out.isEmpty()) {
        for (const QNetworkAddressEntry& e : m_entries) {
            out.append(e.ip());
        }
    }
    return out;
}

QByteArray MdnsResponder::buildResponse(quint16 id, bool ptr, bool srvTxt, bool addr, quint32 ttlScale,
        const QHostAddress& querier, const QByteArray& questions, int qdcount) const {
    const QStringList type = splitType(m_service.type) << QStringLiteral("local");
    const QStringList full = QStringList{m_service.instance} + type;
    const QStringList host{m_service.host, QStringLiteral("local")};
    QByteArray records;
    int count = 0;
    if (ptr) {
        writeRecord(records, type, kTypePtr, false, kTtlOther * ttlScale, encodeName(full));
        ++count;
    }
    if (srvTxt) {
        QByteArray srv;
        put16(srv, 0);
        put16(srv, 0);
        put16(srv, m_service.port);
        srv += encodeName(host);
        writeRecord(records, full, kTypeSrv, true, kTtlHost * ttlScale, srv);
        QByteArray txt;
        for (auto it = m_service.txt.cbegin(); it != m_service.txt.cend(); ++it) {
            const QByteArray kv = (it.key() + QLatin1Char('=') + it.value()).toUtf8().left(255);
            txt.append(static_cast<char>(kv.size()));
            txt.append(kv);
        }
        if (txt.isEmpty()) {
            txt.append('\0');
        }
        writeRecord(records, full, kTypeTxt, true, kTtlOther * ttlScale, txt);
        count += 2;
    }
    if (addr) {
        for (const QHostAddress& a : addressesFor(querier)) {
            QByteArray rd;
            put32(rd, a.toIPv4Address());
            writeRecord(records, host, kTypeA, true, kTtlHost * ttlScale, rd);
            ++count;
        }
    }
    QByteArray out;
    put16(out, id);
    put16(out, 0x8400); // response, authoritative
    put16(out, static_cast<quint16>(qdcount));
    put16(out, static_cast<quint16>(count));
    put16(out, 0);
    put16(out, 0);
    out += questions;
    out += records;
    return out;
}

void MdnsResponder::announce(quint32 ttlScale) {
    if (!m_socket) {
        return;
    }
    const QByteArray pkt = buildResponse(0, true, true, true, ttlScale, QHostAddress());
    m_socket->writeDatagram(pkt, kGroup, kPort);
}

void MdnsResponder::onReadyRead() {
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram(9000);
        const QByteArray msg = dg.data();
        if (msg.size() < 12) {
            continue;
        }
        const quint16 id = get16(msg, 0);
        const quint16 flags = get16(msg, 2);
        if (flags & 0x8000) {
            continue; // a response, not a query
        }
        const int qd = get16(msg, 4);
        int pos = 12;
        bool ptr = false, srvTxt = false, addr = false;
        const QString typeName = joinLower(splitType(m_service.type) << QStringLiteral("local"));
        const QString fullName = m_service.fullName().toLower();
        const QString hostName = (m_service.host + QStringLiteral(".local")).toLower();
        const int qStart = pos;
        for (int i = 0; i < qd; ++i) {
            QStringList labels;
            if (!readName(msg, pos, &labels) || pos + 4 > msg.size()) {
                break;
            }
            const quint16 qtype = get16(msg, pos);
            pos += 4;
            const QString name = joinLower(labels);
            if (name == typeName && (qtype == kTypePtr || qtype == kTypeAny)) {
                ptr = srvTxt = addr = true;
            } else if (name == QLatin1String("_services._dns-sd._udp.local") && qtype == kTypePtr) {
                ptr = true; // service enumeration: answering with our PTR is close enough
            } else if (name == fullName && (qtype == kTypeSrv || qtype == kTypeTxt || qtype == kTypeAny)) {
                srvTxt = addr = true;
            } else if (name == hostName && (qtype == kTypeA || qtype == kTypeAny)) {
                addr = true;
            }
        }
        if (!(ptr || srvTxt || addr)) {
            continue;
        }
        if (dg.senderPort() != kPort) {
            // Legacy unicast query (RFC 6762 §6.7): answer by unicast, repeating the questions.
            const QByteArray reply = buildResponse(id, ptr, srvTxt, addr, 1, dg.senderAddress(),
                    msg.mid(qStart, pos - qStart), qd);
            m_socket->writeDatagram(reply, dg.senderAddress(), dg.senderPort());
        } else {
            m_socket->writeDatagram(buildResponse(0, ptr, srvTxt, addr, 1, dg.senderAddress()), kGroup, kPort);
        }
    }
}

// ============================================================================ browser

MdnsBrowser::MdnsBrowser(QObject* parent)
        : QObject(parent) {
    qRegisterMetaType<vertexav::MdnsService>();
    connect(&m_queryTimer, &QTimer::timeout, this, &MdnsBrowser::query);
}

MdnsBrowser::~MdnsBrowser() {
    stop();
}

bool MdnsBrowser::start(const QString& type) {
    stop();
    m_type = type;
    bool shared = false;
    m_socket = openSocket(this, false, &shared);
    if (!m_socket) {
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &MdnsBrowser::onReadyRead);
    m_queries = 0;
    query();
    m_queryTimer.start(1000);
    return true;
}

void MdnsBrowser::stop() {
    m_queryTimer.stop();
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

QList<MdnsService> MdnsBrowser::services() const {
    QList<MdnsService> out;
    for (const Entry& e : m_entries) {
        if (e.reported) {
            out.append(e.svc);
        }
    }
    return out;
}

void MdnsBrowser::query() {
    if (!m_socket) {
        return;
    }
    // Query at 0 s, 1 s, 3 s and then every 30 s.
    ++m_queries;
    if (m_queries == 2) {
        m_queryTimer.setInterval(2000);
    } else if (m_queries == 3) {
        m_queryTimer.setInterval(30000);
    }
    QByteArray q;
    put16(q, 0);
    put16(q, 0);
    int qd = 1;
    QByteArray questions = encodeName(splitType(m_type) << QStringLiteral("local"));
    put16(questions, kTypePtr);
    put16(questions, kClassIn);
    // Ask for missing SRV/TXT/A of known instances.
    for (const Entry& e : m_entries) {
        if (!e.hasSrv || !e.hasTxt) {
            questions += encodeName(QStringList{e.svc.instance} + splitType(m_type) << QStringLiteral("local"));
            put16(questions, kTypeAny);
            put16(questions, kClassIn);
            ++qd;
        }
    }
    put16(q, static_cast<quint16>(qd));
    put16(q, 0);
    put16(q, 0);
    put16(q, 0);
    q += questions;
    m_socket->writeDatagram(q, kGroup, kPort);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->expires && it->expires < now) {
            const QString inst = it->svc.instance;
            const bool reported = it->reported;
            it = m_entries.erase(it);
            if (reported) {
                emit serviceRemoved(inst);
            }
        } else {
            ++it;
        }
    }
}

void MdnsBrowser::onReadyRead() {
    const QString typeName = joinLower(splitType(m_type) << QStringLiteral("local"));
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram(9000);
        const QByteArray msg = dg.data();
        if (msg.size() < 12 || !(get16(msg, 2) & 0x8000)) {
            continue;
        }
        const int qd = get16(msg, 4);
        const int rr = get16(msg, 6) + get16(msg, 8) + get16(msg, 10);
        int pos = 12;
        bool ok = true;
        for (int i = 0; i < qd && ok; ++i) {
            QStringList l;
            ok = readName(msg, pos, &l) && pos + 4 <= msg.size();
            pos += 4;
        }
        QSet<QString> touched;
        for (int i = 0; i < rr && ok; ++i) {
            QStringList name;
            if (!readName(msg, pos, &name) || pos + 10 > msg.size()) {
                break;
            }
            const quint16 type = get16(msg, pos);
            const quint32 ttl = get32(msg, pos + 4);
            const int rdlen = get16(msg, pos + 8);
            pos += 10;
            if (pos + rdlen > msg.size()) {
                break;
            }
            const int rdStart = pos;
            const QString key = joinLower(name);
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (type == kTypePtr && key == typeName) {
                int p = rdStart;
                QStringList inst;
                if (readName(msg, p, &inst) && !inst.isEmpty()) {
                    const QString instKey = joinLower(inst);
                    if (ttl == 0) {
                        auto it = m_entries.find(instKey);
                        if (it != m_entries.end()) {
                            const bool reported = it->reported;
                            const QString n = it->svc.instance;
                            m_entries.erase(it);
                            if (reported) {
                                emit serviceRemoved(n);
                            }
                        }
                    } else {
                        Entry& e = m_entries[instKey];
                        e.svc.instance = inst.first();
                        e.svc.type = m_type;
                        e.expires = now + qint64(ttl) * 1000;
                        touched.insert(instKey);
                    }
                }
            } else if (type == kTypeSrv && rdlen >= 7) {
                auto it = m_entries.find(key);
                if (it == m_entries.end() && key.endsWith(typeName) && ttl > 0) {
                    it = m_entries.insert(key, Entry());
                    it->svc.instance = name.first();
                    it->svc.type = m_type;
                    it->expires = now + qint64(kTtlOther) * 1000;
                }
                if (it != m_entries.end()) {
                    it->svc.port = get16(msg, rdStart + 4);
                    int p = rdStart + 6;
                    QStringList target;
                    if (readName(msg, p, &target)) {
                        it->target = joinLower(target);
                        it->svc.host = target.value(0);
                    }
                    it->hasSrv = true;
                    touched.insert(key);
                }
            } else if (type == kTypeTxt) {
                auto it = m_entries.find(key);
                if (it != m_entries.end()) {
                    it->svc.txt.clear();
                    int p = rdStart;
                    while (p < rdStart + rdlen) {
                        const int len = static_cast<quint8>(msg[p]);
                        const QString kv = QString::fromUtf8(msg.mid(p + 1, len));
                        p += 1 + len;
                        if (kv.isEmpty()) {
                            continue;
                        }
                        const int eq = kv.indexOf(QLatin1Char('='));
                        it->svc.txt.insert(eq < 0 ? kv : kv.left(eq), eq < 0 ? QString() : kv.mid(eq + 1));
                    }
                    it->hasTxt = true;
                    touched.insert(key);
                }
            } else if (type == kTypeA && rdlen == 4) {
                const QHostAddress a(get32(msg, rdStart));
                auto& list = m_hosts[key];
                if (!list.contains(a)) {
                    list.append(a);
                }
                for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                    if (it->target == key) {
                        touched.insert(it.key());
                    }
                }
            }
            pos = rdStart + rdlen;
        }
        for (const QString& k : touched) {
            maybeComplete(k);
        }
    }
}

void MdnsBrowser::maybeComplete(const QString& key) {
    auto it = m_entries.find(key);
    if (it == m_entries.end() || !it->hasSrv || !it->hasTxt) {
        return;
    }
    const QList<QHostAddress> addrs = m_hosts.value(it->target);
    if (addrs.isEmpty()) {
        return;
    }
    const bool changed = !it->reported || it->svc.addresses != addrs || it->svc.port != it->lastPort ||
            it->svc.txt != it->lastTxt;
    it->svc.addresses = addrs;
    it->reported = true;
    it->lastPort = it->svc.port;
    it->lastTxt = it->svc.txt;
    if (changed) {
        emit serviceUpdated(it->svc);
    }
}

} // namespace vertexav
