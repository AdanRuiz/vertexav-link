"""Emitter-side mix analysis (§10.2): deck on air, transitions, cuts and gates.

Feed it the `audible` value of every deck on each tick; it returns the events and the
mix.state changes to publish. The same logic is ported to libvertexav for Mixxx.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

TR_START = 0.15  # second deck audible enough to start a transition
SILENT = 0.05  # below this a deck is considered not audible
ON_AIR_MARGIN = 0.02  # hysteresis to switch the deck on air
GATE_FAST_US = 50_000  # a drop below SILENT within 50 ms is a gate/chop


@dataclass
class _Gate:
    open: bool
    last_high: Optional[int]
    gated: bool = False


@dataclass
class MixResult:
    events: list = field(default_factory=list)  # (name, deck, data)
    mix: dict = field(default_factory=dict)  # mix.state fields that changed
    on_air_change: Optional[tuple] = None  # (old, new)


class MixAnalyzer:
    def __init__(self) -> None:
        self.on_air: Optional[int] = None
        self.transition: dict = {"active": False, "kind": None, "from_deck": None, "to_deck": None,
                                 "progress": None}
        self._tr_start: Optional[int] = None
        self._from_low_since: Optional[int] = None
        self._gates: dict[int, _Gate] = {}

    def state(self) -> dict:
        return {"on_air_deck": self.on_air, "transition": dict(self.transition)}

    def _end_transition(self, res: MixResult, final_deck: Optional[int]) -> None:
        res.events.append(("transition.end", final_deck, {"deck": final_deck}))
        self.transition = {"active": False, "kind": None, "from_deck": None, "to_deck": None,
                           "progress": None}
        self._tr_start = None
        self._from_low_since = None
        res.mix["transition"] = dict(self.transition)

    def update(self, audible: dict[int, float], now: int, beat_us: float) -> MixResult:
        res = MixResult()
        # Gates (chops, crossfader cuts)
        for deck, a in audible.items():
            g = self._gates.get(deck)
            if g is None:
                g = self._gates[deck] = _Gate(a > TR_START, now if a > TR_START else None)
                continue
            if a > TR_START:
                if not g.open and g.gated:
                    res.events.append(("deck.gate", deck, {"state": "open"}))
                    g.gated = False
                g.open = True
                g.last_high = now
            elif a < SILENT and g.open:
                if g.last_high is not None and now - g.last_high <= GATE_FAST_US:
                    res.events.append(("deck.gate", deck, {"state": "closed"}))
                    g.gated = True
                g.open = False

        # Transition start (blend) relative to the current deck on air
        tr = self.transition
        if not tr["active"] and self.on_air is not None and audible.get(self.on_air, 0) > SILENT:
            others = [d for d, a in audible.items() if d != self.on_air and a > TR_START]
            if others:
                to = max(others, key=lambda d: audible[d])
                self.transition = tr = {"active": True, "kind": "blend", "from_deck": self.on_air,
                                        "to_deck": to, "progress": 0.0}
                self._tr_start = now
                res.events.append(("transition.start", to, {"from_deck": self.on_air, "to_deck": to}))

        # Deck on air with hysteresis
        if audible:
            cand = max(audible, key=lambda d: audible[d])
            cur = audible.get(self.on_air, 0.0) if self.on_air is not None else -1.0
            if cand != self.on_air and audible[cand] >= SILENT and audible[cand] > cur + ON_AIR_MARGIN:
                old = self.on_air
                self.on_air = cand
                res.mix["on_air_deck"] = cand
                if old is not None:
                    res.on_air_change = (old, cand)
                    res.events.append(("on_air.change", cand, {"from_deck": old, "to_deck": cand}))
                    blend_long = (tr["active"] and tr["kind"] == "blend" and self._tr_start is not None
                                  and now - self._tr_start >= beat_us / 2)
                    if not blend_long:
                        self.transition = tr = {"active": True, "kind": "cut", "from_deck": old,
                                                "to_deck": cand, "progress": 1.0}
                        self._tr_start = now
                        self._from_low_since = None
                        res.events.append(("mix.cut", cand, {"from_deck": old, "to_deck": cand}))

        # Transition progress and end
        if tr["active"]:
            af = audible.get(tr["from_deck"], 0.0)
            at = audible.get(tr["to_deck"], 0.0)
            if tr["kind"] == "blend":
                tr["progress"] = round(at / (af + at), 4) if af + at > 0 else 1.0
            if at < SILENT and tr["kind"] == "blend":
                self._end_transition(res, tr["from_deck"])  # aborted mix
            elif af < SILENT:
                if self._from_low_since is None:
                    self._from_low_since = now
                elif now - self._from_low_since >= beat_us:
                    self._end_transition(res, tr["to_deck"])
            else:
                self._from_low_since = None
            if self.transition["active"]:
                res.mix["transition"] = dict(self.transition)
        return res
