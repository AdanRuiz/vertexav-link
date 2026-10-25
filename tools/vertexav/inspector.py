"""VertexAV Link inspector: connects as a receiver, shows everything and validates it.

Run: python -m vertexav.inspector --help
"""
from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import sys
import time
from collections import Counter, defaultdict, deque
from typing import Any, Optional

from . import proto, security
from .clock import enable_fine_timers
from .proto import KIND_CLOCK_PONG, KIND_NAMES, KIND_TEMPO, PacketError, TempoPacket
from .receiver import Receiver, ReceiverConfig
from .schema import Issue, SessionChecker, validate_message
from .tempo import Tempo
from .util import C, enable_ansi

HIDDEN = {"heartbeat"}
COLORS = {"tempo": C.BLUE, "event": C.GREEN, "event.cancel": C.YELLOW, "trigger": C.MAGENTA,
          "track.loaded": C.CYAN, "track.structure": C.CYAN, "welcome": C.BOLD, "snapshot": C.BOLD,
          "error": C.RED}


def _compact(d: dict, skip=("type", "t", "seq", "deck")) -> str:
    parts = []
    for k, v in d.items():
        if k in skip:
            continue
        if isinstance(v, dict):
            v = "{" + _compact(v, ()) + "}"
        elif isinstance(v, float):
            v = f"{v:.3f}".rstrip("0").rstrip(".")
        elif isinstance(v, list):
            v = "[" + ",".join(map(str, v[:6])) + ("…" if len(v) > 6 else "") + "]"
        parts.append(f"{k}={v}")
    return " ".join(parts)


def summarize(msg: dict) -> str:
    t = msg.get("type")
    if t == "tempo":
        flags = []
        if msg.get("phase_reset"):
            flags.append("PHASE_RESET")
        if msg.get("link_aligned") is not None:
            flags.append(f"link_aligned={msg['link_aligned']}")
        return (f"{msg.get('bpm', 0):.2f} BPM ancla beat {msg.get('anchor_beat', 0):.2f} deck {msg.get('source_deck')} "
                f"{'▶' if msg.get('playing') else '■'} {msg.get('stability')} {' '.join(flags)}")
    if t == "deck.state":
        return f"deck {msg.get('deck')}: {_compact(msg)}"
    if t in ("mix.state", "intent"):
        return _compact(msg)
    if t == "event":
        lead = (msg.get("at", 0) - msg.get("t", msg.get("at", 0))) / 1000
        data = _compact(msg.get("data") or {}, ())
        return f"{msg.get('name')} deck {msg.get('deck')} {msg.get('event_id')} en +{lead:.0f} ms {data}"
    if t == "event.cancel":
        return f"{msg.get('event_id')} ({msg.get('reason')})"
    if t == "trigger":
        return f"{msg.get('name')} {msg.get('state')} value={msg.get('value')} deck={msg.get('deck')}"
    if t == "track.loaded":
        m = msg.get("meta") or {}
        return f"deck {msg.get('deck')}: {m.get('artist')} – {m.get('title')} ({m.get('bpm')} BPM, {m.get('key')}) {msg.get('track_id')}"
    if t == "track.structure":
        secs = msg.get("sections") or []
        return f"{msg.get('track_id')}: " + " ".join(f"{s.get('role')}[{s.get('start_beat')}-{s.get('end_beat')}]" for s in secs)
    if t == "snapshot":
        tp = msg.get("tempo") or {}
        return (f"tempo {tp.get('bpm')} BPM, {len(msg.get('decks') or [])} decks, {len(msg.get('tracks') or [])} temas, "
                f"{len(msg.get('structures') or [])} mapas, {len(msg.get('events') or [])} eventos pendientes")
    if t == "welcome":
        em = msg.get("emitter") or {}
        return (f"{em.get('name')} ({em.get('id')}, {em.get('role')}, path {em.get('path')}) sesión {msg.get('session_id')} "
                f"key {msg.get('session_key')} perfiles {msg.get('profiles')} seguridad {msg.get('security')} "
                f"lookahead {msg.get('lookahead_us')} µs")
    if t == "error":
        return f"{msg.get('code')}: {msg.get('message')} fatal={msg.get('fatal')}"
    return _compact(msg, ("type", "seq"))


