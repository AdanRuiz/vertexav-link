#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

#include "vertexav/vertexav.h"

using namespace vertexav;

class TestVertexAV : public QObject {
    Q_OBJECT

  private:
    static TempoPacket samplePacket() {
        TempoPacket p;
        p.bpm = 126.0;
        p.anchorT = 50100000;
        p.anchorBeat = 1024.0;
        p.nextBeatT = 50400000;
        p.nextDownbeatT = 51000000;
        p.beatsPerBar = 4;
        p.sourceDeck = 1;
        p.playing = true;
        p.stability = Stability::Freewheel;
        p.confidence = 1.0;
        return p;
    }

  private slots:
    void packetRoundTrip() {
        const QByteArray body = samplePacket().packBody();
        const QByteArray data = packPacket(Kind::Tempo, 7, 1234, 50100000, body);
        QCOMPARE(data.size(), 68);
        const Validation v = validateDatagram(data, 1234, QByteArray(), false);
        QVERIFY(v.ok);
        QCOMPARE(v.header.seq, 7u);
        TempoPacket out;
        QVERIFY(TempoPacket::unpackBody(v.body, &out));
        QCOMPARE(out.bpm, 126.0);
        QCOMPARE(out.anchorT, Q_INT64_C(50100000));
        QCOMPARE(out.stability, Stability::Freewheel);
        QVERIFY(out.playing);
    }

    void packetValidationSteps() {
        const QByteArray key(32, 'k');
        const QByteArray signedPkt = packPacket(Kind::Tempo, 1, 99, 0, samplePacket().packBody(), key);
        QCOMPARE(signedPkt.size(), 76);
        QVERIFY(validateDatagram(signedPkt, 99, key, true).ok);
        QByteArray bad = signedPkt;
        bad[30] = static_cast<char>(bad[30] ^ 1);
        QCOMPARE(validateDatagram(bad, 99, key, true).step, 4);
        QCOMPARE(validateDatagram(signedPkt.left(20), 99, key, true).step, 1);
        QCOMPARE(validateDatagram(signedPkt, 98, key, true).step, 3);
        const QByteArray unsignedPkt = packPacket(Kind::Tempo, 1, 99, 0, samplePacket().packBody());
        QCOMPARE(validateDatagram(unsignedPkt + QByteArray(1, 0), 99, QByteArray(), false).step, 5);
        // Same bytes as Python: ping is 36, pong 52
        QCOMPARE(packPacket(Kind::ClockPing, 1, 5, 0, PingPacket{123, 9}.packBody()).size(), 36);
        QCOMPARE(packPacket(Kind::ClockPong, 1, 5, 0, PongPacket{123, 9, 1, 2}.packBody()).size(), 52);
    }

    void seqWindows() {
        SeqWindow w;
        QVERIFY(w.accept(100));
        QVERIFY(w.accept(101));
        QVERIFY(!w.accept(101));
        QVERIFY(!w.accept(60));
        QVERIFY(w.accept(357));
        QVERIFY(!w.accept(357 - 65));
        for (quint32 s = 10000; s < 10007; ++s) {
            QVERIFY(!w.accept(s));
        }
        QVERIFY(w.accept(10007));
        SeqWindow wrap;
        QVERIFY(wrap.accept(0xFFFFFFFEu));
        QVERIFY(wrap.accept(0xFFFFFFFFu));
        QVERIFY(wrap.accept(0));
    }

