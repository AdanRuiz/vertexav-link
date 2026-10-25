"""Show clock (§7.1) and receiver-side clock synchronisation (§7.2)."""
from __future__ import annotations

import statistics
import sys
import time
from collections import deque
from dataclasses import dataclass
from typing import Optional


def enable_fine_timers() -> None:
    """On Windows, raise timer resolution to 1 ms so asyncio sleeps are accurate enough."""
    if sys.platform == "win32":
        try:
            import ctypes

            ctypes.WinDLL("winmm").timeBeginPeriod(1)
        except OSError:
            pass


class ShowClock:
    """Monotonic microsecond clock that starts at 0 when created (§7.1)."""

    def __init__(self) -> None:
        self._start = time.perf_counter_ns()

    def now(self) -> int:
        return (time.perf_counter_ns() - self._start) // 1000


@dataclass
class ClockSample:
    rtt: int
    offset: float


class ClockSync:
    """NTP-like filter: window of 32 samples, rtt_min filter, median offset, slewed corrections."""

    WINDOW = 32
    INITIAL = 16
    MAX_SLEW_US_PER_S = 1000.0  # at most 1 ms per second once synced
    # On loopback rtt_min can be a few µs; 1.5x of that would reject almost everything.
    RTT_FLOOR_US = 200

    def __init__(self) -> None:
        self.samples: deque[ClockSample] = deque(maxlen=self.WINDOW)
        self.target: Optional[float] = None
        self.offset: Optional[float] = None
        self._last_local: Optional[int] = None
        self.total = 0
        self.bad_rtt_since: Optional[int] = None

    @property
    def synced(self) -> bool:
        return self.total >= self.INITIAL

    @property
    def rtt_min(self) -> Optional[int]:
        return min((s.rtt for s in self.samples), default=None)

    def add(self, t0: int, t1: int, t2: int, t3: int) -> ClockSample:
        rtt = (t3 - t0) - (t2 - t1)
        off = ((t1 - t0) + (t2 - t3)) / 2.0
        sample = ClockSample(rtt, off)
        self.samples.append(sample)
        self.total += 1
        rmin = self.rtt_min
        limit = max(1.5 * rmin, rmin + self.RTT_FLOOR_US)
        good = [s.offset for s in self.samples if s.rtt <= limit]
        self.target = statistics.median(good)
        if self.offset is None or not self.synced:
            self.offset = self.target
            self._last_local = t3
        # §7.2: warn if rtt_min > 20 ms for more than 10 s.
        if rmin > 20_000:
            if self.bad_rtt_since is None:
                self.bad_rtt_since = t3
        else:
            self.bad_rtt_since = None
        return sample

    def network_unfit(self, local_now: int) -> bool:
        return self.bad_rtt_since is not None and local_now - self.bad_rtt_since > 10_000_000

    def _slew(self, local_now: int) -> None:
        if self.offset is None or self.target is None:
            return
        if self._last_local is None:
            self._last_local = local_now
            return
        elapsed = max(0, local_now - self._last_local)
        self._last_local = local_now
        step = elapsed * self.MAX_SLEW_US_PER_S / 1e6
        diff = self.target - self.offset
        if abs(diff) <= step:
            self.offset = self.target
        else:
            self.offset += step if diff > 0 else -step

    def to_emitter(self, local_t: int) -> int:
        self._slew(local_t)
        if self.offset is None:
            raise RuntimeError("reloj sin sincronizar")
        return int(local_t + self.offset)

    def to_local(self, emitter_t: int) -> int:
        if self.offset is None:
            raise RuntimeError("reloj sin sincronizar")
        return int(emitter_t - self.offset)
