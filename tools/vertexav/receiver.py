"""VertexAV Link receiver core, shared by the inspector and the OS2L bridge.

Handles connection and reconnection, authentication, clock synchronisation over UDP, packet
validation (§8.3), state (snapshot + deltas) and scheduling of events at their exact instant.
"""
from __future__ import annotations

import asyncio
import json
import logging
import secrets
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

import websockets
from websockets.exceptions import ConnectionClosed

from . import proto, security
from .clock import ClockSync, ShowClock
from .proto import (KIND_CLOCK_PING, KIND_CLOCK_PONG, KIND_FEATURES, KIND_TEMPO, PING_BODY,
                    PacketError, SeqWindow)
from .tempo import Tempo, TempoFollower
from .util import dumps, jittered

log = logging.getLogger("vertexav.receiver")


class FatalSessionError(Exception):
    """The session cannot continue and must not be retried automatically."""


class Disconnected(Exception):
    """The emitter said bye."""


@dataclass
class ReceiverConfig:
    host: str = "127.0.0.1"
    port: int = proto.CONTROL_PORT
    name: str = "VertexAV Receiver"
    client_id: str = field(default_factory=lambda: "rx-" + secrets.token_hex(4))
    role: str = "receiver"
    own_id: Optional[str] = None  # bridges: own id, for loop detection (§5.5)
    profiles: list[int] = field(default_factory=lambda: [0, 1, 2])
    topics: Optional[list[str]] = None  # None = everything in the agreed profiles
    latency_us: int = 0
    psk: Optional[bytes] = None
    udp: bool = True
    udp_port: int = 0
    realtime_over_control: bool = False
    reconnect: bool = True


