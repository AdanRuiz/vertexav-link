"""Bridge VertexAV Link → OS2L (§16.1), for lighting software that already speaks OS2L (QLC+).

The bridge is a VertexAV receiver and an OS2L client. It sends:
  * `beat` on every beat at its exact instant (clock sync + latency compensation), with
    `change` = true after a jump and `strength` higher on the downbeat;
  * `btn` for each `trigger` (momentary → on, then off after 100 ms);
  * `btn` "vertexav.section.<role>" on when the deck on air enters a section, the previous
    section's button off;
  * extras (can be disabled with --no-extras): countdown, transition, mix cut, intensity,
    strobe_ok, mood buttons and the energy as `cmd` 1 (0-100).

Run: python -m vertexav.os2l --help
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import socket
import sys
from typing import Optional

from . import security
from .clock import enable_fine_timers
from .receiver import Receiver, ReceiverConfig
from .util import C, dumps, enable_ansi, jittered

log = logging.getLogger("vertexav.os2l")
OS2L_SERVICE = "_os2l._tcp.local."
ENERGY_CMD_ID = 1
MOMENTARY_S = 0.1


class Os2lClient:
    """TCP client with reconnection. Messages go out one by one, a few ms apart, because some
    OS2L servers parse each TCP read as a single JSON document."""

    GAP_S = 0.003

    def __init__(self, host: Optional[str], port: int, discover: bool, verbose: bool):
        self.host = host
        self.port = port
        self.discover = discover
        self.verbose = verbose
        self.queue: asyncio.Queue = asyncio.Queue(maxsize=500)
        self.writer: Optional[asyncio.StreamWriter] = None
        self.connected = asyncio.Event()
        self.sent = 0
        self.on_connect = None

    def send(self, msg: dict, urgent: bool = False) -> None:
        if not self.connected.is_set():
            return
        if urgent and self.writer is not None:
            # Beats go out immediately, ahead of queued buttons, to keep their timing.
            self.writer.write(dumps(msg).encode("utf-8"))
            self.sent += 1
            if self.verbose:
                print(C.c(C.DIM, f"[os2l] → {dumps(msg)}"), flush=True)
            return
        try:
            self.queue.put_nowait(msg)
        except asyncio.QueueFull:
            log.warning("cola OS2L llena; se descarta %s", msg.get("evt"))

    async def _find(self) -> tuple[str, int]:
        from zeroconf import ServiceStateChange
        from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

        azc = AsyncZeroconf()
        names: list[str] = []
        browser = AsyncServiceBrowser(azc.zeroconf, [OS2L_SERVICE], handlers=[
            lambda zeroconf, service_type, name, state_change: names.append(name)
            if state_change is ServiceStateChange.Added else None])
        try:
            for _ in range(30):
                await asyncio.sleep(0.1)
                if names:
                    break
            for name in names:
                info = AsyncServiceInfo(OS2L_SERVICE, name)
                if await info.async_request(azc.zeroconf, 1500) and info.parsed_addresses():
                    log.info("OS2L encontrado por DNS-SD: %s", name)
                    return info.parsed_addresses()[0], info.port
        finally:
            await browser.async_cancel()
            await azc.async_close()
        raise OSError("no se encontró ningún servicio _os2l._tcp")

    async def run(self) -> None:
        delay = 0.5
        while True:
            try:
                host, port = (await self._find()) if self.discover else (self.host, self.port)
                reader, writer = await asyncio.open_connection(host, port)
                sock = writer.get_extra_info("socket")
                if sock is not None:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.writer = writer
                self.connected.set()
                delay = 0.5
                print(C.c(C.GREEN, f"[os2l] conectado a {host}:{port}"), flush=True)
                if self.on_connect:
                    self.on_connect()
                await asyncio.gather(self._write_loop(writer), self._read_loop(reader))
            except (OSError, ConnectionError, asyncio.IncompleteReadError) as e:
                print(C.c(C.YELLOW, f"[os2l] sin conexión ({e}); reintento"), flush=True)
            finally:
                self.connected.clear()
                while not self.queue.empty():
                    self.queue.get_nowait()
                if self.writer:
                    self.writer.close()
                self.writer = None
            await asyncio.sleep(jittered(delay))
            delay = min(delay * 2, 5.0)

    async def _write_loop(self, writer: asyncio.StreamWriter) -> None:
        while True:
            msg = await self.queue.get()
            writer.write(dumps(msg).encode("utf-8"))
            await writer.drain()
            self.sent += 1
            if self.verbose:
                print(C.c(C.DIM, f"[os2l] → {dumps(msg)}"), flush=True)
            await asyncio.sleep(self.GAP_S)

    async def _read_loop(self, reader: asyncio.StreamReader) -> None:
        while True:
            data = await reader.read(4096)
            if not data:
                raise ConnectionError("el servidor OS2L cerró la conexión")
            if self.verbose:
                print(C.c(C.DIM, f"[os2l] ← {data.decode('utf-8', 'replace')}"), flush=True)


class Os2lBridge:
    def __init__(self, rx: Receiver, os2l: Os2lClient, extras: bool = True, page: str = "VertexAV"):
        self.rx = rx
        self.os2l = os2l
        self.extras = extras
        self.page = page
        self.section_btn: Optional[str] = None
        self.intensity_btn: Optional[str] = None
        self.mood_btn: Optional[str] = None
        self.strobe_ok: Optional[bool] = None
        self.energy_sent: Optional[int] = None
        self.last_beat: Optional[int] = None
        self.last_jumps = 0
        self.beats_sent = 0
        rx.on_event_fire = self.on_event
        rx.on_trigger_fire = self.on_trigger
        os2l.on_connect = self.resync

    # ----------------------------------------------------------------- OS2L output
    def btn(self, name: str, state: str) -> None:
        self.os2l.send({"evt": "btn", "name": name, "page": self.page, "state": state})

    def momentary(self, name: str) -> None:
        self.btn(name, "on")
        asyncio.get_running_loop().call_later(MOMENTARY_S, self.btn, name, "off")

    def _exclusive(self, attr: str, name: Optional[str]) -> None:
        old = getattr(self, attr)
        if old == name:
            return
        if old:
            self.btn(old, "off")
        if name:
            self.btn(name, "on")
        setattr(self, attr, name)

    def resync(self) -> None:
        """Bring the OS2L side up to date after a (re)connection on either side."""
        for attr in ("section_btn", "intensity_btn", "mood_btn"):
            setattr(self, attr, None)
        self.strobe_ok = None
        self.energy_sent = None
        sec = self.rx.current_section()
        if sec:
            self._exclusive("section_btn", f"vertexav.section.{sec['role']}")
        self.apply_intent()

    def apply_intent(self) -> None:
        if not self.extras:
            return
        it = self.rx.intent
        if it.get("intensity_hint"):
            self._exclusive("intensity_btn", f"vertexav.intensity.{it['intensity_hint']}")
        if it.get("mood"):
            self._exclusive("mood_btn", f"vertexav.mood.{it['mood']}")
        if isinstance(it.get("strobe_ok"), bool) and it["strobe_ok"] != self.strobe_ok:
            self.strobe_ok = it["strobe_ok"]
            self.btn("vertexav.strobe_ok", "on" if self.strobe_ok else "off")
        if isinstance(it.get("energy"), (int, float)):
            val = round(it["energy"] * 100)
            if val != self.energy_sent:
                self.energy_sent = val
                self.os2l.send({"evt": "cmd", "id": ENERGY_CMD_ID, "param": val})

    # ----------------------------------------------------------------- VertexAV input
    def on_trigger(self, msg: dict) -> None:
        name, state = msg.get("name"), msg.get("state")
        if not isinstance(name, str):
            return
        if state == "momentary":
            self.momentary(name)
        elif state in ("on", "off"):
            self.btn(name, state)

    def on_event(self, msg: dict) -> None:
        name = msg.get("name")
        data = msg.get("data") or {}
        on_air = self.rx.on_air_deck()
        if name == "section.change" and (on_air is None or msg.get("deck") == on_air):
            self._exclusive("section_btn", f"vertexav.section.{data.get('to')}")
        elif not self.extras:
            return
        elif name == "countdown":
            self.momentary(f"vertexav.countdown.{data.get('beats_remaining')}")
        elif name == "transition.start":
            self.btn("vertexav.transition", "on")
        elif name == "transition.end":
            self.btn("vertexav.transition", "off")
        elif name == "mix.cut":
            self.momentary("vertexav.mix.cut")
        elif name == "on_air.change":
            sec = self.rx.current_section(data.get("to_deck"))
            if sec:
                self._exclusive("section_btn", f"vertexav.section.{sec['role']}")

    async def intent_loop(self) -> None:
        while True:
            await asyncio.sleep(0.05)
            if self.os2l.connected.is_set() and self.rx.connected.is_set():
                self.apply_intent()

    async def beat_loop(self) -> None:
        """Send OS2L `beat` at each beat instant minus the configured latency."""
        rx = self.rx
        lat = rx.cfg.latency_us
        while True:
            now = rx.emitter_now()
            fb = rx.follower
            if now is None or fb.model is None or not fb.beats_enabled(now + lat) or not self.os2l.connected.is_set():
                self.last_beat = None
                await asyncio.sleep(0.02)
                continue
            beat_now = fb.beat_at(now + lat)
            idx = math.floor(beat_now + 1e-6) + 1
            # local wait until the emitter instant of beat idx (minus latency)
            target_em = fb.model.time_of_beat(idx + fb._corr_at(now))
            wait = (target_em - lat - now) / 1e6
            if wait > 0.03:
                await asyncio.sleep(min(wait - 0.02, 0.1))  # coarse sleep, re-evaluate tempo changes
                continue
            if wait > 0:
                await asyncio.sleep(wait)
            now2 = rx.emitter_now()
            if now2 is None or not fb.beats_enabled(now2 + lat):
                continue
            b2 = fb.beat_at(now2 + lat)
            if abs(b2 - idx) > 0.25:  # the timeline moved while we waited
                continue
            change = self.last_beat is None or idx != self.last_beat + 1 or fb.jumps != self.last_jumps
            self.last_jumps = fb.jumps
            self.last_beat = idx
            tp = fb.model
            downbeat = tp.beat_in_bar(idx) == 1
            energy = rx.intent.get("energy") if isinstance(rx.intent.get("energy"), (int, float)) else 0.8
            strength = round((1.0 if downbeat else 0.5) * (0.5 + 0.5 * energy), 3)
            self.os2l.send({"evt": "beat", "change": change, "pos": idx, "bpm": round(tp.bpm, 3),
                            "strength": strength}, urgent=True)
            self.beats_sent += 1
            await asyncio.sleep(0.05)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="python -m vertexav.os2l", description="Pasarela VertexAV Link → OS2L")
    p.add_argument("--host", default="127.0.0.1", help="emisor VertexAV")
    p.add_argument("--port", type=int, default=9470)
    p.add_argument("--discover", action="store_true", help="buscar el emisor VertexAV por DNS-SD")
    p.add_argument("--psk", help="código de emparejamiento (modo auth)")
    p.add_argument("--os2l-host", default="127.0.0.1", help="servidor OS2L (QLC+)")
    p.add_argument("--os2l-port", type=int, default=9996, help="puerto OS2L (QLC+ usa 9996 por defecto)")
    p.add_argument("--os2l-discover", action="store_true", help="buscar el servidor OS2L por DNS-SD (_os2l._tcp)")
    p.add_argument("--latency-ms", type=float, default=0.0,
                   help="retraso propio hasta la luz (interfaz DMX...); los beats salen antes")
    p.add_argument("--no-extras", action="store_true", help="solo beat, disparos y secciones (§16.1)")
    p.add_argument("--page", default="VertexAV", help="campo page de los botones OS2L")
    p.add_argument("--duration", type=float)
    p.add_argument("-v", "--verbose", action="store_true")
    return p


async def amain(args: argparse.Namespace) -> None:
    if args.discover:
        from .inspector import discover

        found = await discover(2.5)
        if not found:
            sys.exit("No se encontró ningún emisor VertexAV")
        args.host, args.port = found[0]["host"], found[0]["port"]
    from .util import load_or_create_id

    rx = Receiver(ReceiverConfig(host=args.host, port=args.port, name="VertexAV → OS2L",
                                 client_id=load_or_create_id("os2l-bridge", "os2l-"),
                                 profiles=[0, 1, 2], latency_us=int(args.latency_ms * 1000),
                                 psk=security.code_to_psk(args.psk) if args.psk else None))
    rx.on_status = lambda s: print(C.c(C.BOLD, f"[vertexav] {s}"), flush=True)
    os2l = Os2lClient(args.os2l_host, args.os2l_port, args.os2l_discover, args.verbose)
    bridge = Os2lBridge(rx, os2l, extras=not args.no_extras, page=args.page)

    async def on_rx_connect():
        while True:
            await rx.connected.wait()
            await asyncio.sleep(0.3)  # let the snapshot arrive
            bridge.resync()
            while rx.connected.is_set():
                await asyncio.sleep(0.2)

    async def status():
        while True:
            await asyncio.sleep(5)
            fb = rx.follower
            print(C.c(C.CYAN, f"[estado] beats enviados {bridge.beats_sent}, mensajes OS2L {os2l.sent}, "
                              f"tempo {fb.model.bpm if fb.model else '-'} BPM, sección {bridge.section_btn}"), flush=True)

    tasks = [asyncio.create_task(c) for c in (rx.run(), os2l.run(), bridge.beat_loop(), bridge.intent_loop(),
                                               on_rx_connect(), status())]
    try:
        if args.duration:
            await asyncio.sleep(args.duration)
        else:
            await asyncio.gather(*tasks)
    finally:
        for t in tasks:
            t.cancel()
        await rx.stop()


def main() -> None:
    args = build_parser().parse_args()
    enable_ansi()
    enable_fine_timers()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.WARNING,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
