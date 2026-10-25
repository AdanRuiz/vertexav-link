"""Fake OS2L server (stands in for QLC+) that prints what arrives and measures beat timing.

With --check it also connects to the VertexAV emitter as a receiver and compares the arrival
time of each OS2L `beat` with the beat instant the emitter published.

Run: python -m vertexav.os2l_mock --help
"""
from __future__ import annotations

import argparse
import asyncio
import json
import statistics
from collections import Counter
from typing import Optional

from .clock import enable_fine_timers
from .receiver import Receiver, ReceiverConfig
from .util import C, enable_ansi


class MockServer:
    def __init__(self, args: argparse.Namespace, rx: Optional[Receiver]):
        self.args = args
        self.rx = rx
        self.counts: Counter = Counter()
        self.beat_errors: list[float] = []
        self.coalesced = 0
        self.buttons: dict[str, str] = {}
        self.last_pos: Optional[int] = None
        self.pos_skips = 0

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        print(C.c(C.GREEN, f"[mock] cliente OS2L conectado {peer}"), flush=True)
        dec = json.JSONDecoder()
        buf = ""
        while True:
            data = await reader.read(4096)
            if not data:
                break
            arrival = self.rx.local_now() if self.rx else 0
            buf += data.decode("utf-8", "replace")
            objs = []
            while buf.strip():
                buf = buf.lstrip()
                try:
                    obj, end = dec.raw_decode(buf)
                except json.JSONDecodeError:
                    break
                objs.append(obj)
                buf = buf[end:]
            if len(objs) > 1:
                self.coalesced += 1
            for obj in objs:
                self.on_msg(obj, arrival)
        print(C.c(C.YELLOW, "[mock] cliente desconectado"), flush=True)

    def on_msg(self, msg: dict, arrival: int) -> None:
        evt = msg.get("evt")
        self.counts[evt] += 1
        extra = ""
        if evt == "beat":
            pos = msg.get("pos")
            if self.last_pos is not None and isinstance(pos, int) and pos != self.last_pos + 1 and not msg.get("change"):
                self.pos_skips += 1
            self.last_pos = pos
            rx = self.rx
            if rx and rx.follower.model is not None and rx.sync.offset is not None and isinstance(pos, int):
                fb = rx.follower
                t_beat_em = fb.model.time_of_beat(pos + fb._corr_at(rx.emitter_now()))
                err = arrival - rx.sync.to_local(t_beat_em) + self.args.bridge_latency_ms * 1000
                self.beat_errors.append(err)
                extra = f" (error {err/1000:+.2f} ms)"
        elif evt == "btn":
            self.buttons[msg.get("name")] = msg.get("state")
        if not self.args.quiet and (evt != "beat" or self.args.beats):
            print(f"[mock] {json.dumps(msg, ensure_ascii=False)}{extra}", flush=True)

    def report(self) -> None:
        print(C.c(C.BOLD, "\n=== Informe del servidor OS2L de pruebas ==="))
        print("Mensajes: " + ", ".join(f"{k}={v}" for k, v in self.counts.items()))
        print(f"Lecturas TCP con varios mensajes juntos: {self.coalesced}")
        print(f"Saltos de pos sin change=true: {self.pos_skips}")
        if len(self.beat_errors) > 2:
            errs = sorted(abs(e) for e in self.beat_errors)
            print(f"Error de llegada del beat: media {statistics.mean(self.beat_errors)/1000:+.2f} ms, "
                  f"mediana |e| {errs[len(errs)//2]/1000:.2f} ms, p95 {errs[int(len(errs)*0.95)]/1000:.2f} ms, "
                  f"máx {errs[-1]/1000:.2f} ms ({len(errs)} beats)")
        on = sorted(k for k, v in self.buttons.items() if v == "on")
        print(f"Botones encendidos al final: {on}")


async def amain(args: argparse.Namespace) -> None:
    rx = None
    tasks = []
    if args.check:
        rx = Receiver(ReceiverConfig(host=args.vx_host, port=args.vx_port, name="OS2L mock checker",
                                     profiles=[0]))
        tasks.append(asyncio.create_task(rx.run()))
    mock = MockServer(args, rx)
    server = await asyncio.start_server(mock.handle, args.host, args.port)
    print(C.c(C.BOLD, f"[mock] servidor OS2L en {args.host}:{args.port}"), flush=True)
    try:
        if args.duration:
            await asyncio.sleep(args.duration)
        else:
            await server.serve_forever()
    finally:
        server.close()
        for t in tasks:
            t.cancel()
        mock.report()


def main() -> None:
    p = argparse.ArgumentParser(prog="python -m vertexav.os2l_mock", description="Servidor OS2L de pruebas")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9996)
    p.add_argument("--check", action="store_true", help="medir la precisión de los beats contra el emisor VertexAV")
    p.add_argument("--vx-host", default="127.0.0.1")
    p.add_argument("--vx-port", type=int, default=9470)
    p.add_argument("--bridge-latency-ms", type=float, default=0.0, help="--latency-ms usado en la pasarela")
    p.add_argument("--beats", action="store_true", help="mostrar cada beat")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--duration", type=float)
    args = p.parse_args()
    enable_ansi()
    enable_fine_timers()
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
