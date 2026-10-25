"""VertexAV Link emitter core: control channel (WebSocket), real-time channel (UDP), DNS-SD.

The emitter keeps the current state, sends a snapshot to each new receiver and then deltas.
Use the publishing API (set_tempo, update_deck, event, ...) from the same asyncio loop.
"""
from __future__ import annotations

import asyncio
import json
import logging
import math
import secrets
import socket
import time
from dataclasses import dataclass, field, replace
from typing import Any, Callable, Optional

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from . import proto, security
from .clock import ShowClock
from .proto import (KIND_CLOCK_PING, KIND_CLOCK_PONG, KIND_TEMPO, PONG_BODY, PacketError,
                    SeqWindow, TempoPacket)
from .schema import CONTINUOUS, SIGNED_CONTINUOUS, TOPICS
from .tempo import Tempo
from .util import dumps, load_or_create_id, local_ipv4_addresses

log = logging.getLogger("vertexav.emitter")

TOPIC_OF = {
    "tempo": "tempo", "deck.state": "deck", "mix.state": "mix", "track.loaded": "track",
    "track.unloaded": "track", "intent": "intent", "trigger": "trigger", "event": "event",
    "event.cancel": "event", "track.structure": "structure",
}
_MISSING = object()


@dataclass
class EmitterConfig:
    name: str = "VertexAV Emitter"
    id: Optional[str] = None
    role: str = "emitter"
    origin: Optional[str] = None
    path: Optional[list[str]] = None  # ids from the primary source up to the upstream (bridges)
    profiles: list[int] = field(default_factory=lambda: [0, 1, 2])
    security: str = "open"
    psk: Optional[bytes] = None  # shared key for every client
    keys: dict[str, bytes] = field(default_factory=dict)  # per client_id keys (§15.3)
    host: str = "0.0.0.0"
    port: int = proto.CONTROL_PORT
    udp_port: int = proto.REALTIME_PORT
    scope: str = "lan"
    announce: bool = True
    decks: int = 2
    lookahead_us: int = 150_000
    max_receivers: int = proto.MAX_RECEIVERS_DEFAULT
    link_enabled: bool = False


def _flatten(obj: dict, prefix: str = "") -> dict:
    out = {}
    for k, v in obj.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict) and v:
            out.update(_flatten(v, key + "."))
        else:
            out[key] = v
    return out


def _unflatten(flat: dict) -> dict:
    out: dict = {}
    for key, v in flat.items():
        parts = key.split(".")
        cur = out
        for p in parts[:-1]:
            cur = cur.setdefault(p, {})
        cur[parts[-1]] = v
    return out


def _deep_merge(dst: dict, src: dict) -> None:
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _deep_merge(dst[k], v)
        else:
            dst[k] = v


class ObjectThrottle:
    """Batching, dead zone, edges and final value for continuous controls (§8.4)."""

    INTERVAL = 0.05  # 20 Hz per object
    FINAL_QUIET = 0.05
    DEADZONE = 0.01

    def __init__(self, continuous: set[str]):
        self.continuous = continuous
        self.sent: dict[str, Any] = {}
        self.pending: dict[str, Any] = {}
        self.last_send = -math.inf
        self.last_change = 0.0

    def _edge(self, key: str, old: Any, new: Any) -> bool:
        if not isinstance(new, (int, float)) or not isinstance(old, (int, float)):
            return True
        edges = (-1, 0, 1) if key in SIGNED_CONTINUOUS else (0, 1)
        if new in edges and new != old:
            return True
        return (old == 0) != (new == 0)  # zero crossing

    def offer(self, flat: dict, now: float) -> Optional[dict]:
        immediate = False
        for k, v in flat.items():
            old = self.sent.get(k, _MISSING)
            if k in self.continuous:
                if old is _MISSING or self._edge(k, old, v):
                    immediate = True
                self.pending[k] = v
            elif old is _MISSING or old != v:
                self.pending[k] = v
                immediate = True
        self.last_change = now
        if immediate:
            return self._take(now, deadzone=False)
        if now - self.last_send >= self.INTERVAL:
            return self._take(now, deadzone=True)
        return None

    def poll(self, now: float) -> Optional[dict]:
        if not self.pending:
            return None
        if now - self.last_change >= self.FINAL_QUIET:
            return self._take(now, deadzone=False)
        if now - self.last_send >= self.INTERVAL:
            return self._take(now, deadzone=True)
        return None

    def _take(self, now: float, deadzone: bool) -> Optional[dict]:
        out = {}
        for k, v in list(self.pending.items()):
            old = self.sent.get(k, _MISSING)
            if old is not _MISSING and old == v:
                del self.pending[k]
                continue
            if (deadzone and k in self.continuous and isinstance(v, (int, float))
                    and isinstance(old, (int, float)) and abs(v - old) < self.DEADZONE):
                continue
            out[k] = v
        for k, v in out.items():
            self.sent[k] = v
            del self.pending[k]
        if out:
            self.last_send = now
            return out
        return None


