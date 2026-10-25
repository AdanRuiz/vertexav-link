"""Simulated DJ session for testing receivers (profiles 0-2).

Story, repeated forever with the decks swapping roles:
  * the deck on air plays a track with intro / build / drop / breakdown / build / drop / outro;
  * the next track is loaded on the other deck and mixed in at the outro, alternating a slow
    blend with channel faders and a crossfader cut with a chop;
  * countdowns (16, 8, 4, 2, 1 beats) and section.change are announced lookahead_us early;
  * in the first breakdown the DJ scratches with Slip Mode (stability "freewheel"), in the
    second one without slip (stability "unstable", then phase_reset);
  * the second build is looped twice right before the drop, which cancels announced events;
  * after each mix the tempo moves gradually to the native BPM of the new track.

Run: python -m vertexav.sim --help
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import math
import sys
from dataclasses import dataclass, field, replace
from typing import Callable, Optional

from . import security
from .clock import enable_fine_timers
from .emitter import Emitter, EmitterConfig
from .mixlogic import MixAnalyzer
from .tempo import Tempo
from .util import C, enable_ansi

log = logging.getLogger("vertexav.sim")

FULL = [("intro", 32, 0.30), ("build", 32, 0.55), ("drop", 64, 0.95), ("breakdown", 32, 0.35),
        ("build", 32, 0.60), ("drop", 64, 1.00), ("outro", 48, 0.40)]
FAST = [("intro", 16, 0.30), ("build", 16, 0.55), ("drop", 32, 0.95), ("breakdown", 16, 0.35),
        ("build", 16, 0.60), ("drop", 32, 1.00), ("outro", 48, 0.40)]


@dataclass
class TrackDef:
    track_id: str
    title: str
    artist: str
    genre: str
    bpm: float
    key: str
    color: str
    mood: str
    valence: float
    arousal: float
    palette: list
    template: list = field(default_factory=lambda: FULL)
    first_beat_s: float = 0.112

    def __post_init__(self) -> None:
        self.sections = []
        start = 0
        for role, length, energy in self.template:
            self.sections.append({"start_beat": start, "end_beat": start + length, "role": role,
                                  "energy": energy, "confidence": 0.9})
            start += length
        self.total_beats = start

    def beat_to_s(self, beat: float) -> float:
        return self.first_beat_s + beat * 60.0 / self.bpm

    @property
    def duration_s(self) -> float:
        return round(self.beat_to_s(self.total_beats) + 4.0, 3)

    def section_index(self, beat: float) -> Optional[int]:
        for i, s in enumerate(self.sections):
            if s["start_beat"] <= beat < s["end_beat"]:
                return i
        return None

    def role_start(self, role: str, nth: int = 0) -> Optional[int]:
        found = [s["start_beat"] for s in self.sections if s["role"] == role]
        return found[nth] if nth < len(found) else None

    def meta(self) -> dict:
        return {"title": self.title, "artist": self.artist, "album": "VertexAV Test Sessions",
                "genre": self.genre, "year": 2026, "duration_s": self.duration_s, "bpm": self.bpm,
                "key": self.key, "key_notation": "camelot", "color": self.color, "rating": 4,
                "tags": ["test"], "comment": ""}

    def structure(self) -> dict:
        values = []
        for s in self.sections:
            n = s["end_beat"] - s["start_beat"]
            for i in range(n):
                e = s["energy"]
                if s["role"] == "build":
                    e = s["energy"] + 0.3 * i / n  # tension rises during the build
                values.append(round(min(1.0, e), 3))
        drop = self.role_start("drop")
        outro = self.role_start("outro")
        return {
            "track_id": self.track_id,
            "analysis": {"source": "vertexav-sim", "version": "1.0", "confidence": 0.9},
            "beatgrid": {"type": "constant", "bpm": self.bpm, "first_beat_s": self.first_beat_s,
                         "beats_per_bar": 4, "first_downbeat_beat": 0},
            "sections": self.sections,
            "energy_curve": {"resolution": "beat", "values": values},
            "markers": [
                {"kind": "hotcue", "index": 1, "position_s": self.first_beat_s, "label": "Inicio", "color": "#FF0000"},
                {"kind": "hotcue", "index": 8, "position_s": round(self.beat_to_s(drop), 3), "label": "Drop",
                 "color": "#FFAA00", "role": "drop"},
                {"kind": "intro_end", "position_s": round(self.beat_to_s(self.sections[0]["end_beat"]), 3)},
                {"kind": "outro_start", "position_s": round(self.beat_to_s(outro), 3)},
            ],
            "mood": {"mood": self.mood, "valence": self.valence, "arousal": self.arousal},
        }


def library(template: list) -> list[TrackDef]:
    return [
        TrackDef("sim:0001", "Neon Cathedral", "Test Artist A", "Techno", 126.0, "8A", "#3A7BFF",
                 "dark", 0.25, 0.8, ["#1B0A3C", "#6A00FF", "#FF2E88"], template),
        TrackDef("sim:0002", "Solar Wind", "Test Artist B", "Progressive House", 128.0, "9A", "#FFAA00",
                 "euphoric", 0.75, 0.85, ["#FFB000", "#FF5E00", "#FFFFFF"], template),
        TrackDef("sim:0003", "Deep Current", "Test Artist C", "Deep House", 124.0, "5A", "#00C2A8",
                 "groovy", 0.55, 0.6, ["#003B46", "#07575B", "#66A5AD"], template),
        TrackDef("sim:0004", "Iron Pulse", "Test Artist D", "Techno", 130.0, "11A", "#FF2020",
                 "aggressive", 0.3, 0.95, ["#FF0000", "#200000", "#FFFFFF"], template),
    ]


@dataclass
class SimDeck:
    n: int
    track: Optional[TrackDef] = None
    playing: bool = False
    offset: float = 0.0  # deck_beat = master_beat - offset
    fader: float = 0.0
    loop: Optional[list] = None  # [in_beat, out_beat, wraps_left]
    played: bool = False  # the loaded track was already mixed out
    load_id: int = 0  # changes on every load, identifies one play of a track

    def beat(self, master_beat: float) -> float:
        return master_beat - self.offset


def intensity(energy: float) -> str:
    if energy < 0.4:
        return "low"
    if energy < 0.65:
        return "medium"
    if energy < 0.9:
        return "high"
    return "peak"


class Simulation:
    TICK = 0.005

    def __init__(self, emitter: Emitter, template: list, start_beat: float = 0.0,
                 printer: Callable[[str], None] = print):
        self.em = emitter
        self.tracks = library(template)
        self.next_track = 0
        self.decks = {1: SimDeck(1), 2: SimDeck(2)}
        self.mix = MixAnalyzer()
        self.crossfader = 0.0
        self.master = Tempo(bpm=self.tracks[0].bpm, playing=False)
        self.on_air = 1  # deck the script considers "A"
        self.phase = "solo"
        self.style = "blend"
        self.cycle = 0
        self.mix_start = 0.0
        self.tempo_target: Optional[float] = None
        self.last_ramp_beat: Optional[int] = None
        self.announced: dict[tuple, tuple[str, int]] = {}
        self.done_keys: set = set()
        self.current_section: dict[int, Optional[int]] = {1: None, 2: None}
        self.scratch: Optional[dict] = None
        self.scratch_done: set = set()
        self.loop_done: set = set()
        self.start_beat = start_beat
        self.say = printer

    # -------------------------------------------------------------------- helpers
    def _load(self, deck: SimDeck) -> None:
        tr = self.tracks[self.next_track % len(self.tracks)]
        self.next_track += 1
        deck.track = tr
        deck.playing = False
        deck.loop = None
        deck.played = False
        deck.load_id = self.next_track
        self.em.track_loaded(deck.n, tr.track_id, tr.meta(), structure_available=True)
        self.em.track_structure(tr.structure())
        self.em.update_deck(deck.n, position_s=tr.first_beat_s, position_beat=0.0, rate=1.0)
        self.say(C.c(C.CYAN, f"[sim] deck {deck.n} carga '{tr.title}' ({tr.bpm} BPM)"))

    def _pos(self, deck: SimDeck, mb: float) -> dict:
        b = deck.beat(mb)
        return {"position_s": round(deck.track.beat_to_s(b), 4), "position_beat": round(b, 4),
                "rate": round(self.master.bpm / deck.track.bpm, 5), "bpm": round(self.master.bpm, 3)}

    def _start(self, deck: SimDeck, mb: float) -> None:
        deck.playing = True
        self.em.update_deck(deck.n, playing=True, **self._pos(deck, mb))
        self.em.event("track.start", deck=deck.n, data={})

    def _stop(self, deck: SimDeck, mb: float, kind: str = "pause") -> None:
        deck.playing = False
        self.em.update_deck(deck.n, playing=False, **self._pos(deck, mb))
        self.em.event("track.stop", deck=deck.n, data={"kind": kind})

    def _publish_tempo(self, **changes) -> None:
        self.master = replace(self.master, **changes)
        self.em.set_tempo(self.master)
        self.master.phase_reset = False

    def _apply_intent(self, deck: SimDeck, idx: int) -> None:
        tr = deck.track
        sec = tr.sections[idx]
        e = sec["energy"]
        self.em.update_intent(energy=e, mood=tr.mood, valence=tr.valence,
                              arousal=round(min(1.0, tr.arousal * (0.6 + 0.4 * e)), 3),
                              palette=tr.palette, intensity_hint=intensity(e),
                              strobe_ok=sec["role"] in ("drop", "build") and e >= 0.5)

    # -------------------------------------------------------------------- main loop
    async def run(self) -> None:
        now = self.em.now()
        d1 = self.decks[1]
        self._load(d1)
        d1.offset = -self.start_beat
        d1.fader = 1.0
        self.master = Tempo(bpm=d1.track.bpm, anchor_t=now, anchor_beat=0.0, beats_per_bar=4,
                            bar_offset=float(d1.offset % 4), source_deck=1, playing=True,
                            confidence=1.0, stability="stable", phase_reset=True)
        self._publish_tempo()
        self._start(d1, 0.0)
        self.em.update_deck(1, fader=1.0)
        self.em.update_mix(crossfader=0.0)
        while True:
            self.tick()
            await asyncio.sleep(self.TICK)

    def tick(self) -> None:
        now = self.em.now()
        mb = self.master.beat_at(now)
        self._script(now, mb)
        self._decks(now, mb)
        self._mix(now, mb)
        self._schedule_events(now)
        self._sections(now)

    # -------------------------------------------------------------------- DJ script
    def _script(self, now: int, mb: float) -> None:
        a = self.decks[self.on_air]
        b = self.decks[3 - self.on_air]
        ab = a.beat(mb)
        if self.scratch:
            self._scratch_step(now, mb)
        if self.phase == "solo":
            if (b.track is None or b.played) and ab >= 16:
                self._load(b)
            if ab >= a.track.role_start("outro") and b.track is not None and not b.played:
                start = math.ceil((mb - self.master.bar_offset) / 4 - 1e-9) * 4 + self.master.bar_offset
                self.mix_start = start
                b.offset = start  # deck B beat 0 on that downbeat
                self.phase = "mixing"
                self.style = "blend" if self.cycle % 2 == 0 else "cut"
                if self.style == "cut":
                    self.crossfader = -1.0 if a.n == 1 else 1.0
                    b.fader = 1.0
                    self.em.update_deck(b.n, pfl=True)
                self.say(C.c(C.CYAN, f"[sim] mezcla {self.style}: deck {a.n} → deck {b.n}"))
            self._loop_build(now, mb, a)
            self._maybe_scratch(now, mb, a)
        elif self.phase == "mixing":
            rel = mb - self.mix_start
            if rel >= 0 and not b.playing:
                self._start(b, mb)
            if self.style == "blend":
                b.fader = max(0.0, min(1.0, rel / 16))
                a.fader = max(0.0, min(1.0, 1 - (rel - 16) / 16))
            else:
                if 16 <= rel < 24 or rel >= 24.5:
                    self.crossfader = 1.0 if b.n == 2 else -1.0  # cut to B
                elif 24 <= rel < 24.5:
                    self.crossfader = -1.0 if a.n == 1 else 1.0  # chop back to A for half a beat
                if rel >= 17:
                    self.em.update_deck(b.n, pfl=False)
            if rel >= 36 and a.playing:
                self._stop(a, mb)
                a.fader = 0.0
                a.played = True
                if self.style == "cut":
                    self.crossfader = 0.0
                self.on_air = b.n
                self.tempo_target = b.track.bpm
                self.phase = "settle"
                self.cycle += 1
        elif self.phase == "settle":
            beat_idx = math.floor(mb)
            if self.tempo_target is not None and beat_idx != self.last_ramp_beat:
                self.last_ramp_beat = beat_idx
                diff = self.tempo_target - self.master.bpm
                if abs(diff) < 1e-6:
                    self.tempo_target = None
                    self.phase = "solo"
                else:
                    step = max(-0.5, min(0.5, diff))
                    t_beat = self.master.time_of_beat(beat_idx)
                    base = self.master.rebase(max(t_beat, now - 50_000))
                    self._publish_tempo(bpm=round(self.master.bpm + step, 3), anchor_t=base.anchor_t,
                                        anchor_beat=base.anchor_beat)
                    for d in self.decks.values():
                        if d.playing:
                            self.em.update_deck(d.n, **self._pos(d, self.master.beat_at(now)))
            self._loop_build(now, mb, b)
            self._maybe_scratch(now, mb, b)

    def _loop_build(self, now: int, mb: float, deck: SimDeck) -> None:
        """Loop the last 4 beats of the 2nd build twice, activated 100 ms before the drop."""
        tr = deck.track
        if not deck.playing or tr is None:
            return
        drop = tr.role_start("drop", 1)
        key = (deck.n, deck.load_id)
        if drop is None or key in self.loop_done:
            return
        lead = 0.1 * self.master.bpm / 60  # 100 ms in beats
        db = deck.beat(mb)
        if drop - lead <= db < drop:
            self.loop_done.add(key)
            deck.loop = [drop - 4, drop, 2]
            self.em.update_deck(deck.n, loop={"active": True, "in_s": round(tr.beat_to_s(drop - 4), 4),
                                              "out_s": round(tr.beat_to_s(drop), 4)})
            self.em.event("loop.on", deck=deck.n, data={"in_s": round(tr.beat_to_s(drop - 4), 4),
                                                         "out_s": round(tr.beat_to_s(drop), 4), "beats": 4})
            self.say(C.c(C.CYAN, f"[sim] deck {deck.n}: loop de 4 beats antes del drop (cancela eventos)"))

    def _maybe_scratch(self, now: int, mb: float, deck: SimDeck) -> None:
        tr = deck.track
        if self.scratch or not deck.playing or tr is None:
            return
        # Alternate per load: Slip Mode scratch (freewheel) or plain scratch (unstable + phase_reset)
        for nth, slip in ((0, deck.load_id % 2 == 0), (1, deck.load_id % 2 == 1)):
            start = tr.role_start("breakdown", nth)
            if start is None:
                continue
            key = (deck.n, deck.load_id, nth)
            if key in self.scratch_done:
                continue
            if start + 8 <= deck.beat(mb) < start + 9:
                self.scratch_done.add(key)
                length = 1.0 if slip else 2.0
                self.scratch = {"deck": deck, "slip": slip, "end": mb + length}
                self.em.event("scratch.start", deck=deck.n, data={})
                if slip:
                    self.em.update_deck(deck.n, scratching=True,
                                        slip={"active": True, "position_s": round(tr.beat_to_s(deck.beat(mb)), 4)})
                    self._publish_tempo(stability="freewheel")
                else:
                    self.em.update_deck(deck.n, scratching=True)
                    self._publish_tempo(stability="unstable")
                self.say(C.c(C.CYAN, f"[sim] deck {deck.n}: scratch {'con' if slip else 'sin'} slip"))

    def _scratch_step(self, now: int, mb: float) -> None:
        sc = self.scratch
        if mb < sc["end"]:
            return
        deck = sc["deck"]
        self.scratch = None
        self.em.event("scratch.end", deck=deck.n, data={})
        if sc["slip"]:
            self.em.update_deck(deck.n, scratching=False, slip={"active": False, "position_s": None})
            self._publish_tempo(stability="stable")
        else:
            # Without slip the track moved: new anchor half a beat later, phase_reset.
            self._publish_tempo(anchor_t=now, anchor_beat=mb + 0.5, stability="stable", phase_reset=True)
            self.em.event("seek", deck=deck.n, data={"from_s": round(deck.track.beat_to_s(deck.beat(mb)), 4),
                                                      "to_s": round(deck.track.beat_to_s(deck.beat(mb + 0.5)), 4)})
            self.em.update_deck(deck.n, scratching=False, **self._pos(deck, mb + 0.5))

    # -------------------------------------------------------------------- decks and mix
    def _decks(self, now: int, mb: float) -> None:
        for d in self.decks.values():
            if not d.playing or d.track is None:
                continue
            if d.loop:
                start, end, left = d.loop
                if d.beat(mb) >= end:
                    d.offset += end - start
                    d.loop[2] -= 1
                    upd = self._pos(d, mb)
                    if d.loop[2] <= 0:
                        d.loop = None
                        upd["loop"] = {"active": False, "in_s": None, "out_s": None}
                        self.em.event("loop.off", deck=d.n, data={"in_s": round(d.track.beat_to_s(start), 4),
                                                                   "out_s": round(d.track.beat_to_s(end), 4), "beats": 4})
                    self.em.update_deck(d.n, **upd)
            if d.beat(mb) >= d.track.total_beats:
                self._stop(d, mb, "end")

    def _audible(self, d: SimDeck) -> float:
        if not d.playing:
            return 0.0
        gain = max(0.0, min(1.0, 1 - self.crossfader)) if d.n == 1 else max(0.0, min(1.0, 1 + self.crossfader))
        return round(d.fader * gain, 4)

    def _mix(self, now: int, mb: float) -> None:
        aud = {n: self._audible(d) for n, d in self.decks.items()}
        for n, d in self.decks.items():
            self.em.update_deck(n, fader=round(d.fader, 4), audible=aud[n])
        self.em.update_mix(crossfader=self.crossfader)
        res = self.mix.update(aud, now, self.master.beat_us)
        if res.mix:
            self.em.update_mix(**res.mix)
        for name, deck, data in res.events:
            self.em.event(name, deck=deck, data=data)
            if name in ("mix.cut", "transition.start", "transition.end", "on_air.change"):
                self.say(C.c(C.DIM, f"[sim] {name} {data}"))
        if res.on_air_change:
            old, new = res.on_air_change
            self.em.update_deck(old, on_air=False)
            self.em.update_deck(new, on_air=True)
            self._publish_tempo(source_deck=new)
            d = self.decks[new]
            if d.track:
                idx = d.track.section_index(d.beat(mb))
                self.current_section[new] = idx
                if idx is not None:
                    self._apply_intent(d, idx)
        elif self.mix.on_air is not None and not self.em.decks[self.mix.on_air]["on_air"]:
            self.em.update_deck(self.mix.on_air, on_air=True)

    # -------------------------------------------------------------------- structure events
    def _schedule_events(self, now: int) -> None:
        look = self.em.cfg.lookahead_us
        deck_n = self.mix.on_air
        deck = self.decks.get(deck_n) if deck_n else None
        live_keys = set()
        if deck and deck.playing and deck.track and self.master.stability != "unstable":
            tr = deck.track
            db = deck.beat(self.master.beat_at(now))
            for idx, sec in enumerate(tr.sections):
                if idx == 0:
                    continue
                prev = tr.sections[idx - 1]
                targets = [("sc", sec["start_beat"], None)]
                if sec["role"] == "drop" or sec["energy"] - prev["energy"] > 0.3:
                    targets += [("cd", sec["start_beat"] - n, n) for n in (16, 8, 4, 2, 1)]
                for kind, beat, n in targets:
                    if beat <= db:
                        continue
                    if deck.loop and beat >= deck.loop[1]:
                        continue  # an active loop makes it unreachable: cancel what was announced
                    at = self.master.time_of_beat(beat + deck.offset)
                    if at - now > look:
                        continue
                    key = (kind, tr.track_id, deck.n, idx, n, deck.offset)
                    live_keys.add(key)
                    if key in self.announced or key in self.done_keys:
                        continue
                    if kind == "sc":
                        eid = self.em.event("section.change", at=at, deck=deck.n, data={
                            "from": prev["role"], "to": sec["role"], "section_index": idx, "energy": sec["energy"]})
                    else:
                        target_at = self.master.time_of_beat(sec["start_beat"] + deck.offset)
                        eid = self.em.event("countdown", at=at, deck=deck.n, data={
                            "target": sec["role"], "beats_remaining": n, "target_at": target_at})
                    self.announced[key] = (eid, at)
        for key, (eid, at) in list(self.announced.items()):
            if at <= now:
                self.done_keys.add(key)
                del self.announced[key]
            elif key not in live_keys:
                reason = "loop" if self.decks[key[2]].loop else ("seek" if self.master.stability != "unstable" else "scratch")
                self.em.cancel_event(eid, reason)
                del self.announced[key]
                self.say(C.c(C.YELLOW, f"[sim] event.cancel {eid} ({reason})"))

    def _sections(self, now: int) -> None:
        mb = self.master.beat_at(now)
        for n, d in self.decks.items():
            if not d.playing or not d.track:
                continue
            idx = d.track.section_index(d.beat(mb))
            if idx == self.current_section.get(n):
                continue
            self.current_section[n] = idx
            if n != self.mix.on_air or idx is None:
                continue
            sec = d.track.sections[idx]
            self._apply_intent(d, idx)
            if sec["role"] == "drop":
                self.em.trigger("strobe", "momentary", value=1.0, deck=n)
                self.em.trigger("fog", "off", deck=n)
            elif sec["role"] == "build":
                self.em.trigger("fog", "on", value=0.6, deck=n)
            self.say(C.c(C.GREEN, f"[sim] deck {n}: {sec['role']} (energía {sec['energy']})"))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="python -m vertexav.sim", description="Emisor VertexAV Link simulado")
    p.add_argument("--name", default="VertexAV Simulator")
    p.add_argument("--host", default="0.0.0.0", help="dirección de escucha (127.0.0.1 = solo este equipo)")
    p.add_argument("--port", type=int, default=9470)
    p.add_argument("--udp-port", type=int, default=9471)
    p.add_argument("--security", choices=["open", "auth"], default="open")
    p.add_argument("--psk", help="código de emparejamiento (base32); con --security auth")
    p.add_argument("--gen-psk", action="store_true", help="genera un código de emparejamiento nuevo y sale")
    p.add_argument("--profiles", default="0,1,2")
    p.add_argument("--fast", action="store_true", help="secciones más cortas (ciclo de ~1,5 min)")
    p.add_argument("--start-beat", type=float, default=0.0, help="beat inicial del primer tema")
    p.add_argument("--no-announce", action="store_true", help="no anunciarse por DNS-SD")
    p.add_argument("--lookahead-ms", type=int, default=150)
    p.add_argument("--id", help="identificador del emisor (por defecto uno estable por instalación)")
    p.add_argument("--duration", type=float, help="segundos de ejecución (por defecto, sin fin)")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


async def amain(args: argparse.Namespace) -> None:
    psk = security.code_to_psk(args.psk) if args.psk else None
    if args.security == "auth" and psk is None:
        sys.exit("--security auth necesita --psk (genera uno con --gen-psk)")
    local = args.host.startswith("127.")
    cfg = EmitterConfig(name=args.name, id=args.id, profiles=[int(x) for x in args.profiles.split(",")],
                        security=args.security, psk=psk, host=args.host, port=args.port,
                        udp_port=args.udp_port, scope="local" if local else "lan",
                        announce=not args.no_announce, lookahead_us=args.lookahead_ms * 1000)
    em = Emitter(cfg)
    await em.start()
    if args.security == "open" and not local:
        print(C.c(C.YELLOW + C.BOLD, "AVISO: modo 'open' fuera de 127.0.0.1: cualquiera en la red puede "
                                     "conectarse o inyectar tempo falso (§15.2)."))
    em.on_receiver_message = lambda s, m: (
        print(C.c(C.MAGENTA, f"[{s.name}] {m.get('type')} {m.get('summary', '')}"))
        if m.get("type") in ("receiver.state", "receiver.capabilities") and not args.quiet else None)
    printer = (lambda s: None) if args.quiet else print
    sim = Simulation(em, FAST if args.fast else FULL, args.start_beat, printer)
    task = asyncio.create_task(sim.run())
    try:
        if args.duration:
            await asyncio.sleep(args.duration)
        else:
            await asyncio.Event().wait()
    finally:
        task.cancel()
        await em.stop()


def main() -> None:
    args = build_parser().parse_args()
    enable_ansi()
    if args.gen_psk:
        print(security.psk_to_code(security.generate_psk()))
        return
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    enable_fine_timers()
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
