"""End-to-end tests with a real emitter and receiver on localhost (ephemeral ports)."""
from __future__ import annotations

import asyncio
import json

import pytest
import websockets

from vertexav import proto, security
from vertexav.emitter import Emitter, EmitterConfig
from vertexav.receiver import FatalSessionError, Receiver, ReceiverConfig
from vertexav.schema import SessionChecker, validate_message
from vertexav.sim import FAST, Simulation
from vertexav.tempo import Tempo


async def _emitter(**kw) -> Emitter:
    cfg = EmitterConfig(host="127.0.0.1", port=0, udp_port=0, announce=False, id="vx-test01", **kw)
    em = Emitter(cfg)
    await em.start()
    return em


async def _receiver(em: Emitter, **kw) -> tuple[Receiver, asyncio.Task]:
    rx = Receiver(ReceiverConfig(host="127.0.0.1", port=em.cfg.port, reconnect=False, **kw))
    task = asyncio.create_task(rx.run())
    await asyncio.wait_for(rx.connected.wait(), 5)
    return rx, task


async def test_open_session_snapshot_tempo_and_clock():
    em = await _emitter()
    em.set_tempo(Tempo(bpm=128, anchor_t=em.now(), anchor_beat=0, playing=True))
    msgs = []
    rx, task = await _receiver(em)
    rx.on_message = lambda m, raw: msgs.append(m)
    await asyncio.sleep(0.6)
    assert rx.sync.synced and rx.sync.rtt_min < 20_000
    assert rx.tempo and rx.tempo.bpm == 128
    assert rx.follower.model is not None
    assert rx.stats["packet_errors"] == 0
    assert proto.KIND_TEMPO in {k for k, _ in rx.windows}
    await rx.stop()
    await em.stop()
    task.cancel()


async def test_auth_session_signs_packets():
    psk = security.generate_psk()
    em = await _emitter(security="auth", psk=psk)
    em.set_tempo(Tempo(bpm=120, anchor_t=em.now(), playing=True))
    rx, task = await _receiver(em, psk=psk)
    await asyncio.sleep(0.5)
    assert rx.mac_key is not None and rx.sync.synced
    assert rx.welcome["security"] == "auth"
    assert rx.stats["packet_errors"] == 0
    assert rx.windows  # signed tempo packets accepted
    await rx.stop()
    await em.stop()
    task.cancel()


async def test_auth_wrong_key_is_rejected():
    em = await _emitter(security="auth", psk=security.generate_psk())
    rx = Receiver(ReceiverConfig(host="127.0.0.1", port=em.cfg.port, reconnect=False,
                                 psk=security.generate_psk()))
    statuses = []
    rx.on_status = statuses.append
    await asyncio.wait_for(rx.run(), 5)
    assert any("auth_failed" in s for s in statuses)
    assert not em.sessions
    await em.stop()


async def test_receiver_refuses_downgrade():
    em = await _emitter(security="open")
    rx = Receiver(ReceiverConfig(host="127.0.0.1", port=em.cfg.port, reconnect=False,
                                 psk=security.generate_psk()))
    statuses = []
    rx.on_status = statuses.append
    await asyncio.wait_for(rx.run(), 5)
    assert any("security_downgrade" in s for s in statuses)
    await em.stop()


async def test_bridge_detects_loop():
    em = await _emitter(role="bridge", path=["vx-source", "vx-me"])
    rx = Receiver(ReceiverConfig(host="127.0.0.1", port=em.cfg.port, reconnect=False, role="bridge",
                                 own_id="vx-me"))
    statuses = []
    rx.on_status = statuses.append
    await asyncio.wait_for(rx.run(), 5)
    assert any("loop_detected" in s for s in statuses)
    await em.stop()


async def _raw(em: Emitter):
    return await websockets.connect(f"ws://127.0.0.1:{em.cfg.port}/vertexav", ping_interval=None,
                                    max_size=None)


async def test_limits_before_auth():
    em = await _emitter()
    # 1) a message other than hello closes the connection
    ws = await _raw(em)
    await ws.send(json.dumps({"type": "subscribe", "topics": []}))
    with pytest.raises(websockets.ConnectionClosed):
        await asyncio.wait_for(ws.recv(), 2)
    # 2) more than 4 KB before authenticating closes without answer
    ws = await _raw(em)
    await ws.send(json.dumps({"type": "hello", "pad": "x" * 5000}))
    with pytest.raises(websockets.ConnectionClosed):
        await asyncio.wait_for(ws.recv(), 2)
    # 3) no hello within 2 s
    ws = await _raw(em)
    with pytest.raises(websockets.ConnectionClosed):
        await asyncio.wait_for(ws.recv(), 3.5)
    # 4) wrong path
    with pytest.raises(websockets.InvalidStatus):
        await websockets.connect(f"ws://127.0.0.1:{em.cfg.port}/other")
    # 5) at most 4 pending connections per IP
    pending = [await _raw(em) for _ in range(4)]
    extra = await _raw(em)
    with pytest.raises(websockets.ConnectionClosed):
        await asyncio.wait_for(extra.recv(), 2)
    for p in pending:
        await p.close()
    await em.stop()


async def test_unknown_version_is_fatal():
    em = await _emitter()
    ws = await _raw(em)
    await ws.send(json.dumps({"type": "hello", "protocol": "vertexav", "version": "2.0", "role": "receiver",
                              "name": "x", "client_id": "x", "profiles": [0]}))
    msg = json.loads(await asyncio.wait_for(ws.recv(), 2))
    assert msg["type"] == "error" and msg["code"] == "unsupported_version" and msg["fatal"]
    await em.stop()


async def test_simulation_messages_are_valid():
    """Run the simulator for a few seconds and validate every control message."""
    em = await _emitter(lookahead_us=150_000)
    sim = Simulation(em, FAST, start_beat=12, printer=lambda s: None)
    sim_task = asyncio.create_task(sim.run())
    checker = SessionChecker()
    problems = []

    def on_msg(m, raw):
        issues = validate_message(m) + checker.check(m, asyncio.get_running_loop().time())
        problems.extend((m.get("type"), str(i)) for i in issues if i.level == "error")

    rx = Receiver(ReceiverConfig(host="127.0.0.1", port=em.cfg.port, reconnect=False))
    rx.on_message = on_msg
    rx.on_sent = checker.on_sent
    task = asyncio.create_task(rx.run())
    await asyncio.sleep(6)
    assert not problems, problems
    assert rx.stats["packet_errors"] == 0
    assert rx.current_section() is not None
    sim_task.cancel()
    await rx.stop()
    await em.stop()
    task.cancel()