class PacketChecker:
    """Checks on the real-time channel: tempo consistency, rates, loss and beat accuracy."""

    def __init__(self, ins: "Inspector"):
        self.ins = ins
        self.counts: Counter = Counter()
        self.errors_by_step: Counter = Counter()
        self.last_tempo_arrival: Optional[int] = None
        self.max_gap_us = 0
        self.gap_warnings = 0
        self.beat_errors: deque = deque(maxlen=2000)  # µs, follower vs announced
        self.latencies: deque = deque(maxlen=2000)  # µs, one-way estimate
        self.lookahead_hits = 0
        self.beats_seen: set = set()
        self.json_mismatch = 0
        self.last_print_beat: Optional[int] = None

    def on_packet(self, hdr: proto.Header, pkt: Any, t3: int) -> None:
        rx = self.ins.rx
        self.counts[hdr.kind] += 1
        if hdr.kind != KIND_TEMPO or rx.sync.offset is None:
            return
        assert isinstance(pkt, TempoPacket)
        em_now = rx.sync.to_emitter(t3)
        self.latencies.append(em_now - hdr.t)
        tp = Tempo(bpm=pkt.bpm, anchor_t=pkt.anchor_t, anchor_beat=pkt.anchor_beat,
                   beats_per_bar=pkt.beats_per_bar)
        issues = []
        if pkt.playing and pkt.stability != "unstable":
            nb_beat = tp.beat_at(pkt.next_beat_t)
            if abs(nb_beat - round(nb_beat)) > 0.002:
                issues.append(Issue("error", f"next_beat_t no cae en un beat entero (beat {nb_beat:.4f})", "§9.2"))
            if not 0 < pkt.next_beat_t - hdr.t <= tp.beat_us + 1000:
                issues.append(Issue("error", f"next_beat_t - t = {(pkt.next_beat_t - hdr.t)/1000:.1f} ms fuera de (0, periodo]", "§9.2"))
            nd_beat = tp.beat_at(pkt.next_downbeat_t)
            if abs(nd_beat - round(nd_beat)) > 0.002 or not 0 <= round(nd_beat) - round(nb_beat) < max(1, pkt.beats_per_bar):
                issues.append(Issue("error", "next_downbeat_t incoherente con next_beat_t y beats_per_bar", "§9.2"))
            jt = self.ins.rx.tempo
            if jt is not None and jt.anchor_t <= hdr.t:
                diff = jt.beat_at(hdr.t) - tp.beat_at(hdr.t)
                if abs(diff) > 0.01 and jt.stability != "unstable":
                    self.json_mismatch += 1
                    if self.json_mismatch <= 5:
                        issues.append(Issue("warn", f"tempo UDP y tempo JSON difieren {diff:.3f} beats", "§9.2"))
            # Beat accuracy: where the smoothed follower places the announced beat.
            fb = rx.follower
            idx = round(nb_beat)
            if fb.model is not None and rx.follower.beats_enabled(em_now):
                predicted = fb.model.time_of_beat(idx + fb._corr_at(em_now))
                self.beat_errors.append(predicted - pkt.next_beat_t)
            lead = pkt.next_beat_t - hdr.t
            la = (rx.welcome or {}).get("lookahead_us") or 0
            if la and abs(lead - la) <= 15_000 and idx not in self.beats_seen:
                self.lookahead_hits += 1
                self.beats_seen.add(idx)
                if len(self.beats_seen) > 4000:
                    self.beats_seen = set(sorted(self.beats_seen)[-1000:])
            # Rate: at least 20 Hz while playing
            if self.last_tempo_arrival is not None:
                gap = t3 - self.last_tempo_arrival
                self.max_gap_us = max(self.max_gap_us, gap)
                if gap > 100_000:
                    self.gap_warnings += 1
                    if self.gap_warnings <= 5:
                        issues.append(Issue("warn", f"hueco de {gap/1000:.0f} ms entre paquetes tempo (mín. 20 Hz)", "§9.3"))
            if self.ins.args.udp:
                self.ins.line("udp", f"tempo seq {hdr.seq} {pkt.bpm:.2f} BPM beat {tp.beat_at(hdr.t):.3f} "
                              f"next +{(pkt.next_beat_t - hdr.t)/1000:.1f} ms {pkt.stability}"
                              f"{' PHASE_RESET' if pkt.phase_reset else ''}", C.DIM)
        self.last_tempo_arrival = t3 if pkt.playing else None
        for i in issues:
            self.ins.report(i, f"udp tempo seq {hdr.seq}")

    def on_error(self, err: PacketError, data: bytes) -> None:
        self.errors_by_step[err.step] += 1
        n = sum(self.errors_by_step.values())
        if n <= 20 or self.ins.args.verbose:
            self.ins.report(Issue("error" if err.step not in (7,) else "warn", f"paquete UDP descartado, {err}", "§8.3"), "udp")