class Receiver:
    def __init__(self, cfg: ReceiverConfig):
        self.cfg = cfg
        self.clock = ShowClock()
        self.sync = ClockSync()
        self.follower = TempoFollower()
        # callbacks
        self.on_message: Optional[Callable[[dict, str], None]] = None  # (msg, raw)
        self.on_sent: Optional[Callable[[dict], None]] = None
        self.on_packet: Optional[Callable[[proto.Header, Any, int], None]] = None
        self.on_packet_error: Optional[Callable[[PacketError, bytes], None]] = None
        self.on_status: Optional[Callable[[str], None]] = None
        self.on_event_fire: Optional[Callable[[dict], None]] = None
        self.on_trigger_fire: Optional[Callable[[dict], None]] = None
        # session
        self.ws = None
        self.welcome: Optional[dict] = None
        self.session_key: Optional[int] = None
        self.mac_key: Optional[bytes] = None
        self.security_seen: dict[str, str] = {}  # emitter id -> strongest mode seen
        self.windows: dict[tuple, SeqWindow] = {}
        self._ping_seq = 0
        self._ping_id = 0
        self._pings: dict[int, int] = {}
        self._udp: Optional[asyncio.DatagramTransport] = None
        self.udp_local_port: Optional[int] = None
        self._tasks: list[asyncio.Task] = []
        self._scheduled: dict[str, asyncio.TimerHandle] = {}
        self._last_rx = 0.0
        self._stop = False
        self.connected = asyncio.Event()
        # state
        self.tempo: Optional[Tempo] = None
        self.decks: dict[int, dict] = {}
        self.mix: dict = {}
        self.tracks: dict[int, dict] = {}
        self.structures: dict[str, dict] = {}
        self.intent: dict = {}
        self.events: dict[str, dict] = {}
        self._last_t: dict[str, int] = {}
        self.stats = {"packets": 0, "packet_errors": 0, "messages": 0, "stale_deltas": 0,
                      "reconnects": 0}

    # ------------------------------------------------------------------ helpers
    def local_now(self) -> int:
        return self.clock.now()

    def emitter_now(self) -> Optional[int]:
        if self.sync.offset is None:
            return None
        return self.sync.to_emitter(self.local_now())

    def _status(self, text: str) -> None:
        log.info(text)
        if self.on_status:
            self.on_status(text)

    async def send(self, msg: dict) -> None:
        if self.ws is None:
            return
        await self.ws.send(dumps(msg))
        if self.on_sent:
            self.on_sent(msg)

    # ------------------------------------------------------------------ lifecycle
    async def run(self) -> None:
        delay = 0.5
        while not self._stop:
            try:
                await self._session()
                delay = 0.5
            except FatalSessionError as e:
                self._status(f"sesión terminada: {e}")
                return
            except (OSError, ConnectionClosed, asyncio.TimeoutError, websockets.InvalidHandshake,
                    websockets.InvalidURI, Disconnected, json.JSONDecodeError) as e:
                self._status(f"conexión perdida: {e.__class__.__name__}: {e}")
            finally:
                await self._teardown()
            if not self.cfg.reconnect or self._stop:
                return
            self.stats["reconnects"] += 1
            wait = jittered(delay)
            self._status(f"reintento en {wait:.2f} s")
            await asyncio.sleep(wait)
            delay = min(delay * 2, 5.0)  # §6.5

    async def stop(self) -> None:
        self._stop = True
        if self.ws is not None:
            try:
                await self.ws.send(dumps({"type": "bye", "reason": "shutdown"}))
                await self.ws.close()
            except ConnectionClosed:
                pass

    async def _teardown(self) -> None:
        self.connected.clear()
        for t in self._tasks:
            t.cancel()
        self._tasks.clear()
        for h in self._scheduled.values():
            h.cancel()
        self._scheduled.clear()
        if self.ws is not None:
            try:
                await self.ws.close()
            except Exception:
                pass
        self.ws = None

    async def _open_udp(self) -> None:
        if self._udp is not None or not self.cfg.udp or self.cfg.realtime_over_control:
            return
        loop = asyncio.get_running_loop()
        self._udp, _ = await loop.create_datagram_endpoint(
            lambda: _RxUdp(self), local_addr=("0.0.0.0", self.cfg.udp_port))
        self.udp_local_port = self._udp.get_extra_info("sockname")[1]

    async def _recv(self, timeout: float) -> dict:
        raw = await asyncio.wait_for(self.ws.recv(), timeout)
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", errors="replace")
        if proto.json_depth_exceeds(raw):
            await self._fatal("message_too_large", "Anidamiento JSON excesivo")
        msg = json.loads(raw)
        if self.on_message:
            self.on_message(msg, raw)
        return msg

    async def _fatal(self, code: str, message: str) -> None:
        try:
            await self.ws.send(dumps({"type": "error", "code": code, "message": message, "fatal": True}))
            await self.ws.close()
        except ConnectionClosed:
            pass
        raise FatalSessionError(f"{code}: {message}")

    async def _session(self) -> None:
        await self._open_udp()
        url = f"ws://{self.cfg.host}:{self.cfg.port}{proto.WS_PATH}"
        self.ws = await websockets.connect(url, max_size=proto.MAX_MSG_EMITTER_TO_RECEIVER,
                                           ping_interval=None, open_timeout=5, compression=None)
        hello = {"type": "hello", "protocol": proto.PROTOCOL, "version": proto.VERSION,
                 "role": self.cfg.role, "name": self.cfg.name, "client_id": self.cfg.client_id,
                 "profiles": self.cfg.profiles, "latency_us": self.cfg.latency_us,
                 "realtime_over_control": self.cfg.realtime_over_control}
        if self.udp_local_port:
            hello["udp_port"] = self.udp_local_port
        await self.send(hello)
        msg = await self._recv(5.0)
        server_nonce = client_nonce = None
        if msg.get("type") == "auth.challenge":
            if self.cfg.psk is None:
                await self._fatal("auth_required", "El emisor exige autenticación y no hay clave (--psk)")
            server_nonce = security.unb64(msg["server_nonce"])
            client_nonce = security.new_nonce()
            proof = security.client_proof(self.cfg.psk, server_nonce, client_nonce, self.cfg.client_id)
            await self.send({"type": "auth.response", "client_nonce": security.b64(client_nonce),
                             "proof": security.b64(proof)})
            msg = await self._recv(5.0)
        if msg.get("type") == "error":
            raise FatalSessionError(f"{msg.get('code')}: {msg.get('message')}")
        if msg.get("type") != "welcome":
            await self._fatal("bad_message", f"se esperaba welcome, llegó {msg.get('type')}")
        welcome = msg
        if str(welcome.get("version", "")).split(".")[0] != proto.VERSION.split(".")[0]:
            await self._fatal("unsupported_version", f"Versión {welcome.get('version')} no soportada")
        emitter = welcome.get("emitter") or {}
        mode = welcome.get("security")
        # No downgrades (§15.2): with a key configured we demand auth.
        if self.cfg.psk is not None and mode == "open":
            await self._fatal("security_downgrade", "El emisor ofrece 'open' pero estamos emparejados")
        prev = self.security_seen.get(emitter.get("id"))
        if prev in ("auth", "tls") and mode == "open":
            await self._fatal("security_downgrade", "Modo de seguridad más débil que el anterior")
        if mode in ("auth", "tls"):
            if server_nonce is None:
                await self._fatal("auth_failed", "welcome en modo auth sin desafío")
            expected = security.server_proof(self.cfg.psk, client_nonce, server_nonce, welcome.get("session_id", ""))
            try:
                got = security.unb64(welcome.get("server_proof") or "")
            except Exception:
                got = b""
            if not security.equal(expected, got):
                await self._fatal("auth_failed", "server_proof incorrecto: emisor no auténtico")
            self.mac_key = security.mac_key(self.cfg.psk, server_nonce, client_nonce)
        else:
            self.mac_key = None
        self.security_seen[emitter.get("id")] = mode
        if self.cfg.own_id and self.cfg.own_id in (emitter.get("path") or []):
            await self._fatal("loop_detected", "Nuestro id está en emitter.path")
        # The emitter numbers packets per connection: start the windows again.
        self.windows.clear()
        if welcome.get("session_key") != self.session_key:
            # New emitter session: forget clock samples and tempo too (§8.3).
            self.sync = ClockSync()
            self.follower = TempoFollower()
        self.session_key = welcome.get("session_key")
        self.welcome = welcome
        self.events.clear()
        self._last_t.clear()
        self._last_rx = self.local_now()
        self._status(f"conectado a {emitter.get('name')} ({emitter.get('id')}), sesión "
                     f"{welcome.get('session_id')}, perfiles {welcome.get('profiles')}, seguridad {mode}")
        self._tasks += [asyncio.create_task(self._heartbeat()),
                        asyncio.create_task(self._clock_task())]
        reader = asyncio.create_task(self._reader())
        self._tasks.append(reader)
        await self._initial_sync()
        sub = {"type": "subscribe"}
        agreed = set(welcome.get("profiles") or [])
        from .schema import TOPICS
        topics = self.cfg.topics or [t for t, p in TOPICS.items() if p in agreed and p <= 2]
        sub["topics"] = topics
        await self.send(sub)
        self.connected.set()
        await reader

    async def _reader(self) -> None:
        while True:
            msg = await self._recv(proto.HEARTBEAT_TIMEOUT_S)
            self._last_rx = self.local_now()
            self.stats["messages"] += 1
            self._apply(msg)
            if msg.get("type") == "error" and msg.get("fatal"):
                raise FatalSessionError(f"{msg.get('code')}: {msg.get('message')}")
            if msg.get("type") == "bye":
                raise Disconnected(msg.get("reason", "bye"))

    async def _heartbeat(self) -> None:
        while True:
            await asyncio.sleep(proto.HEARTBEAT_INTERVAL_S)
            await self.send({"type": "heartbeat", "t": self.local_now()})

    # ------------------------------------------------------------------ clock sync
    def _send_ping(self) -> None:
        if self.session_key is None:
            return
        self._ping_id = (self._ping_id + 1) & 0xFFFFFFFF
        t0 = self.local_now()
        self._pings[self._ping_id] = t0
        if len(self._pings) > 64:
            self._pings.pop(next(iter(self._pings)))
        if self._udp is not None:
            self._ping_seq += 1
            body = PING_BODY.pack(t0, self._ping_id)
            pkt = proto.pack_packet(KIND_CLOCK_PING, self._ping_seq, self.session_key, 0, body, self.mac_key)
            self._udp.sendto(pkt, (self.cfg.host, self.welcome.get("udp_port") or proto.REALTIME_PORT))
        elif self.ws is not None:
            asyncio.create_task(self.send({"type": "clock.ping", "t0": t0, "ping_id": self._ping_id}))

    async def _initial_sync(self) -> None:
        """16 exchanges spaced 20-50 ms (§7.2), at most ~1.5 s if UDP is blocked."""
        for _ in range(16):
            self._send_ping()
            await asyncio.sleep(0.03)
        for _ in range(30):
            if self.sync.synced:
                return
            self._send_ping()
            await asyncio.sleep(0.03)
        if not self.sync.synced:
            self._status("AVISO: sin respuesta a clock.ping por UDP (¿cortafuegos?)")

    async def _clock_task(self) -> None:
        await asyncio.sleep(2.0)
        while True:
            self._send_ping()
            await asyncio.sleep(1.5)

    def _on_pong(self, t0: int, ping_id: int, t1: int, t2: int, t3: int) -> None:
        sent = self._pings.pop(ping_id, None)
        if sent is None or sent != t0:
            return
        self.sync.add(t0, t1, t2, t3)

    # ------------------------------------------------------------------ UDP
    def _window(self, kind: int, sub: int = 0) -> SeqWindow:
        w = self.windows.get((kind, sub))
        if w is None:
            w = self.windows[(kind, sub)] = SeqWindow()
        return w

    def handle_datagram(self, data: bytes, addr: tuple) -> None:
        t3 = self.local_now()
        self.stats["packets"] += 1
        if self.session_key is None:
            return
        try:
            hdr, body = proto.validate_datagram(data, self.session_key, self.mac_key,
                                                require_signature=self.mac_key is not None)
            emitter_now = self.emitter_now() if self.sync.offset is not None else None
            if emitter_now is not None and abs(hdr.t - emitter_now) > 2_000_000:
                raise PacketError(6, f"t={hdr.t} difiere {abs(hdr.t - emitter_now)/1e6:.2f} s del reloj estimado")
            sub = body[0] if hdr.kind == KIND_FEATURES else 0
            if hdr.kind != proto.KIND_AUDIO and not self._window(hdr.kind, sub).accept(hdr.seq):
                raise PacketError(7, f"seq {hdr.seq} fuera de ventana")
            pkt = proto.unpack_body(hdr.kind, body)
        except PacketError as e:
            self.stats["packet_errors"] += 1
            if self.on_packet_error:
                self.on_packet_error(e, data)
            return
        except Exception as e:  # malformed body
            self.stats["packet_errors"] += 1
            if self.on_packet_error:
                self.on_packet_error(PacketError(5, str(e)), data)
            return
        if hdr.kind == KIND_CLOCK_PONG:
            self._on_pong(pkt.t0, pkt.ping_id, pkt.t1, pkt.t2, t3)
        elif hdr.kind == KIND_TEMPO and self.sync.offset is not None:
            base = self.tempo or Tempo()
            tp = Tempo(bpm=pkt.bpm, anchor_t=pkt.anchor_t, anchor_beat=pkt.anchor_beat,
                       beats_per_bar=pkt.beats_per_bar, bar_offset=base.bar_offset,
                       source_deck=pkt.source_deck, playing=pkt.playing, confidence=pkt.confidence,
                       stability=pkt.stability, phase_reset=pkt.phase_reset,
                       link_aligned=pkt.link_aligned if base.link_aligned is not None else None)
            # bar_offset is not in the binary packet: derive it from next_downbeat_t.
            nd_beat = tp.beat_at(pkt.next_downbeat_t)
            tp.bar_offset = round(nd_beat) % max(1, pkt.beats_per_bar)
            self.follower.update(tp, self.sync.to_emitter(t3))
        if self.on_packet:
            self.on_packet(hdr, pkt, t3)

    # ------------------------------------------------------------------ state
    def _fresh(self, key: str, t: Any) -> bool:
        """Deltas older than the last applied for the same object are discarded (§8.2)."""
        if not isinstance(t, int):
            return True
        last = self._last_t.get(key)
        if last is not None and t < last:
            self.stats["stale_deltas"] += 1
            return False
        self._last_t[key] = t
        return True

    @staticmethod
    def _merge(dst: dict, src: dict) -> None:
        for k, v in src.items():
            if k in ("type", "seq"):
                continue
            if isinstance(v, dict) and isinstance(dst.get(k), dict):
                Receiver._merge(dst[k], v)
            else:
                dst[k] = v

    def _apply(self, msg: dict) -> None:
        mtype = msg.get("type")
        if mtype == "snapshot":
            if msg.get("tempo"):
                self._apply_tempo(msg["tempo"])
            self.decks = {d["deck"]: {**d, "_pos_t": msg.get("t")} for d in msg.get("decks") or []
                          if isinstance(d, dict)}
            self.mix = msg.get("mix") or {}
            self.tracks = {t["deck"]: t for t in msg.get("tracks") or [] if isinstance(t, dict)}
            self.structures = {s["track_id"]: s for s in msg.get("structures") or [] if isinstance(s, dict)}
            self.intent = msg.get("intent") or {}
            for ev in msg.get("events") or []:
                self._apply({**ev, "type": "event"})
        elif mtype == "tempo":
            if self._fresh("tempo", msg.get("t")):
                self._apply_tempo(msg)
        elif mtype == "tempo.rt":
            self._apply_tempo(msg)
        elif mtype == "deck.state":
            deck = msg.get("deck")
            if self._fresh(f"deck:{deck}", msg.get("t")):
                st = self.decks.setdefault(deck, {"deck": deck})
                self._merge(st, msg)
                if "position_beat" in msg or "position_s" in msg:
                    st["_pos_t"] = msg.get("t")  # position is valid at this instant (§10.1)
        elif mtype == "mix.state":
            if self._fresh("mix", msg.get("t")):
                self._merge(self.mix, msg)
        elif mtype == "intent":
            if self._fresh("intent", msg.get("t")):
                self._merge(self.intent, msg)
        elif mtype == "track.loaded":
            self.tracks[msg.get("deck")] = msg
        elif mtype == "track.unloaded":
            self.tracks.pop(msg.get("deck"), None)
        elif mtype == "track.structure":
            self.structures[msg.get("track_id")] = msg
        elif mtype == "event":
            eid = msg.get("event_id")
            self.events[eid] = msg
            self._schedule(eid, msg.get("at"), lambda m=msg: self._fire_event(m))
        elif mtype == "event.cancel":
            eid = msg.get("event_id")
            self.events.pop(eid, None)
            h = self._scheduled.pop(eid, None)
            if h:
                h.cancel()
        elif mtype == "trigger":
            key = f"trigger-{msg.get('seq')}-{msg.get('name')}"
            self._schedule(key, msg.get("at"), lambda m=msg: self._fire_trigger(m))

    def _apply_tempo(self, msg: dict) -> None:
        self.tempo = Tempo.from_json(msg, self.tempo)
        now = self.emitter_now()
        if now is not None:
            self.follower.update(self.tempo, now)
        elif self.follower.model is None:
            self.follower.update(self.tempo, msg.get("anchor_t", 0))
        self.tempo.phase_reset = False

    def _schedule(self, key: str, at: Any, cb: Callable[[], None]) -> None:
        """Run cb at emitter instant `at` minus our latency (§7.3)."""
        loop = asyncio.get_running_loop()
        if not isinstance(at, int) or self.sync.offset is None:
            loop.call_soon(cb)
            return
        local_target = self.sync.to_local(at) - self.cfg.latency_us
        delay = max(0.0, (local_target - self.local_now()) / 1e6)
        self._scheduled[key] = loop.call_later(delay, self._run_scheduled, key, cb)

    def _run_scheduled(self, key: str, cb: Callable[[], None]) -> None:
        self._scheduled.pop(key, None)
        cb()

    def _fire_event(self, msg: dict) -> None:
        self.events.pop(msg.get("event_id"), None)
        if self.on_event_fire:
            self.on_event_fire(msg)

    def _fire_trigger(self, msg: dict) -> None:
        if self.on_trigger_fire:
            self.on_trigger_fire(msg)

    # ------------------------------------------------------------------ queries
    def on_air_deck(self) -> Optional[int]:
        return self.mix.get("on_air_deck")

    def current_section(self, deck: Optional[int] = None) -> Optional[dict]:
        """Section of the deck (default: on air) at the current extrapolated position."""
        deck = deck or self.on_air_deck()
        st = self.decks.get(deck)
        tr = self.tracks.get(deck)
        if not st or not tr:
            return None
        struct = self.structures.get(tr.get("track_id"))
        now = self.emitter_now()
        if not struct or now is None or st.get("position_beat") is None:
            return None
        beat = st["position_beat"]
        if st.get("playing") and isinstance(st.get("_pos_t"), int) and self.tempo:
            beat += (now - st["_pos_t"]) / 1e6 * self.tempo.bpm / 60
        for i, s in enumerate(struct.get("sections") or []):
            if s["start_beat"] <= beat < s["end_beat"]:
                return {**s, "index": i}
        return None


class _RxUdp(asyncio.DatagramProtocol):
    def __init__(self, rx: Receiver):
        self.rx = rx

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        self.rx.handle_datagram(data, addr)

    def error_received(self, exc: Exception) -> None:
        pass