@dataclass
class Session:
    ws: ServerConnection
    ip: str
    client_id: str = ""
    name: str = ""
    role: str = "receiver"
    profiles: list[int] = field(default_factory=list)
    topics: set[str] = field(default_factory=set)
    latency_us: int = 0
    udp_addr: Optional[tuple] = None
    rt_over_control: bool = False
    mac_key: Optional[bytes] = None
    ready: bool = False  # snapshot sent
    json_seq: int = 0
    udp_seq: dict = field(default_factory=dict)
    ping_window: SeqWindow = field(default_factory=SeqWindow)
    queue: asyncio.Queue = field(default_factory=lambda: asyncio.Queue(maxsize=2000))
    last_rx: float = field(default_factory=time.monotonic)
    last_tx: float = field(default_factory=time.monotonic)
    rx_times: list = field(default_factory=list)
    receiver_state: Optional[dict] = None
    capabilities: Optional[list] = None


class Emitter:
    def __init__(self, cfg: EmitterConfig, clock: Optional[ShowClock] = None):
        self.cfg = cfg
        self.clock = clock or ShowClock()
        self.id = cfg.id or load_or_create_id("emitter" if cfg.role == "emitter" else "bridge")
        upstream = list(cfg.path or [])
        self.path = upstream + [self.id]
        self.origin = cfg.origin or (upstream[0] if upstream else self.id)
        self.hops = len(self.path) - 1
        self.session_key = secrets.randbelow(0xFFFFFFFF) + 1
        self.session_id = secrets.token_hex(3)
        self.sessions: list[Session] = []
        self.on_receiver_message: Optional[Callable[[Session, dict], None]] = None
        # state
        self.tempo = Tempo(playing=False)
        self.decks: dict[int, dict] = {}
        self.mix: dict = {"crossfader": 0.0, "on_air_deck": None,
                          "transition": {"active": False, "kind": None, "from_deck": None,
                                         "to_deck": None, "progress": None}}
        self.tracks: dict[int, dict] = {}
        self.structures: dict[str, dict] = {}
        self.intent: dict = {}
        self.events: dict[str, dict] = {}
        self._event_counter = 10000
        self._throttles: dict[str, ObjectThrottle] = {}
        self._phase_reset_until = 0
        self._last_udp_tempo = -1e18
        self._last_beat_announced: Optional[int] = None
        # networking
        self._server = None
        self._udp: Optional[asyncio.DatagramTransport] = None
        self._tasks: list[asyncio.Task] = []
        self._unauth: dict[str, int] = {}
        self._auth_failures: dict[str, list] = {}
        self._zc = None
        self._zc_info = None
        for d in range(1, cfg.decks + 1):
            self.decks[d] = self._empty_deck(d)

    # ------------------------------------------------------------------ lifecycle
    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        self._server = await serve(
            self._handle, self.cfg.host, self.cfg.port,
            process_request=self._process_request,
            max_size=proto.MAX_MSG_PREAUTH, open_timeout=proto.HELLO_TIMEOUT_S,
            ping_interval=None, compression=None,
        )
        self._udp, _ = await loop.create_datagram_endpoint(
            lambda: _UdpProtocol(self), local_addr=(self.cfg.host, self.cfg.udp_port))
        # Port 0 means "any free port" (tests); publish the real ones.
        self.cfg.port = self._server.sockets[0].getsockname()[1]
        self.cfg.udp_port = self._udp.get_extra_info("sockname")[1]
        self._tasks += [asyncio.create_task(self._realtime_loop()),
                        asyncio.create_task(self._housekeeping_loop())]
        if self.cfg.announce:
            await self._announce()
        log.info("emisor %s escuchando ws://%s:%d%s y udp %d (sesión %s, key %d)", self.id,
                 self.cfg.host, self.cfg.port, proto.WS_PATH, self.cfg.udp_port, self.session_id,
                 self.session_key)

    async def stop(self, reason: str = "shutdown") -> None:
        await self.withdraw()
        for s in list(self.sessions):
            self._send(s, {"type": "bye", "reason": reason})
        await asyncio.sleep(0.05)
        for t in self._tasks:
            t.cancel()
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        if self._udp:
            self._udp.close()

    async def _announce(self) -> None:
        try:
            from zeroconf import IPVersion
            from zeroconf.asyncio import AsyncServiceInfo, AsyncZeroconf
        except ImportError:
            log.warning("zeroconf no instalado: sin anuncio DNS-SD")
            return
        local_only = self.cfg.scope == "local" or self.cfg.host.startswith("127.")
        if local_only:
            addrs = ["127.0.0.1"]
        elif self.cfg.host in ("0.0.0.0", ""):
            addrs = local_ipv4_addresses()
        else:
            addrs = [self.cfg.host]
        props = {
            "v": proto.VERSION, "role": self.cfg.role, "id": self.id, "origin": self.origin,
            "hops": str(self.hops), "path": ",".join(self.path), "name": self.cfg.name,
            "profiles": ",".join(map(str, self.cfg.profiles)), "security": self.cfg.security,
            "scope": "local" if local_only else "lan",
        }
        # Own host name: the machine name may also be answered by the OS responder with other
        # addresses (for example Windows and WSL share it), so a unique name avoids mixing them.
        host = "vertexav-" + self.id.replace("vx-", "")
        info = AsyncServiceInfo(
            proto.SERVICE_TYPE, f"{self.cfg.name}.{proto.SERVICE_TYPE}", port=self.cfg.port,
            properties=props, server=f"{host}.local.", parsed_addresses=addrs)
        try:
            interfaces = addrs if local_only else None
            self._zc = AsyncZeroconf(ip_version=IPVersion.V4Only,
                                     **({"interfaces": interfaces} if interfaces else {}))
            await self._zc.async_register_service(info, allow_name_change=True)
            self._zc_info = info
            log.info("anunciado DNS-SD '%s' en %s", info.name, ", ".join(addrs))
        except Exception as e:  # mDNS can fail on loopback-only setups
            log.warning("no se pudo anunciar por DNS-SD: %s", e)
            self._zc = None

    async def withdraw(self) -> None:
        """Remove the DNS-SD announcement (goodbye with TTL 0)."""
        if self._zc and self._zc_info:
            try:
                await self._zc.async_unregister_service(self._zc_info)
                await self._zc.async_close()
            except Exception:
                pass
        self._zc = self._zc_info = None

    async def reannounce(self) -> None:
        if self.cfg.announce and self._zc is None:
            await self._announce()

    # ------------------------------------------------------------------ state helpers
    @staticmethod
    def _empty_deck(d: int) -> dict:
        return {
            "deck": d, "track_id": None, "loaded": False, "playing": False, "position_s": 0.0,
            "position_beat": 0.0, "rate": 1.0, "bpm": None, "key": None,
            "loop": {"active": False, "in_s": None, "out_s": None}, "on_air": False,
            "audible": 0.0, "fader": 0.0,
            "eq": {"low": 0.5, "mid": 0.5, "high": 0.5, "low_kill": False, "mid_kill": False,
                   "high_kill": False},
            "filter": 0.0, "pfl": False, "scratching": False,
            "slip": {"active": False, "position_s": None},
        }

    def now(self) -> int:
        return self.clock.now()

    def _throttle(self, key: str, mtype: str) -> ObjectThrottle:
        th = self._throttles.get(key)
        if th is None:
            th = self._throttles[key] = ObjectThrottle(CONTINUOUS.get(mtype, set()))
        return th

    # ------------------------------------------------------------------ publishing API
    def set_tempo(self, tempo: Tempo) -> None:
        """Publish a new tempo/anchor (control + immediate UDP), §9.3."""
        now = self.now()
        self.tempo = replace(tempo)
        if tempo.phase_reset:
            self._phase_reset_until = now + 250_000
        msg = {"type": "tempo", "t": now, **tempo.to_json()}
        self._publish(msg)
        self._send_udp_tempo(now)
        self.tempo.phase_reset = False

    def update_deck(self, deck: int, **fields: Any) -> None:
        if deck not in self.decks:
            self.decks[deck] = self._empty_deck(deck)
        _deep_merge(self.decks[deck], fields)
        self._throttled(f"deck:{deck}", "deck.state", fields, {"deck": deck})

    def update_mix(self, **fields: Any) -> None:
        _deep_merge(self.mix, fields)
        self._throttled("mix", "mix.state", fields, {})

    def update_intent(self, **fields: Any) -> None:
        _deep_merge(self.intent, fields)
        self._throttled("intent", "intent", fields, {})

    def _throttled(self, key: str, mtype: str, fields: dict, ident: dict) -> None:
        th = self._throttle(key, mtype)
        out = th.offer(_flatten(fields), time.monotonic())
        if out:
            self._publish({"type": mtype, "t": self.now(), **ident, **_unflatten(out)})

    def track_loaded(self, deck: int, track_id: str, meta: dict, structure_available: bool = False,
                     artwork: Optional[dict] = None) -> None:
        msg = {"deck": deck, "track_id": track_id, "meta": meta, "artwork": artwork,
               "structure_available": structure_available}
        self.tracks[deck] = msg
        self.decks.setdefault(deck, self._empty_deck(deck))
        self.decks[deck].update({"track_id": track_id, "loaded": True})
        self._publish({"type": "track.loaded", "t": self.now(), **msg})
        self.update_deck(deck, track_id=track_id, loaded=True, bpm=meta.get("bpm"), key=meta.get("key"))

    def track_unloaded(self, deck: int) -> None:
        self.tracks.pop(deck, None)
        self._publish({"type": "track.unloaded", "t": self.now(), "deck": deck})
        self.update_deck(deck, track_id=None, loaded=False, playing=False)

    def track_structure(self, structure: dict) -> None:
        self.structures[structure["track_id"]] = structure
        loaded = {tr["track_id"] for tr in self.tracks.values()}
        for tid in list(self.structures):
            if tid not in loaded and tid != structure["track_id"]:
                del self.structures[tid]
        self._publish({"type": "track.structure", "t": self.now(), **structure})

    def event(self, name: str, at: Optional[int] = None, deck: Optional[int] = None,
              data: Optional[dict] = None) -> str:
        now = self.now()
        self._event_counter += 1
        eid = f"e-{self._event_counter}"
        msg = {"type": "event", "t": now, "at": now if at is None else int(at), "event_id": eid,
               "name": name, "deck": deck, "data": data or {}}
        if msg["at"] > now:
            self.events[eid] = msg
        self._publish(msg)
        return eid

    def cancel_event(self, event_id: str, reason: str) -> None:
        self.events.pop(event_id, None)
        self._publish({"type": "event.cancel", "t": self.now(), "event_id": event_id, "reason": reason})

    def trigger(self, name: str, state: str = "momentary", value: Optional[float] = None,
                deck: Optional[int] = None, at: Optional[int] = None) -> None:
        now = self.now()
        self._publish({"type": "trigger", "t": now, "at": now if at is None else at, "name": name,
                       "state": state, "value": value, "deck": deck})

    def snapshot(self) -> dict:
        now = self.now()
        return {
            "type": "snapshot", "t": now,
            "tempo": self.tempo.to_json() | {"phase_reset": False},
            "decks": [dict(d) for d in self.decks.values()],
            "mix": json.loads(json.dumps(self.mix)),
            "tracks": list(self.tracks.values()),
            "structures": [s for s in self.structures.values()],
            "intent": dict(self.intent) or None,
            "events": [e for e in self.events.values() if e["at"] >= now],
        }

    # ------------------------------------------------------------------ sending
    def _publish(self, msg: dict) -> None:
        topic = TOPIC_OF.get(msg["type"])
        for s in self.sessions:
            if s.ready and (topic is None or topic in s.topics):
                self._send(s, msg)

    def _send(self, s: Session, msg: dict) -> None:
        s.json_seq += 1
        text = dumps({**msg, "seq": s.json_seq} if msg["type"] not in ("bye", "error") else msg)
        if len(text.encode("utf-8")) > proto.MAX_MSG_EMITTER_TO_RECEIVER:
            log.error("mensaje %s de %d bytes supera el límite de 512 KB; no se envía", msg["type"], len(text))
            return
        try:
            s.queue.put_nowait(text)
        except asyncio.QueueFull:
            log.warning("receptor %s demasiado lento; se cierra", s.name)
            asyncio.create_task(s.ws.close(1008, "receiver too slow"))

    async def _writer(self, s: Session) -> None:
        try:
            while True:
                text = await s.queue.get()
                await s.ws.send(text)
                s.last_tx = time.monotonic()
        except (ConnectionClosed, asyncio.CancelledError):
            pass

    def _udp_send(self, s: Session, kind: int, t: int, body: bytes) -> None:
        if not self._udp or not s.udp_addr:
            return
        seq = s.udp_seq.get(kind, 0) + 1
        s.udp_seq[kind] = seq
        self._udp.sendto(proto.pack_packet(kind, seq, self.session_key, t, body, s.mac_key), s.udp_addr)

    def _tempo_packet(self, now: int) -> TempoPacket:
        tp = self.tempo
        return TempoPacket(
            bpm=tp.bpm, anchor_t=tp.anchor_t, anchor_beat=tp.anchor_beat,
            next_beat_t=tp.next_beat_t(now), next_downbeat_t=tp.next_downbeat_t(now),
            beats_per_bar=tp.beats_per_bar, source_deck=tp.source_deck or 0, playing=tp.playing,
            phase_reset=now < self._phase_reset_until, stability=tp.stability,
            link_aligned=bool(tp.link_aligned), confidence=tp.confidence)

    def _send_udp_tempo(self, now: int) -> None:
        self._last_udp_tempo = now
        targets = [s for s in self.sessions if s.ready and "tempo" in s.topics]
        if not targets:
            return
        pkt = self._tempo_packet(now)
        body = pkt.pack_body()
        for s in targets:
            if s.rt_over_control:
                self._send(s, {"type": "tempo.rt", "t": now, "bpm": pkt.bpm, "anchor_t": pkt.anchor_t,
                               "anchor_beat": pkt.anchor_beat, "next_beat_t": pkt.next_beat_t,
                               "next_downbeat_t": pkt.next_downbeat_t, "playing": pkt.playing,
                               "stability": pkt.stability, "phase_reset": pkt.phase_reset})
            else:
                self._udp_send(s, KIND_TEMPO, now, body)

    async def _realtime_loop(self) -> None:
        """UDP tempo at 20 Hz while playing (2 Hz when stopped) plus one packet per beat, sent
        lookahead_us before it (§9.3)."""
        while True:
            now = self.now()
            tp = self.tempo
            period = 50_000 if tp.playing else 500_000
            due = now - self._last_udp_tempo >= period
            if tp.playing and tp.stability != "unstable":
                nb = tp.next_beat_t(now)
                if nb - now <= self.cfg.lookahead_us and nb != self._last_beat_announced:
                    self._last_beat_announced = nb
                    due = True
            if due:
                self._send_udp_tempo(now)
            await asyncio.sleep(0.004)

    async def _housekeeping_loop(self) -> None:
        while True:
            mono = time.monotonic()
            for key, th in self._throttles.items():
                out = th.poll(mono)
                if out:
                    mtype = "deck.state" if key.startswith("deck:") else ("mix.state" if key == "mix" else "intent")
                    ident = {"deck": int(key.split(":")[1])} if key.startswith("deck:") else {}
                    self._publish({"type": mtype, "t": self.now(), **ident, **_unflatten(out)})
            now = self.now()
            for eid in [e for e, m in self.events.items() if m["at"] < now - 1_000_000]:
                del self.events[eid]
            for s in list(self.sessions):
                if mono - s.last_tx >= proto.HEARTBEAT_INTERVAL_S:
                    self._send(s, {"type": "heartbeat", "t": now})
                    s.last_tx = mono
                if mono - s.last_rx > proto.HEARTBEAT_TIMEOUT_S:
                    log.info("receptor %s sin latido; se cierra", s.name)
                    asyncio.create_task(s.ws.close(1001, "heartbeat timeout"))
                    s.last_rx = mono  # avoid repeated closes
            await asyncio.sleep(0.01)

    # ------------------------------------------------------------------ control channel
    def _process_request(self, connection: ServerConnection, request):
        path = request.path.split("?")[0]
        if path != proto.WS_PATH:
            return connection.respond(404, "Not found\n")
        return None

    def _locked_out(self, ip: str) -> bool:
        now = time.monotonic()
        fails = [t for t in self._auth_failures.get(ip, []) if now - t < 30.0]
        self._auth_failures[ip] = fails
        return len(fails) >= 5

    async def _recv_json(self, ws: ServerConnection, timeout: float) -> Optional[dict]:
        raw = await asyncio.wait_for(ws.recv(), timeout=max(0.01, timeout))
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", errors="replace")
        if proto.json_depth_exceeds(raw):
            raise _TooDeep()
        msg = json.loads(raw)
        return msg if isinstance(msg, dict) else None

    async def _fatal(self, ws: ServerConnection, code: str, message: str) -> None:
        try:
            await ws.send(dumps({"type": "error", "code": code, "message": message, "fatal": True}))
            await ws.close(1008, code)
        except ConnectionClosed:
            pass

    async def _handle(self, ws: ServerConnection) -> None:
        ip = ws.remote_address[0] if ws.remote_address else "?"
        start = time.monotonic()
        if self._unauth.get(ip, 0) >= proto.MAX_UNAUTH_PER_IP or sum(self._unauth.values()) >= proto.MAX_UNAUTH_TOTAL:
            await ws.close(1013, "too many pending connections")
            return
        self._unauth[ip] = self._unauth.get(ip, 0) + 1
        authed = False
        session: Optional[Session] = None
        try:
            try:
                hello = await self._recv_json(ws, proto.HELLO_TIMEOUT_S - (time.monotonic() - start))
            except (asyncio.TimeoutError, json.JSONDecodeError, _TooDeep, UnicodeDecodeError):
                await ws.close(1008)
                return
            if not hello or hello.get("type") != "hello":
                await ws.close(1008)  # before auth only hello / auth.response are accepted
                return
            version = str(hello.get("version", ""))
            if version.split(".")[0] != proto.VERSION.split(".")[0] or hello.get("protocol") != "vertexav":
                await self._fatal(ws, "unsupported_version", f"Versión {version} no soportada")
                return
            client_id = hello.get("client_id")
            profiles = hello.get("profiles")
            if not isinstance(client_id, str) or not isinstance(profiles, list) or not isinstance(hello.get("name"), str):
                await ws.close(1008)
                return
            session = Session(ws=ws, ip=ip, client_id=client_id, name=hello["name"],
                              role=hello.get("role", "receiver"))
            server_proof_b64 = None
            if self.cfg.security != "open":
                if self._locked_out(ip):
                    await ws.close(1008)
                    return
                psk = self.cfg.keys.get(client_id, self.cfg.psk)
                if psk is None:
                    await self._fatal(ws, "auth_required", "Receptor no emparejado")
                    return
                server_nonce = security.new_nonce()
                await ws.send(dumps({"type": "auth.challenge", "server_nonce": security.b64(server_nonce)}))
                remaining = min(proto.AUTH_RESPONSE_TIMEOUT_S,
                                proto.WELCOME_TIMEOUT_S - (time.monotonic() - start))
                try:
                    resp = await self._recv_json(ws, remaining)
                except (asyncio.TimeoutError, json.JSONDecodeError, _TooDeep, UnicodeDecodeError):
                    await ws.close(1008)
                    return
                if not resp or resp.get("type") != "auth.response":
                    await ws.close(1008)
                    return
                try:
                    client_nonce = security.unb64(resp.get("client_nonce", ""))
                    proof = security.unb64(resp.get("proof", ""))
                except Exception:
                    client_nonce, proof = b"", b""
                expected = security.client_proof(psk, server_nonce, client_nonce, client_id)
                if len(client_nonce) != security.NONCE_LEN or not security.equal(expected, proof):
                    self._auth_failures.setdefault(ip, []).append(time.monotonic())
                    await self._fatal(ws, "auth_failed", "Prueba de autenticación incorrecta")
                    return
                server_proof_b64 = security.b64(security.server_proof(psk, client_nonce, server_nonce, self.session_id))
                session.mac_key = security.mac_key(psk, server_nonce, client_nonce)
            if time.monotonic() - start > proto.WELCOME_TIMEOUT_S:
                await ws.close(1008)
                return
            if len(self.sessions) >= self.cfg.max_receivers:
                await self._fatal(ws, "internal", "Demasiados receptores")
                return
            # Authenticated: lift limits to the session values (§15.7)
            authed = True
            self._unauth[ip] -= 1
            ws.protocol.max_message_size = proto.MAX_MSG_RECEIVER_TO_EMITTER
            session.profiles = sorted(set(p for p in profiles if isinstance(p, int)) & set(self.cfg.profiles))
            session.latency_us = int(hello.get("latency_us") or 0)
            session.rt_over_control = bool(hello.get("realtime_over_control"))
            if isinstance(hello.get("udp_port"), int):
                session.udp_addr = (ip, hello["udp_port"])
            session.topics = {t for t, p in TOPICS.items() if p in session.profiles}
            welcome = {
                "type": "welcome", "version": proto.VERSION, "session_id": self.session_id,
                "session_key": self.session_key,
                "emitter": {"id": self.id, "name": self.cfg.name, "role": self.cfg.role, "path": self.path},
                "security": self.cfg.security, "server_proof": server_proof_b64,
                "profiles": session.profiles, "udp_port": self.cfg.udp_port, "multicast": None,
                "decks": len(self.decks), "lookahead_us": self.cfg.lookahead_us,
                "link": {"enabled": self.cfg.link_enabled},
            }
            await ws.send(dumps(welcome))
            self.sessions.append(session)
            log.info("receptor conectado: %s (%s) desde %s perfiles %s", session.name, client_id, ip, session.profiles)
            writer = asyncio.create_task(self._writer(session))
            auto_snapshot = asyncio.get_running_loop().call_later(1.0, self._send_snapshot, session)
            try:
                await self._session_loop(session, auto_snapshot)
            finally:
                auto_snapshot.cancel()
                writer.cancel()
        except ConnectionClosed:
            pass
        finally:
            if not authed:
                self._unauth[ip] = max(0, self._unauth.get(ip, 1) - 1)
            if session in self.sessions:
                self.sessions.remove(session)
                log.info("receptor desconectado: %s", session.name)

    def _send_snapshot(self, s: Session) -> None:
        if s.ready:
            return
        s.ready = True
        self._send(s, self.snapshot())

    async def _session_loop(self, s: Session, auto_snapshot: asyncio.TimerHandle) -> None:
        async for raw in s.ws:
            mono = time.monotonic()
            s.last_rx = mono
            s.rx_times = [t for t in s.rx_times if mono - t < 1.0] + [mono]
            if len(s.rx_times) > 50:
                if len(s.rx_times) == 51:
                    self._send(s, {"type": "error", "code": "rate_limited",
                                   "message": "Más de 50 mensajes por segundo", "fatal": False})
                continue
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8", errors="replace")
            if proto.json_depth_exceeds(raw):
                await self._fatal(s.ws, "message_too_large", "Anidamiento JSON excesivo")
                return
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                self._send(s, {"type": "error", "code": "bad_message", "message": "JSON mal formado", "fatal": False})
                continue
            if not isinstance(msg, dict) or not isinstance(msg.get("type"), str):
                self._send(s, {"type": "error", "code": "bad_message", "message": "Falta type", "fatal": False})
                continue
            mtype = msg["type"]
            if mtype == "subscribe":
                topics = msg.get("topics")
                if not isinstance(topics, list):
                    self._send(s, {"type": "error", "code": "bad_message", "message": "topics debe ser una lista", "fatal": False})
                    continue
                allowed = set()
                for t in topics:
                    if t in TOPICS and TOPICS[t] in s.profiles:
                        allowed.add(t)
                    elif t in TOPICS:
                        self._send(s, {"type": "error", "code": "unsupported_profile",
                                       "message": f"El tema {t} exige el perfil {TOPICS[t]}", "fatal": False})
                s.topics = allowed
                auto_snapshot.cancel()
                s.ready = False  # a new subscribe gets a fresh snapshot
                self._send_snapshot(s)
            elif mtype == "clock.ping":
                t1 = self.now()
                self._send(s, {"type": "clock.pong", "t0": msg.get("t0"), "ping_id": msg.get("ping_id"),
                               "t1": t1, "t2": self.now()})
            elif mtype == "latency.update" and isinstance(msg.get("latency_us"), int):
                s.latency_us = msg["latency_us"]
            elif mtype == "receiver.state":
                s.receiver_state = msg
            elif mtype == "receiver.capabilities":
                s.capabilities = msg.get("triggers")
            elif mtype == "audio.subscribe":
                self._send(s, {"type": "error", "code": "unsupported_transport",
                               "message": "Este emisor no ofrece audio (perfil 3)", "fatal": False})
            elif mtype == "bye":
                await s.ws.close(1000)
                return
            elif mtype == "error" and msg.get("fatal"):
                log.warning("error fatal del receptor %s: %s", s.name, msg.get("code"))
                await s.ws.close(1000)
                return
            elif mtype in ("hello", "auth.response"):
                self._send(s, {"type": "error", "code": "bad_message", "message": f"{mtype} inesperado", "fatal": False})
            if self.on_receiver_message:
                self.on_receiver_message(s, msg)

    # ------------------------------------------------------------------ UDP
    def _on_datagram(self, data: bytes, addr: tuple) -> None:
        t1 = self.now()  # take t1 as early as possible (§7.2)
        # The receiver sends pings from the socket bound to the udp_port it gave in hello, so
        # (ip, port) identifies the session and therefore its mac_key.
        session = next((s for s in self.sessions if s.udp_addr == addr), None)
        if session is None:
            return
        try:
            hdr, body = proto.validate_datagram(data, self.session_key, session.mac_key,
                                                require_signature=self.cfg.security != "open")
        except PacketError:
            return
        if hdr.kind != KIND_CLOCK_PING or not session.ping_window.accept(hdr.seq):
            return
        t0, ping_id = proto.PING_BODY.unpack(body)
        if self._udp is None:
            return
        seq = session.udp_seq.get(KIND_CLOCK_PONG, 0) + 1
        session.udp_seq[KIND_CLOCK_PONG] = seq
        t2 = self.now()
        pong = PONG_BODY.pack(t0, ping_id, t1, t2)
        self._udp.sendto(proto.pack_packet(KIND_CLOCK_PONG, seq, self.session_key, t2, pong, session.mac_key), addr)


class _TooDeep(Exception):
    pass


class _UdpProtocol(asyncio.DatagramProtocol):
    def __init__(self, emitter: Emitter):
        self.emitter = emitter

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        self.emitter._on_datagram(data, addr)

    def error_received(self, exc: Exception) -> None:  # e.g. ICMP port unreachable on Windows
        pass