class Inspector:
    def __init__(self, args: argparse.Namespace, rx: Receiver):
        self.args = args
        self.rx = rx
        self.checker = SessionChecker()
        self.packets = PacketChecker(self)
        self.counts: Counter = Counter()
        self.issues: Counter = Counter()
        self.issue_texts: Counter = Counter()
        self.start = time.monotonic()
        self.log = open(args.log, "a", encoding="utf-8") if args.log else None
        rx.on_message = self.on_message
        rx.on_sent = self.on_sent
        rx.on_packet = self.packets.on_packet
        rx.on_packet_error = self.packets.on_error
        rx.on_status = lambda s: self.line("estado", s, C.BOLD)

    def ts(self) -> str:
        return f"{time.monotonic() - self.start:8.3f}"

    def line(self, tag: str, text: str, color: str = "") -> None:
        print(f"{C.c(C.DIM, self.ts())} {C.c(color, tag) if color else tag} {text}", flush=True)

    def report(self, issue: Issue, where: str) -> None:
        self.issues[issue.level] += 1
        self.issue_texts[f"{issue.level}: {issue.msg.split(' (')[0][:90]} ({issue.ref})"] += 1
        color = C.RED if issue.level == "error" else C.YELLOW
        print(f"{C.c(C.DIM, self.ts())} {C.c(color, '  ↳ ' + str(issue))} [{where}]", flush=True)

    def _record(self, direction: str, msg: dict) -> None:
        if self.log:
            self.log.write(json.dumps({"rx_us": self.rx.local_now(), "dir": direction, "msg": msg},
                                      ensure_ascii=False) + "\n")
            self.log.flush()  # readable while the session is being recorded

    def on_sent(self, msg: dict) -> None:
        self.checker.on_sent(msg)
        self._record("out", msg)
        issues = validate_message(msg, "r2e")
        for i in issues:
            self.report(i, f"enviado {msg.get('type')}")
        if self.args.raw:
            self.line("→", json.dumps(msg, ensure_ascii=False), C.DIM)
        elif not self.args.quiet and msg.get("type") not in HIDDEN:
            self.line("→ " + str(msg.get("type")), _compact(msg, ("type",)), C.DIM)

    def on_message(self, msg: Any, raw: str) -> None:
        if not isinstance(msg, dict):
            self.report(Issue("error", "mensaje que no es objeto JSON", "§8.1"), "control")
            return
        mtype = msg.get("type")
        if mtype == "welcome":
            # New connection: seq and session state start again.
            self.checker = SessionChecker()
            if self.args.topics:
                self.checker.on_sent({"type": "subscribe", "topics": self.args.topics.split(",")})
        self.counts[mtype] += 1
        self._record("in", msg)
        issues = validate_message(msg, "e2r") + self.checker.check(msg, time.monotonic())
        show = (self.args.show is None or mtype in self.args.show) and (mtype not in HIDDEN or self.args.verbose)
        if self.args.quiet:
            show = mtype in ("welcome", "error") or bool(issues)
        if show:
            if self.args.raw:
                self.line(str(mtype), raw, COLORS.get(mtype, ""))
            else:
                self.line(str(mtype), summarize(msg), COLORS.get(mtype, ""))
        for i in issues:
            self.report(i, str(mtype))

    async def status_loop(self) -> None:
        last = Counter()
        while True:
            await asyncio.sleep(self.args.status_every)
            rx = self.rx
            now = rx.emitter_now()
            parts = []
            if rx.sync.offset is not None:
                parts.append(f"reloj offset {rx.sync.offset/1000:+.3f} ms rtt_min {rx.sync.rtt_min/1000:.3f} ms")
                if rx.sync.network_unfit(rx.local_now()):
                    parts.append(C.c(C.RED, "RED NO APTA (rtt_min > 20 ms)"))
            fb = rx.follower
            if fb.model is not None and now is not None:
                b = fb.beat_at(now)
                tp = fb.model
                bar = int((b - tp.bar_offset) // tp.beats_per_bar) + 1
                beat = int((b - tp.bar_offset) % tp.beats_per_bar) + 1
                parts.append(f"{tp.bpm:.2f} BPM compás {bar}.{beat} {'▶' if tp.playing else '■'} {tp.stability}")
            rates = []
            for kind, n in self.packets.counts.items():
                rates.append(f"{KIND_NAMES.get(kind, kind)} {(n - last[kind]) / self.args.status_every:.0f}/s")
            last = Counter(self.packets.counts)
            if rates:
                parts.append("udp " + ", ".join(rates))
            sec = rx.current_section()
            if sec:
                parts.append(f"sección {sec['role']} ({sec['index']})")
            if rx.intent:
                parts.append(f"energía {rx.intent.get('energy')}")
            if self.packets.beat_errors:
                errs = [abs(e) for e in self.packets.beat_errors]
                parts.append(f"error beat p95 {statistics.quantiles(errs, n=20)[-1]/1000:.2f} ms" if len(errs) > 20 else "")
            self.line("estado", " | ".join(p for p in parts if p), C.CYAN)

    def final_report(self) -> int:
        p = self.packets
        rx = self.rx
        print()
        print(C.c(C.BOLD, "=== Informe del inspector ==="))
        print(f"Duración: {time.monotonic() - self.start:.1f} s, reconexiones: {rx.stats['reconnects']}")
        print("Mensajes de control: " + ", ".join(f"{k}={v}" for k, v in sorted(self.counts.items(), key=lambda x: str(x[0]))))
        print("Paquetes UDP: " + ", ".join(f"{KIND_NAMES.get(k, k)}={v}" for k, v in p.counts.items()))
        lost = sum(w.gaps for w in rx.windows.values())
        dup = sum(w.dropped_dup for w in rx.windows.values())
        print(f"Pérdidas estimadas: {lost}, duplicados/desordenados: {dup}, descartados por paso: {dict(p.errors_by_step)}")
        if rx.sync.offset is not None:
            print(f"Reloj: offset {rx.sync.offset/1000:+.3f} ms, rtt_min {rx.sync.rtt_min/1000:.3f} ms, muestras {rx.sync.total}")
        if p.latencies:
            lat = sorted(p.latencies)
            print(f"Latencia de entrega UDP estimada: mediana {lat[len(lat)//2]/1000:.2f} ms, máx {lat[-1]/1000:.2f} ms")
        if p.beat_errors:
            errs = sorted(abs(e) for e in p.beat_errors)
            print(f"Precisión del beat (seguidor suavizado vs next_beat_t): mediana {errs[len(errs)//2]/1000:.3f} ms, "
                  f"p95 {errs[int(len(errs)*0.95)]/1000:.3f} ms, máx {errs[-1]/1000:.3f} ms")
        print(f"Beats anunciados con lookahead: {p.lookahead_hits}, hueco máximo entre paquetes tempo: {p.max_gap_us/1000:.0f} ms")
        print(f"Seguidor de tempo: saltos {rx.follower.jumps}, correcciones suaves {rx.follower.smooth_corrections}")
        print(f"Problemas: {self.issues['error']} errores, {self.issues['warn']} avisos")
        for text, n in self.issue_texts.most_common(25):
            color = C.RED if text.startswith("error") else C.YELLOW
            print("  " + C.c(color, f"{n:5d} × {text}"))
        if self.log:
            self.log.close()
        return 1 if (self.args.strict and self.issues["error"]) else 0


async def discover(timeout: float) -> list[dict]:
    from zeroconf import ServiceStateChange
    from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

    found: dict[str, dict] = {}
    azc = AsyncZeroconf()
    pending = []

    def handler(zeroconf, service_type, name, state_change):
        if state_change is ServiceStateChange.Added:
            pending.append(name)

    browser = AsyncServiceBrowser(azc.zeroconf, [proto.SERVICE_TYPE], handlers=[handler])
    await asyncio.sleep(timeout)
    for name in pending:
        info = AsyncServiceInfo(proto.SERVICE_TYPE, name)
        if await info.async_request(azc.zeroconf, 1500):
            txt = {k.decode(): (v.decode() if v else "") for k, v in info.properties.items()}
            addrs = info.parsed_addresses()
            found[name] = {"name": name, "host": addrs[0] if addrs else None, "port": info.port, "txt": txt}
    await browser.async_cancel()
    await azc.async_close()
    out = [f for f in found.values() if int(f["txt"].get("hops", "0") or 0) <= proto.MAX_HOPS]
    return sorted(out, key=lambda f: int(f["txt"].get("hops", "0") or 0))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="python -m vertexav.inspector",
                                description="Receptor de pruebas: muestra y valida todo lo que envía un emisor VertexAV Link")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=proto.CONTROL_PORT)
    p.add_argument("--discover", action="store_true", help="buscar emisores por DNS-SD y conectar al primero")
    p.add_argument("--psk", help="código de emparejamiento (modo auth)")
    p.add_argument("--profiles", default="0,1,2")
    p.add_argument("--topics", help="lista de temas separados por comas (por defecto, todos los de los perfiles)")
    p.add_argument("--name", default="VertexAV Inspector")
    p.add_argument("--latency-ms", type=float, default=0.0)
    p.add_argument("--show", help="mostrar solo estos tipos de mensaje (separados por comas)")
    p.add_argument("--raw", action="store_true", help="mostrar el JSON completo")
    p.add_argument("--udp", action="store_true", help="mostrar cada paquete tempo UDP")
    p.add_argument("--no-udp", action="store_true", help="pedir el tiempo real por el canal de control")
    p.add_argument("--quiet", action="store_true", help="mostrar solo problemas y estado")
    p.add_argument("--status-every", type=float, default=2.0)
    p.add_argument("--log", help="grabar los mensajes en un archivo JSON Lines")
    p.add_argument("--duration", type=float, help="segundos de ejecución y salir con informe")
    p.add_argument("--strict", action="store_true", help="código de salida 1 si hubo errores")
    p.add_argument("--no-reconnect", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


async def amain(args: argparse.Namespace) -> int:
    if args.discover:
        print("Buscando emisores _vertexav._tcp…")
        found = await discover(2.5)
        for f in found:
            t = f["txt"]
            print(f"  {f['name']} → {f['host']}:{f['port']}  id={t.get('id')} role={t.get('role')} hops={t.get('hops')} "
                  f"perfiles={t.get('profiles')} seguridad={t.get('security')} scope={t.get('scope')}")
        if not found:
            print("No se encontró ningún emisor.")
            return 2
        args.host, args.port = found[0]["host"], found[0]["port"]
    args.show = set(args.show.split(",")) if args.show else None
    cfg = ReceiverConfig(host=args.host, port=args.port, name=args.name,
                         profiles=[int(x) for x in args.profiles.split(",")],
                         topics=args.topics.split(",") if args.topics else None,
                         latency_us=int(args.latency_ms * 1000),
                         psk=security.code_to_psk(args.psk) if args.psk else None,
                         realtime_over_control=args.no_udp, reconnect=not args.no_reconnect)
    rx = Receiver(cfg)
    ins = Inspector(args, rx)
    tasks = [asyncio.create_task(rx.run()), asyncio.create_task(ins.status_loop())]
    code = 0
    try:
        if args.duration:
            await asyncio.wait([tasks[0]], timeout=args.duration)
        else:
            await tasks[0]
    finally:
        # Also runs on Ctrl+C (the main task is cancelled), so the report is always printed.
        try:
            await rx.stop()
        except BaseException:
            pass
        for t in tasks:
            t.cancel()
        code = ins.final_report()
    return code


def main() -> None:
    args = build_parser().parse_args()
    enable_ansi()
    enable_fine_timers()
    try:
        code = asyncio.run(amain(args))
    except KeyboardInterrupt:
        code = 0
    sys.exit(code)


if __name__ == "__main__":
    main()
