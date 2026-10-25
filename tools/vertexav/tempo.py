"""Tempo model (§9.1) and receiver-side tempo follower with the smoothing rules of §9.4."""
from __future__ import annotations

import math
from dataclasses import dataclass, field, replace
from typing import Optional


@dataclass
class Tempo:
    bpm: float = 120.0
    anchor_t: int = 0
    anchor_beat: float = 0.0
    beats_per_bar: int = 4
    bar_offset: float = 0.0
    source_deck: int = 1
    playing: bool = False
    confidence: float = 1.0
    stability: str = "stable"
    phase_reset: bool = False
    link_aligned: Optional[bool] = None

    @property
    def beat_us(self) -> float:
        return 60e6 / self.bpm if self.bpm > 0 else math.inf

    def beat_at(self, t: int) -> float:
        return self.anchor_beat + (t - self.anchor_t) / 1e6 * self.bpm / 60.0

    def time_of_beat(self, beat: float) -> int:
        return int(round(self.anchor_t + (beat - self.anchor_beat) * 60e6 / self.bpm))

    def next_beat_index(self, t: int) -> int:
        return math.floor(self.beat_at(t) + 1e-9) + 1

    def next_beat_t(self, t: int) -> int:
        return self.time_of_beat(self.next_beat_index(t))

    def next_downbeat_index(self, t: int) -> float:
        b = self.beat_at(t)
        bpb = max(1, self.beats_per_bar)
        m = math.floor((b - self.bar_offset) / bpb + 1e-9) + 1
        return self.bar_offset + m * bpb

    def next_downbeat_t(self, t: int) -> int:
        return self.time_of_beat(self.next_downbeat_index(t))

    def beat_in_bar(self, beat: float) -> int:
        """1-based position inside the bar of an integer beat."""
        bpb = max(1, self.beats_per_bar)
        return int(round(beat - self.bar_offset)) % bpb + 1

    def rebase(self, t: int) -> "Tempo":
        """Same timeline, anchor moved to t."""
        return replace(self, anchor_t=t, anchor_beat=self.beat_at(t))

    def to_json(self) -> dict:
        return {
            "bpm": round(self.bpm, 6),
            "anchor_t": int(self.anchor_t),
            "anchor_beat": round(self.anchor_beat, 9),
            "beats_per_bar": self.beats_per_bar,
            "bar_offset": self.bar_offset,
            "source_deck": self.source_deck,
            "playing": self.playing,
            "confidence": round(self.confidence, 3),
            "stability": self.stability,
            "phase_reset": self.phase_reset,
            "link_aligned": self.link_aligned,
        }

    @classmethod
    def from_json(cls, msg: dict, base: Optional["Tempo"] = None) -> "Tempo":
        t = replace(base) if base else cls()
        for k in ("bpm", "anchor_t", "anchor_beat", "beats_per_bar", "bar_offset", "source_deck",
                  "playing", "confidence", "stability", "phase_reset", "link_aligned"):
            if k in msg:
                setattr(t, k, msg[k])
        return t


@dataclass
class TempoFollower:
    """Receiver-side tempo following (§9.4 "Reglas para el receptor").

    Works in emitter show time. Feed updates with `update(tempo, now)` and read the smoothed
    beat position with `beat_at(now)`.
    """

    MAX_CORR_PER_BEAT = 0.02  # beats of phase correction per beat elapsed
    JUMP_THRESHOLD = 0.25  # beats
    UNSTABLE_MAX_BEATS = 8

    model: Optional[Tempo] = None
    last_stable: Optional[Tempo] = None
    corr: float = 0.0  # beats to subtract from model.beat_at(t)
    corr_t: int = 0
    unstable_since_beat: Optional[float] = None
    jumps: int = 0
    smooth_corrections: int = 0
    _last_key: tuple = field(default_factory=tuple)

    def _corr_at(self, t: int) -> float:
        if self.corr == 0.0 or self.model is None:
            return 0.0
        elapsed_beats = max(0.0, (t - self.corr_t) / self.model.beat_us)
        budget = elapsed_beats * self.MAX_CORR_PER_BEAT
        if abs(self.corr) <= budget:
            return 0.0
        return self.corr - math.copysign(budget, self.corr)

    def beat_at(self, t: int) -> Optional[float]:
        if self.model is None:
            return None
        return self.model.beat_at(t) - self._corr_at(t)

    @property
    def tempo(self) -> Optional[Tempo]:
        return self.model

    def beats_enabled(self, t: int) -> bool:
        """False when nothing is playing or the source has been unstable for more than 8 beats."""
        if self.model is None or not self.model.playing:
            return False
        if self.unstable_since_beat is not None:
            b = self.beat_at(t)
            if b is not None and b - self.unstable_since_beat > self.UNSTABLE_MAX_BEATS:
                return False
        return True

    def update(self, new: Tempo, now: int) -> str:
        """Apply an incoming tempo. Returns what happened: init/same/jump/smooth/frozen/stop."""
        key = (new.bpm, new.anchor_t, new.anchor_beat, new.stability, new.playing, new.beats_per_bar,
               new.bar_offset)
        if key == self._last_key:
            return "same"  # includes repeated phase_reset flags for the same anchor
        self._last_key = key
        if new.stability == "unstable":
            # Freeze phase: keep following the last stable model, ignore incoming anchors.
            if self.unstable_since_beat is None:
                self.unstable_since_beat = self.beat_at(now) if self.model else 0.0
            if self.model is None:
                self.model = replace(new)
            return "frozen"
        self.unstable_since_beat = None
        if self.model is None:
            self.model = replace(new, phase_reset=False)
            self.last_stable = self.model
            return "init"
        current = self.beat_at(now)
        candidate = new.beat_at(now)
        err = candidate - current
        if new.phase_reset or abs(err) > self.JUMP_THRESHOLD or not new.playing:
            self.model = replace(new, phase_reset=False)
            self.corr = 0.0
            self.last_stable = self.model
            if new.phase_reset or abs(err) > self.JUMP_THRESHOLD:
                self.jumps += 1
                return "jump"
            return "stop"
        # Gradual correction: new timeline minus the remaining error, decaying at 2 %/beat.
        self.model = replace(new, phase_reset=False)
        self.corr = err
        self.corr_t = now
        self.last_stable = self.model
        if abs(err) > 1e-6:
            self.smooth_corrections += 1
        return "smooth"