    void hkdfVector() {
        const QByteArray okm = security::hkdfSha256(QByteArray(22, 0x0b), QByteArray::fromHex("000102030405060708090a0b0c"),
                QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9"), 42);
        QCOMPARE(okm.toHex(), QByteArray("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
    }

    void pairingCode() {
        const QByteArray psk = security::generatePsk();
        const QString code = security::pskToCode(psk);
        QCOMPARE(code.size(), 52);
        bool ok = false;
        QCOMPARE(security::codeToPsk(code.toLower(), &ok), psk);
        QVERIFY(ok);
    }

    void tempoMath() {
        Tempo t;
        t.bpm = 120;
        QCOMPARE(t.nextBeatT(0), Q_INT64_C(500000));
        QCOMPARE(t.nextDownbeatT(100000), Q_INT64_C(2000000));
        QCOMPARE(t.beatInBar(5), 2);
        const Tempo back = Tempo::fromJson(t.toJson());
        QCOMPARE(back.bpm, 120.0);
    }

    void follower() {
        TempoFollower f;
        Tempo base;
        base.bpm = 120;
        base.playing = true;
        QCOMPARE(f.update(base, 0), TempoFollower::Result::Init);
        Tempo ahead = base;
        ahead.anchorT = 1000000;
        ahead.anchorBeat = 2.1;
        QCOMPARE(f.update(ahead, 1000000), TempoFollower::Result::Smooth);
        QVERIFY(std::abs(f.beatAt(1000000) - 2.0) < 1e-9);
        QVERIFY(std::abs(f.beatAt(4000000) - 8.1) < 1e-6);
        Tempo jump = base;
        jump.anchorT = 5000000;
        jump.anchorBeat = 10.5;
        QCOMPARE(f.update(jump, 5000000), TempoFollower::Result::Jump);
        Tempo unstable = jump;
        unstable.stability = Stability::Unstable;
        unstable.anchorBeat = 99;
        QCOMPARE(f.update(unstable, 5100000), TempoFollower::Result::Frozen);
        QVERIFY(f.beatsEnabled(5100000 + 3000000));
        QVERIFY(!f.beatsEnabled(5100000 + 5000000));
    }

    void mixAnalyzer() {
        MixAnalyzer m;
        const double beat = 500000;
        m.update({{1, 1.0}, {2, 0.0}}, 0, beat);
        MixResult r = m.update({{1, 1.0}, {2, 0.2}}, 10000, beat);
        QCOMPARE(r.events.value(0).name, QStringLiteral("transition.start"));
        r = m.update({{1, 0.4}, {2, 0.9}}, 2000000, beat);
        QVERIFY(r.onAirChanged);
        m.update({{1, 0.0}, {2, 1.0}}, 3000000, beat);
        r = m.update({{1, 0.0}, {2, 1.0}}, 3600000, beat);
        QCOMPARE(r.events.value(0).name, QStringLiteral("transition.end"));
        MixAnalyzer c;
        c.update({{1, 1.0}, {2, 0.0}}, 0, beat);
        r = c.update({{1, 0.0}, {2, 1.0}}, 5000, beat);
        QStringList names;
        for (const auto& e : r.events) {
            names << e.name;
        }
        QVERIFY(names.contains(QStringLiteral("mix.cut")));
        QVERIFY(names.contains(QStringLiteral("deck.gate")));
    }

    void jsonDepth() {
        QVERIFY(jsonDepthExceeds(QByteArray(33, '[') + QByteArray(33, ']')));
        QVERIFY(!jsonDepthExceeds("{\"a\":\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"}"));
    }

    void loopbackOpenSession() {
        EmitterConfig ec;
        ec.host = QHostAddress::LocalHost;
        ec.port = 0;
        ec.udpPort = 0;
        ec.announce = false;
        Emitter em(ec);
        QVERIFY(em.start());
        Tempo t;
        t.bpm = 240; // 4 beats per second for a quick test
        t.anchorT = em.now();
        t.playing = true;
        em.setTempo(t);
        em.trackLoaded(1, QStringLiteral("t:1"), QJsonObject{{"title", "x"}, {"bpm", 240.0}}, false);

        ReceiverConfig rc;
        rc.port = em.port();
        rc.reconnect = false;
        Receiver rx(rc);
        QSignalSpy connectedSpy(&rx, &Receiver::connected);
        QSignalSpy snapshotSpy(&rx, &Receiver::snapshotReceived);
        QSignalSpy beatSpy(&rx, &Receiver::beat);
        QSignalSpy eventSpy(&rx, &Receiver::eventFired);
        rx.start();
        QVERIFY(connectedSpy.wait(3000));
        QVERIFY(snapshotSpy.wait(3000));
        QCOMPARE(rx.tracks().value(1).value("track_id").toString(), QStringLiteral("t:1"));
        QTRY_VERIFY_WITH_TIMEOUT(rx.clockSync().synced(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(beatSpy.count() >= 3, 4000);
        // Consecutive beats, not doubled
        const qint64 b0 = beatSpy.at(0).at(0).toLongLong();
        const qint64 b1 = beatSpy.at(1).at(0).toLongLong();
        QCOMPARE(b1, b0 + 1);
        // A scheduled event fires; a cancelled one does not
        const QString keep = em.event(QStringLiteral("countdown"), em.now() + 100000, 1,
                QJsonObject{{"target", "drop"}, {"beats_remaining", 1}, {"target_at", QJsonValue(qint64(em.now() + 350000))}});
        const QString drop = em.event(QStringLiteral("section.change"), em.now() + 120000, 1,
                QJsonObject{{"from", "build"}, {"to", "drop"}, {"section_index", 2}, {"energy", 0.9}});
        QTest::qWait(30);
        em.cancelEvent(drop, QStringLiteral("loop"));
        QTest::qWait(400);
        QCOMPARE(eventSpy.count(), 1);
        QCOMPARE(eventSpy.at(0).at(0).toJsonObject().value("event_id").toString(), keep);
        QCOMPARE(rx.packetErrors(), 0ull);
        QCOMPARE(em.receiverCount(), 1);
        rx.stop();
        QTRY_COMPARE_WITH_TIMEOUT(em.receiverCount(), 0, 2000);
    }

    void emitterInOwnThread() {
        // Regression: Mixxx runs the emitter in its own QThread; its timers must follow it.
        EmitterConfig ec;
        ec.host = QHostAddress::LocalHost;
        ec.port = 0;
        ec.udpPort = 0;
        ec.announce = false;
        QThread thread;
        auto* em = new Emitter(ec);
        em->moveToThread(&thread);
        thread.start();
        bool ok = false;
        QMetaObject::invokeMethod(em, [em, &ok] { ok = em->start(); }, Qt::BlockingQueuedConnection);
        QVERIFY(ok);
        Tempo t;
        t.bpm = 240;
        t.anchorT = em->now();
        t.playing = true;
        em->setTempo(t); // called from another thread: queued
        ReceiverConfig rc;
        rc.port = em->port();
        rc.reconnect = false;
        Receiver rx(rc);
        QSignalSpy beatSpy(&rx, &Receiver::beat);
        QSignalSpy lost(&rx, &Receiver::disconnected);
        rx.start();
        QTRY_VERIFY_WITH_TIMEOUT(beatSpy.count() >= 4, 6000);
        QTest::qWait(3500); // longer than the heartbeat timeout
        QCOMPARE(lost.count(), 0);
        rx.stop();
        QMetaObject::invokeMethod(em, [em] { em->stop(); delete em; }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }

    void loopbackAuthSession() {
        const QByteArray psk = security::generatePsk();
        EmitterConfig ec;
        ec.host = QHostAddress::LocalHost;
        ec.port = 0;
        ec.udpPort = 0;
        ec.announce = false;
        ec.security = QStringLiteral("auth");
        ec.psk = psk;
        Emitter em(ec);
        QVERIFY(em.start());
        Tempo t;
        t.anchorT = em.now();
        t.playing = true;
        em.setTempo(t);
        ReceiverConfig rc;
        rc.port = em.port();
        rc.reconnect = false;
        rc.psk = psk;
        Receiver rx(rc);
        QSignalSpy connectedSpy(&rx, &Receiver::connected);
        rx.start();
        QVERIFY(connectedSpy.wait(3000));
        QTRY_VERIFY_WITH_TIMEOUT(rx.clockSync().synced(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(rx.follower().hasModel(), 2000);
        QCOMPARE(rx.packetErrors(), 0ull);
        rx.stop();

        // Wrong key: rejected
        ReceiverConfig bad = rc;
        bad.psk = security::generatePsk();
        Receiver rx2(bad);
        QSignalSpy status(&rx2, &Receiver::statusMessage);
        rx2.start();
        QTRY_VERIFY_WITH_TIMEOUT(!status.isEmpty() &&
                        status.last().at(0).toString().contains(QStringLiteral("auth_failed")),
                3000);
        QVERIFY(!rx2.isConnected());
    }

    void downgradeRefused() {
        EmitterConfig ec;
        ec.host = QHostAddress::LocalHost;
        ec.port = 0;
        ec.udpPort = 0;
        ec.announce = false;
        Emitter em(ec);
        QVERIFY(em.start());
        ReceiverConfig rc;
        rc.port = em.port();
        rc.reconnect = false;
        rc.psk = security::generatePsk();
        Receiver rx(rc);
        QSignalSpy status(&rx, &Receiver::statusMessage);
        rx.start();
        QTRY_VERIFY_WITH_TIMEOUT(!status.isEmpty() &&
                        status.last().at(0).toString().contains(QStringLiteral("security_downgrade")),
                3000);
    }

    void preAuthLimits() {
        EmitterConfig ec;
        ec.host = QHostAddress::LocalHost;
        ec.port = 0;
        ec.udpPort = 0;
        ec.announce = false;
        Emitter em(ec);
        QVERIFY(em.start());
        // A message other than hello closes the connection.
        WsConnection* ws = WsConnection::connectTo(QStringLiteral("127.0.0.1"), em.port(), QStringLiteral("/vertexav"));
        QSignalSpy opened(ws, &WsConnection::opened);
        QSignalSpy closed(ws, &WsConnection::disconnected);
        QVERIFY(opened.wait(2000));
        ws->sendText("{\"type\":\"subscribe\",\"topics\":[]}");
        QVERIFY(closed.wait(2000));
        delete ws;
        // No hello within 2 s
        WsConnection* idle = WsConnection::connectTo(QStringLiteral("127.0.0.1"), em.port(), QStringLiteral("/vertexav"));
        QSignalSpy idleClosed(idle, &WsConnection::disconnected);
        QVERIFY(idleClosed.wait(3500));
        delete idle;
        // Wrong path: HTTP 404
        QTcpSocket raw;
        raw.connectToHost(QStringLiteral("127.0.0.1"), em.port());
        QVERIFY(raw.waitForConnected(1000));
        raw.write("GET /other HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        QTRY_VERIFY_WITH_TIMEOUT(raw.bytesAvailable() > 0 || raw.waitForReadyRead(50), 2000);
        QVERIFY(raw.readAll().startsWith("HTTP/1.1 404"));
    }
};

QTEST_GUILESS_MAIN(TestVertexAV)
#include "test_vertexav.moc"
