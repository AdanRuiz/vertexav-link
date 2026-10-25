"""Validation of control messages against the spec (used by the inspector and the tests).

`validate_message` checks a single JSON message in isolation. `SessionChecker` adds checks
that need history (seq, deltas, events, topics, rates).
"""
from __future__ import annotations

import base64
import re
from collections import deque
from dataclasses import dataclass
from typing import Any, Callable, Optional

from .proto import MAX_SAFE_INT, VERSION

SECTION_ROLES = {"intro", "verse", "chorus", "build", "drop", "breakdown", "bridge", "outro",
                 "silence", "unknown"}
MOODS = {"dark", "euphoric", "happy", "melancholic", "aggressive", "chill", "dreamy", "groovy",
         "epic", "neutral"}
INTENSITY = {"low", "medium", "high", "peak"}
SECURITY_MODES = {"open", "auth", "tls"}
TOPICS = {"tempo": 0, "deck": 1, "mix": 1, "track": 1, "event": 1, "intent": 1, "trigger": 1,
          "structure": 2, "features": 3, "audio": 3}
ERROR_CODES = {
    "unsupported_version": True, "auth_required": True, "auth_failed": True,
    "security_downgrade": True, "loop_detected": True, "message_too_large": True,
    "bad_message": False, "unsupported_profile": False, "unsupported_transport": False,
    "rate_limited": False, "internal": False,
}
EVENT_DATA = {
    "track.start": [],
    "track.stop": ["kind"],
    "transition.start": ["from_deck", "to_deck"],
    "transition.end": ["deck"],
    "section.change": ["from", "to", "section_index", "energy"],
    "countdown": ["target", "beats_remaining", "target_at"],
    "seek": ["from_s", "to_s"],
    "loop.on": ["in_s", "out_s", "beats"],
    "loop.off": ["in_s", "out_s", "beats"],
    "scratch.start": [],
    "scratch.end": [],
    "fx.on": ["name", "amount"],
    "fx.off": ["name", "amount"],
    "sampler.fire": ["slot", "name"],
    "bass.kill": [],
    "bass.return": [],
    "mix.cut": ["from_deck", "to_deck"],
    "deck.gate": ["state"],
    "source.lost": [],
    "source.restored": [],
    "on_air.change": ["from_deck", "to_deck"],
}
SCHEDULED_EVENTS = {"section.change", "countdown"}  # predictable, should arrive before `at`
TRIGGER_RE = re.compile(
    r"^(blackout|fullon|strobe|freeze|fog|mode\.(chill|build|peak)|color\.[a-z0-9_]+|fx\.\d+|custom\.[A-Za-z0-9_.\-]+)$"
)
TYPE_RE = re.compile(r"^([a-z0-9_]+(\.[a-z0-9_]+)*|x-[A-Za-z0-9_.\-]+)$")
HEX_COLOR = re.compile(r"^#[0-9A-Fa-f]{6}$")

# Message type -> topic, for subscription checks. None = session messages (always allowed).
MESSAGE_TOPIC = {
    "tempo": "tempo", "deck.state": "deck", "mix.state": "mix", "track.loaded": "track",
    "track.unloaded": "track", "intent": "intent", "trigger": "trigger", "event": "event",
    "event.cancel": "event", "track.structure": "structure", "features.config": "features",
    "audio.config": "audio",
}
STATE_TYPES = {"tempo", "deck.state", "mix.state", "intent"}
# Continuous fields per object (§8.4), as dotted paths.
CONTINUOUS = {
    "deck.state": {"audible", "fader", "filter", "eq.low", "eq.mid", "eq.high"},
    "mix.state": {"crossfader", "transition.progress"},
    "intent": {"energy", "valence", "arousal"},
}
SIGNED_CONTINUOUS = {"filter", "crossfader"}


@dataclass
class Issue:
    level: str  # "error" | "warn"
    msg: str
    ref: str = ""

    def __str__(self) -> str:
        r = f" ({self.ref})" if self.ref else ""
        return f"{self.level.upper()}: {self.msg}{r}"


class _V:
    """Small helper that accumulates issues while checking fields."""

    def __init__(self, msg: dict, prefix: str = ""):
        self.msg = msg
        self.prefix = prefix
        self.issues: list[Issue] = []

    def err(self, text: str, ref: str = "") -> None:
        self.issues.append(Issue("error", self.prefix + text, ref))

    def warn(self, text: str, ref: str = "") -> None:
        self.issues.append(Issue("warn", self.prefix + text, ref))

    def field(self, name: str, kind: str, required: bool = False, *, nullable: bool = False,
              rng: Optional[tuple] = None, enum: Optional[set] = None, ref: str = "",
              obj: Optional[dict] = None) -> Any:
        src = self.msg if obj is None else obj
        if name not in src:
            if required:
                self.err(f"falta el campo obligatorio '{name}'", ref)
            return None
        val = src[name]
        if val is None:
            if not nullable:
                self.err(f"'{name}' no puede ser null", ref)
            return None
        ok = {
            "int": isinstance(val, int) and not isinstance(val, bool),
            "num": isinstance(val, (int, float)) and not isinstance(val, bool),
            "str": isinstance(val, str),
            "bool": isinstance(val, bool),
            "obj": isinstance(val, dict),
            "list": isinstance(val, list),
            "any": True,
        }[kind]
        if not ok:
            self.err(f"'{name}' debe ser {kind}, es {type(val).__name__}", ref)
            return None
        if rng is not None and kind in ("int", "num"):
            lo, hi = rng
            if (lo is not None and val < lo) or (hi is not None and val > hi):
                self.err(f"'{name}'={val} fuera de rango [{lo}, {hi}]", ref)
        if enum is not None and val not in enum:
            self.err(f"'{name}'={val!r} no es uno de {sorted(enum)}", ref)
        return val


def _walk_numbers(obj: Any, path: str = ""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from _walk_numbers(v, f"{path}.{k}" if path else k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from _walk_numbers(v, f"{path}[{i}]")
    elif isinstance(obj, (int, float)) and not isinstance(obj, bool):
        yield path, obj


def _b64_len(text: Any) -> Optional[int]:
    if not isinstance(text, str):
        return None
    try:
        return len(base64.b64decode(text, validate=True))
    except Exception:
        return None


# --- per-type checkers -------------------------------------------------------------------

def _hello(v: _V) -> None:
    v.field("protocol", "str", True, enum={"vertexav"}, ref="§6.2")
    v.field("version", "str", True, ref="§6.2")
    v.field("role", "str", True, enum={"receiver", "bridge"}, ref="§6.2")
    v.field("name", "str", True, ref="§6.2")
    v.field("client_id", "str", True, ref="§6.2")
    profiles = v.field("profiles", "list", True, ref="§6.2")
    for p in profiles or []:
        if not isinstance(p, int) or not 0 <= p <= 3:
            v.err(f"perfil inválido {p!r}", "§4")
    v.field("latency_us", "int", rng=(0, 10_000_000), ref="§6.2")
    v.field("udp_port", "int", rng=(1, 65535), ref="§6.2")
    v.field("realtime_over_control", "bool", ref="§6.2")


def _welcome(v: _V) -> None:
    ver = v.field("version", "str", True, ref="§6.3")
    if ver and ver.split(".")[0] != VERSION.split(".")[0]:
        v.err(f"versión mayor incompatible {ver}", "§14")
    v.field("session_id", "str", True, ref="§6.3")
    v.field("session_key", "int", True, rng=(1, 0xFFFFFFFF), ref="§6.3")
    em = v.field("emitter", "obj", True, ref="§6.3")
    if em is not None:
        v.field("id", "str", True, obj=em, ref="§5.5")
        v.field("name", "str", True, obj=em, ref="§6.3")
        v.field("role", "str", True, enum={"emitter", "bridge"}, obj=em, ref="§6.3")
        path = v.field("path", "list", True, obj=em, ref="§5.5")
        if path is not None:
            if not path or not all(isinstance(p, str) for p in path):
                v.err("emitter.path debe ser una lista no vacía de id", "§5.5")
            elif path[-1] != em.get("id"):
                v.err("el último elemento de emitter.path debe ser emitter.id", "§5.5")
            if len(path) - 1 > 4:
                v.err("más de 4 saltos en emitter.path", "§5.5")
            if em.get("role") == "emitter" and len(path) != 1:
                v.warn("un emisor primario debería tener path de un solo id", "§5.5")
    sec = v.field("security", "str", True, enum=SECURITY_MODES, ref="§6.3")
    if "server_proof" not in v.msg:
        v.err("falta 'server_proof' (null en modo open)", "§6.3")
    else:
        sp = v.msg["server_proof"]
        if sec == "open" and sp is not None:
            v.err("server_proof debe ser null en modo open", "§6.3")
        if sec in ("auth", "tls") and _b64_len(sp) != 32:
            v.err("server_proof debe ser HMAC-SHA256 en base64 (32 bytes)", "§15.4")
    profiles = v.field("profiles", "list", True, ref="§6.3")
    for p in profiles or []:
        if not isinstance(p, int) or not 0 <= p <= 3:
            v.err(f"perfil inválido {p!r}", "§4")
    v.field("udp_port", "int", nullable=True, rng=(1, 65535), ref="§6.3")
    v.field("decks", "int", rng=(0, 64), ref="§6.3")
    v.field("lookahead_us", "int", rng=(0, 5_000_000), ref="§6.3")
    v.field("link", "obj", nullable=True, ref="§6.3")


def _subscribe(v: _V) -> None:
    topics = v.field("topics", "list", True, ref="§6.4")
    for t in topics or []:
        if t not in TOPICS:
            v.err(f"tema desconocido {t!r}", "§6.4")


def _tempo(v: _V, full: bool = True) -> None:
    bpm = v.field("bpm", "num", full, rng=(0, 1000), ref="§9.2")
    if bpm is not None and v.msg.get("playing") and not 20 <= bpm <= 400:
        v.warn(f"bpm={bpm} poco realista", "§9.2")
    v.field("anchor_t", "int", full, rng=(0, MAX_SAFE_INT - 1), ref="§9.2")
    v.field("anchor_beat", "num", full, ref="§9.2")
    v.field("beats_per_bar", "int", full, rng=(1, 32), ref="§9.2")
    v.field("bar_offset", "num", ref="§9.2")
    v.field("source_deck", "int", nullable=True, rng=(1, 64), ref="§9.2")
    v.field("playing", "bool", full, ref="§9.2")
    v.field("confidence", "num", rng=(0, 1), ref="§9.2")
    v.field("stability", "str", full, enum={"stable", "unstable", "freewheel"}, ref="§9.4")
    v.field("phase_reset", "bool", ref="§9.2")
    v.field("link_aligned", "bool", nullable=True, ref="§9.2")


def _deck_state(v: _V, full: bool = False) -> None:
    v.field("deck", "int", True, rng=(1, 64), ref="§10.1")
    v.field("track_id", "str", nullable=True, ref="§10.1")
    v.field("loaded", "bool", full, ref="§10.1")
    v.field("playing", "bool", full, ref="§10.1")
    pos = v.field("position_s", "num", nullable=True, ref="§10.1")
    if pos is not None and pos < -60:
        v.warn(f"position_s={pos} muy negativa", "§10.1")
    v.field("position_beat", "num", nullable=True, ref="§10.1")
    v.field("rate", "num", rng=(-10, 10), ref="§10.1")
    v.field("bpm", "num", nullable=True, rng=(0, 1000), ref="§10.1")
    v.field("key", "str", nullable=True, ref="§10.1")
    loop = v.field("loop", "obj", ref="§10.1")
    if loop is not None:
        v.field("active", "bool", obj=loop, ref="§10.1")
        v.field("in_s", "num", nullable=True, obj=loop, ref="§10.1")
        v.field("out_s", "num", nullable=True, obj=loop, ref="§10.1")
        if loop.get("in_s") is not None and loop.get("out_s") is not None and loop["out_s"] <= loop["in_s"]:
            v.err("loop.out_s debe ser mayor que loop.in_s", "§10.1")
    v.field("on_air", "bool", ref="§10.1")
    v.field("audible", "num", rng=(0, 1), ref="§10.1")
    v.field("fader", "num", rng=(0, 1), ref="§10.1")
    eq = v.field("eq", "obj", ref="§10.1")
    if eq is not None:
        for b in ("low", "mid", "high"):
            v.field(b, "num", rng=(0, 1), obj=eq, ref="§10.1")
            v.field(f"{b}_kill", "bool", obj=eq, ref="§10.1")
    v.field("filter", "num", rng=(-1, 1), ref="§10.1")
    v.field("pfl", "bool", ref="§10.1")
    v.field("scratching", "bool", ref="§10.1")
    slip = v.field("slip", "obj", ref="§10.1")
    if slip is not None:
        v.field("active", "bool", obj=slip, ref="§10.1")
        v.field("position_s", "num", nullable=True, obj=slip, ref="§10.1")


def _mix_state(v: _V, full: bool = False) -> None:
    v.field("crossfader", "num", full, rng=(-1, 1), ref="§10.2")
    v.field("on_air_deck", "int", nullable=True, rng=(1, 64), ref="§10.2")
    tr = v.field("transition", "obj", ref="§10.2")
    if tr is not None:
        v.field("active", "bool", full, obj=tr, ref="§10.2")
        v.field("kind", "str", enum={"blend", "cut"}, nullable=True, obj=tr, ref="§10.2")
        v.field("from_deck", "int", nullable=True, obj=tr, ref="§10.2")
        v.field("to_deck", "int", nullable=True, obj=tr, ref="§10.2")
        v.field("progress", "num", rng=(0, 1), nullable=True, obj=tr, ref="§10.2")


def _track_loaded(v: _V) -> None:
    v.field("deck", "int", True, rng=(1, 64), ref="§10.3")
    v.field("track_id", "str", True, ref="§10.3")
    meta = v.field("meta", "obj", ref="§10.3")
    if meta is not None:
        for k in ("title", "artist", "album", "genre", "comment", "key"):
            v.field(k, "str", nullable=True, obj=meta, ref="§10.3")
        v.field("year", "int", nullable=True, obj=meta, ref="§10.3")
        v.field("duration_s", "num", nullable=True, rng=(0, None), obj=meta, ref="§10.3")
        v.field("bpm", "num", nullable=True, rng=(0, 1000), obj=meta, ref="§10.3")
        v.field("key_notation", "str", nullable=True, enum={"camelot", "openkey", "traditional"},
                obj=meta, ref="§10.3")
        color = v.field("color", "str", nullable=True, obj=meta, ref="§10.3")
        if color is not None and not HEX_COLOR.match(color):
            v.err(f"color {color!r} no es #RRGGBB", "§10.3")
        v.field("rating", "int", nullable=True, rng=(0, 5), obj=meta, ref="§10.3")
        tags = v.field("tags", "list", nullable=True, obj=meta, ref="§10.3")
        if tags and not all(isinstance(t, str) for t in tags):
            v.err("meta.tags debe ser una lista de textos", "§10.3")
    art = v.field("artwork", "obj", nullable=True, ref="§10.3")
    if art is not None:
        v.field("url", "str", True, obj=art, ref="§10.3")
        v.field("hash", "str", obj=art, ref="§10.3")
    v.field("structure_available", "bool", ref="§10.3")


def _intent(v: _V, full: bool = False) -> None:
    v.field("energy", "num", rng=(0, 1), ref="§10.4")
    v.field("mood", "str", nullable=True, enum=MOODS, ref="B.2")
    v.field("valence", "num", rng=(0, 1), ref="§10.4")
    v.field("arousal", "num", rng=(0, 1), ref="§10.4")
    pal = v.field("palette", "list", ref="§10.4")
    if pal is not None:
        if not 1 <= len(pal) <= 6:
            v.err(f"palette debe tener 1 a 6 colores, tiene {len(pal)}", "§10.4")
        for c in pal:
            if not isinstance(c, str) or not HEX_COLOR.match(c):
                v.err(f"color de palette inválido {c!r}", "§10.4")
    v.field("intensity_hint", "str", enum=INTENSITY, ref="§10.4")
    v.field("strobe_ok", "bool", ref="§10.4")


def _trigger(v: _V) -> None:
    name = v.field("name", "str", True, ref="§10.5")
    if name is not None and not TRIGGER_RE.match(name):
        v.warn(f"disparo no estándar {name!r}: los propios usan 'custom.'", "B.3")
    v.field("state", "str", True, enum={"on", "off", "momentary"}, ref="§10.5")
    v.field("at", "int", rng=(0, MAX_SAFE_INT - 1), ref="§10.5")
    v.field("value", "num", nullable=True, rng=(0, 1), ref="§10.5")
    v.field("deck", "int", nullable=True, rng=(1, 64), ref="§10.5")


def _event(v: _V) -> None:
    v.field("at", "int", True, rng=(0, MAX_SAFE_INT - 1), ref="§10.6")
    v.field("event_id", "str", True, ref="§10.6")
    name = v.field("name", "str", True, ref="§10.6")
    v.field("deck", "int", nullable=True, rng=(1, 64), ref="§10.6")
    data = v.field("data", "obj", ref="§10.6")
    if name is None:
        return
    if name not in EVENT_DATA:
        if not name.startswith("x-"):
            v.warn(f"evento no estándar {name!r}", "B.4")
        return
    data = data or {}
    for k in EVENT_DATA[name]:
        if k not in data:
            v.err(f"evento {name}: falta data.{k}", "B.4")
    if name == "section.change":
        for k in ("from", "to"):
            if k in data and data[k] is not None and data[k] not in SECTION_ROLES:
                v.err(f"section.change: data.{k}={data[k]!r} no es un rol válido", "B.1")
        v.field("section_index", "int", rng=(0, None), obj=data, ref="§11.3")
        v.field("energy", "num", rng=(0, 1), nullable=True, obj=data, ref="§11.3")
    elif name == "countdown":
        if data.get("target") not in SECTION_ROLES:
            v.err(f"countdown: target={data.get('target')!r} no es un rol válido", "§11.4")
        br = v.field("beats_remaining", "int", rng=(1, None), obj=data, ref="§11.4")
        if br is not None and br not in (16, 8, 4, 2, 1):
            v.warn(f"countdown: beats_remaining={br} no es un valor recomendado", "§11.4")
        ta = v.field("target_at", "int", obj=data, ref="§11.4")
        if ta is not None and isinstance(v.msg.get("at"), int) and ta <= v.msg["at"]:
            v.err("countdown: target_at debe ser posterior a at", "§11.4")
    elif name == "track.stop":
        v.field("kind", "str", enum={"pause", "brake", "end"}, obj=data, ref="B.4")
    elif name == "deck.gate":
        v.field("state", "str", enum={"closed", "open"}, obj=data, ref="B.4")


def _event_cancel(v: _V) -> None:
    v.field("event_id", "str", True, ref="§10.6")
    v.field("reason", "str", ref="§10.6")


def _structure(v: _V) -> None:
    v.field("track_id", "str", True, ref="§11.1")
    v.field("analysis", "obj", ref="§11.1")
    bg = v.field("beatgrid", "obj", True, ref="§11.1")
    if bg is not None:
        kind = v.field("type", "str", True, enum={"constant", "variable"}, obj=bg, ref="§11.1")
        if kind == "constant":
            v.field("bpm", "num", True, rng=(1, 1000), obj=bg, ref="§11.1")
            v.field("first_beat_s", "num", True, obj=bg, ref="§11.1")
        elif kind == "variable":
            beats = v.field("beats_s", "list", True, obj=bg, ref="§11.1")
            if beats and any(b2 <= b1 for b1, b2 in zip(beats, beats[1:])):
                v.err("beatgrid.beats_s debe ser estrictamente creciente", "§11.1")
        v.field("beats_per_bar", "int", rng=(1, 32), obj=bg, ref="§11.1")
        v.field("first_downbeat_beat", "int", obj=bg, ref="§11.1")
    secs = v.field("sections", "list", True, ref="§11.1")
    prev_end = None
    for i, s in enumerate(secs or []):
        if not isinstance(s, dict):
            v.err(f"sections[{i}] no es un objeto", "§11.1")
            continue
        sv = _V(s, f"sections[{i}].")
        st = sv.field("start_beat", "num", True, ref="§11.1")
        en = sv.field("end_beat", "num", True, ref="§11.1")
        sv.field("role", "str", True, enum=SECTION_ROLES, ref="B.1")
        sv.field("energy", "num", rng=(0, 1), ref="§11.1")
        sv.field("confidence", "num", rng=(0, 1), ref="§11.1")
        v.issues += sv.issues
        if st is not None and en is not None:
            if en <= st:
                v.err(f"sections[{i}]: end_beat <= start_beat", "§11.1")
            if prev_end is not None and st < prev_end:
                v.err(f"sections[{i}] se solapa con la anterior o no está ordenada", "§11.1")
            prev_end = en
    ec = v.field("energy_curve", "obj", ref="§11.1")
    if ec is not None:
        res = ec.get("resolution")
        if not (res in ("beat", "bar") or (isinstance(res, (int, float)) and not isinstance(res, bool) and res > 0)):
            v.err(f"energy_curve.resolution inválida {res!r}", "§11.1")
        vals = v.field("values", "list", True, obj=ec, ref="§11.1")
        if vals and any((not isinstance(x, (int, float))) or not 0 <= x <= 1 for x in vals):
            v.err("energy_curve.values fuera de 0–1", "§11.1")
    marks = v.field("markers", "list", ref="§11.1")
    for i, m in enumerate(marks or []):
        if not isinstance(m, dict) or "kind" not in m or "position_s" not in m:
            v.err(f"markers[{i}] necesita kind y position_s", "§11.1")
        elif m.get("role") is not None and m["role"] not in SECTION_ROLES:
            v.warn(f"markers[{i}].role={m['role']!r} no es un rol de sección", "§11.1")


def _error(v: _V) -> None:
    code = v.field("code", "str", True, ref="§14")
    v.field("message", "str", ref="§14")
    fatal = v.field("fatal", "bool", True, ref="§14")
    if code in ERROR_CODES and fatal is not None and fatal != ERROR_CODES[code]:
        v.err(f"error {code} debe tener fatal={ERROR_CODES[code]}", "§14")
    elif code is not None and code not in ERROR_CODES:
        v.warn(f"código de error no estándar {code!r}", "§14")


def _auth_challenge(v: _V) -> None:
    if _b64_len(v.msg.get("server_nonce")) != 16:
        v.err("server_nonce debe ser 16 bytes en base64", "§15.4")


def _auth_response(v: _V) -> None:
    if _b64_len(v.msg.get("client_nonce")) != 16:
        v.err("client_nonce debe ser 16 bytes en base64", "§15.4")
    if _b64_len(v.msg.get("proof")) != 32:
        v.err("proof debe ser 32 bytes en base64", "§15.4")


def _snapshot(v: _V) -> None:
    v.field("t", "int", True, ref="§8.2")
    tempo = v.field("tempo", "obj", nullable=True, ref="§8.2")
    if tempo is not None:
        sub = _V(tempo, "snapshot.tempo.")
        _tempo(sub, True)
        v.issues += sub.issues
    for key, fn in (("decks", lambda s: _deck_state(s, True)), ("tracks", _track_loaded),
                    ("structures", _structure), ("events", _event)):
        items = v.field(key, "list", ref="§8.2")
        for i, item in enumerate(items or []):
            if not isinstance(item, dict):
                v.err(f"snapshot.{key}[{i}] no es un objeto", "§8.2")
                continue
            sub = _V(item, f"snapshot.{key}[{i}].")
            fn(sub)
            v.issues += sub.issues
    for key, fn in (("mix", lambda s: _mix_state(s, True)), ("intent", _intent)):
        item = v.field(key, "obj", nullable=True, ref="§8.2")
        if item is not None:
            sub = _V(item, f"snapshot.{key}.")
            fn(sub)
            v.issues += sub.issues


CHECKERS: dict[str, Callable[[_V], None]] = {
    "hello": _hello,
    "welcome": _welcome,
    "subscribe": _subscribe,
    "snapshot": _snapshot,
    "heartbeat": lambda v: v.field("t", "int", True, ref="§6.5"),
    "bye": lambda v: v.field("reason", "str", ref="§6.6"),
    "clock.ping": lambda v: (v.field("t0", "int", True, ref="§7.2"),
                             v.field("ping_id", "int", True, ref="§7.2")),
    "clock.pong": lambda v: [v.field(k, "int", True, ref="§7.2") for k in ("t0", "ping_id", "t1", "t2")],
    "tempo": _tempo,
    "deck.state": _deck_state,
    "mix.state": _mix_state,
    "track.loaded": _track_loaded,
    "track.unloaded": lambda v: v.field("deck", "int", True, rng=(1, 64), ref="§10.3"),
    "intent": _intent,
    "trigger": _trigger,
    "event": _event,
    "event.cancel": _event_cancel,
    "track.structure": _structure,
    "features.config": lambda v: (v.field("rate_hz", "num", True, ref="§12.2"),
                                  v.field("sources", "list", True, ref="§12.2"),
                                  v.field("bands", "obj", True, ref="§12.2")),
    "audio.subscribe": lambda v: (v.field("sources", "list", True, ref="§12.4"),
                                  v.field("transport", "str", True, enum={"shm", "udp"}, ref="§12.4")),
    "audio.config": lambda v: (v.field("transport", "str", True, enum={"shm", "udp"}, ref="§12.4"),
                               v.field("sample_rate", "int", True, ref="§12.4"),
                               v.field("channels", "int", True, ref="§12.4"),
                               v.field("format", "str", True, enum={"f32le", "s16le"}, ref="§12.4")),
    "receiver.state": lambda v: v.field("status", "str", ref="§13.1"),
    "receiver.capabilities": lambda v: v.field("triggers", "list", True, ref="§13.2"),
    "latency.update": lambda v: v.field("latency_us", "int", True, rng=(0, 10_000_000), ref="§13.3"),
    "error": _error,
    "auth.challenge": _auth_challenge,
    "auth.response": _auth_response,
}
EMITTER_TO_RECEIVER = {"welcome", "snapshot", "tempo", "deck.state", "mix.state", "track.loaded",
                       "track.unloaded", "intent", "trigger", "event", "event.cancel",
                       "track.structure", "features.config", "audio.config", "auth.challenge",
                       "clock.pong"}
RECEIVER_TO_EMITTER = {"hello", "subscribe", "audio.subscribe", "receiver.state",
                       "receiver.capabilities", "latency.update", "auth.response", "clock.ping"}


def validate_message(msg: Any, direction: str = "e2r") -> list[Issue]:
    """Check one decoded JSON message. direction: 'e2r' (emitter→receiver) or 'r2e'."""
    if not isinstance(msg, dict):
        return [Issue("error", "el mensaje no es un objeto JSON", "§8.1")]
    v = _V(msg)
    mtype = msg.get("type")
    if not isinstance(mtype, str):
        v.err("falta 'type' o no es texto", "§8.1")
        return v.issues
    if not TYPE_RE.match(mtype):
        v.err(f"type {mtype!r} debe ir en minúsculas con puntos", "§8.1")
    v.field("t", "int", rng=(0, MAX_SAFE_INT - 1), ref="§8.1")
    v.field("seq", "int", rng=(0, None), ref="§8.1")
    for path, num in _walk_numbers(msg):
        if abs(num) >= MAX_SAFE_INT:
            v.err(f"{path}={num} no cabe sin pérdida en JSON (>= 2^53)", "§7.1")
    if direction == "e2r" and mtype in RECEIVER_TO_EMITTER:
        v.err(f"'{mtype}' solo lo envía el receptor", "§6")
    if direction == "r2e" and mtype in EMITTER_TO_RECEIVER:
        v.err(f"'{mtype}' solo lo envía el emisor", "§6")
    if direction == "e2r" and mtype in STATE_TYPES and "t" not in msg:
        v.warn("los mensajes de estado deberían llevar 't'", "§8.2")
    checker = CHECKERS.get(mtype)
    if checker is None:
        if not mtype.startswith("x-"):
            v.warn(f"tipo de mensaje desconocido {mtype!r} (el receptor debe ignorarlo)", "§8.1")
        return v.issues
    checker(v)
    return v.issues


def _flatten(obj: dict, prefix: str = "") -> dict:
    out = {}
    for k, val in obj.items():
        key = f"{prefix}{k}"
        if isinstance(val, dict):
            out.update(_flatten(val, key + "."))
        else:
            out[key] = val
    return out


def _is_edge(name: str, value: Any) -> bool:
    if not isinstance(value, (int, float)):
        return True
    if name in SIGNED_CONTINUOUS:
        return value in (-1, 0, 1)
    return value in (0, 1)


class SessionChecker:
    """Stateful checks over a receiver's view of one session."""

    def __init__(self) -> None:
        self.profiles: set[int] = set()
        self.topics: Optional[set[str]] = None
        self.last_seq: Optional[int] = None
        self.last_t_by_obj: dict[str, int] = {}
        self.events: dict[str, dict] = {}
        self.cancelled: set[str] = set()
        self.tempo: Optional[dict] = None
        self.tracks: dict[int, str] = {}
        self.structures: dict[str, dict] = {}
        self.lookahead_us = 0
        self.snapshot_seen = False
        self.rate_window: deque[float] = deque()
        self.cont_last: dict[str, tuple[float, dict]] = {}
        self.rate_warned = False
        self.transition_since: Optional[float] = None
        self.transition_warned = False

    def on_sent(self, msg: dict) -> None:
        """Record what the receiver sent (subscribe defines topics)."""
        if msg.get("type") == "subscribe":
            self.topics = set(msg.get("topics", []))

    def check(self, msg: dict, now_s: float) -> list[Issue]:
        issues: list[Issue] = []

        def err(text: str, ref: str = "") -> None:
            issues.append(Issue("error", text, ref))

        def warn(text: str, ref: str = "") -> None:
            issues.append(Issue("warn", text, ref))

        mtype = msg.get("type")
        # seq must grow within a connection (§8.1)
        seq = msg.get("seq")
        if isinstance(seq, int):
            if self.last_seq is not None and seq <= self.last_seq:
                err(f"seq {seq} no crece (anterior {self.last_seq})", "§8.1")
            self.last_seq = seq
        # message rate (§8.4 target < 100/s)
        self.rate_window.append(now_s)
        while self.rate_window and now_s - self.rate_window[0] > 1.0:
            self.rate_window.popleft()
        if len(self.rate_window) > 100 and not self.rate_warned:
            warn("más de 100 mensajes por segundo en el canal de control", "§8.4")
            self.rate_warned = True
        elif len(self.rate_window) < 80:
            self.rate_warned = False

        if mtype == "welcome":
            self.profiles = set(msg.get("profiles") or [])
            self.lookahead_us = msg.get("lookahead_us") or 0
            return issues
        if mtype == "snapshot":
            self.snapshot_seen = True
            if msg.get("tempo"):
                self.tempo = msg["tempo"]
            for tr in msg.get("tracks") or []:
                self.tracks[tr.get("deck")] = tr.get("track_id")
            for st in msg.get("structures") or []:
                self.structures[st.get("track_id")] = st
            for ev in msg.get("events") or []:
                self.events[ev.get("event_id")] = ev
            return issues

        topic = MESSAGE_TOPIC.get(mtype)
        if topic is not None:
            if not self.snapshot_seen:
                err(f"'{mtype}' llegó antes del snapshot", "§6.1")
            if TOPICS[topic] not in self.profiles and self.profiles:
                err(f"'{mtype}' pertenece al perfil {TOPICS[topic]}, no acordado en welcome", "§6.4")
            elif self.topics is not None and topic not in self.topics:
                err(f"'{mtype}' (tema {topic}) no está en la suscripción", "§6.4")

        # Deltas older than the last applied for the same object (§8.2)
        if mtype in STATE_TYPES and isinstance(msg.get("t"), int):
            obj = mtype + (f":{msg.get('deck')}" if mtype == "deck.state" else "")
            last = self.last_t_by_obj.get(obj)
            if last is not None and msg["t"] < last:
                err(f"{obj}: delta con t={msg['t']} anterior al último ({last})", "§8.2")
            self.last_t_by_obj[obj] = msg["t"]
            # Continuous-value throttling (§8.4)
            cont = CONTINUOUS.get(mtype)
            if cont:
                flat = _flatten({k: v for k, v in msg.items() if k not in ("type", "t", "seq")})
                changed = {k: v for k, v in flat.items() if k in cont}
                discrete = {k for k in flat if k not in cont and k != "deck"}
                if changed and not discrete:
                    prev = self.cont_last.get(obj)
                    if prev is not None:
                        dt = now_s - prev[0]
                        edge = any(_is_edge(k, v) for k, v in changed.items())
                        if dt < 0.040 and not edge:
                            warn(f"{obj}: valores continuos a más de 20 Hz (Δ={dt*1000:.0f} ms)", "§8.4")
                        elif dt < 0.045 and not edge:
                            small = all(
                                isinstance(v, (int, float)) and isinstance(prev[1].get(k), (int, float))
                                and abs(v - prev[1][k]) < 0.01 for k, v in changed.items())
                            if small:
                                warn(f"{obj}: cambio menor que la zona muerta 0,01", "§8.4")
                    merged = dict(prev[1]) if prev else {}
                    merged.update(changed)
                    self.cont_last[obj] = (now_s, merged)

        if mtype == "mix.state" and isinstance(msg.get("transition"), dict) and "active" in msg["transition"]:
            if msg["transition"]["active"]:
                if self.transition_since is None:
                    self.transition_since = now_s
            else:
                self.transition_since = None
                self.transition_warned = False
        if self.transition_since is not None and now_s - self.transition_since > 90 and not self.transition_warned:
            warn("transition.active lleva más de 90 s: ¿transición atascada?", "§10.2")
            self.transition_warned = True

        if mtype == "tempo":
            self.tempo = msg
        elif mtype == "track.loaded":
            self.tracks[msg.get("deck")] = msg.get("track_id")
        elif mtype == "track.unloaded":
            self.tracks.pop(msg.get("deck"), None)
        elif mtype == "track.structure":
            tid = msg.get("track_id")
            if tid not in self.tracks.values():
                warn(f"track.structure de {tid} que no está cargado en ningún deck", "§11.1")
            self.structures[tid] = msg
        elif mtype == "deck.state":
            tid = msg.get("track_id")
            if tid is not None and self.tracks.get(msg.get("deck")) not in (None, tid):
                err(f"deck {msg.get('deck')}: track_id {tid} distinto del último track.loaded", "§10.3")
        elif mtype == "event":
            eid = msg.get("event_id")
            if eid in self.events:
                err(f"event_id {eid} repetido", "§10.6")
            self.events[eid] = msg
            at, t = msg.get("at"), msg.get("t")
            name = msg.get("name")
            if isinstance(at, int) and isinstance(t, int):
                if name in SCHEDULED_EVENTS and at <= t:
                    warn(f"{name} previsible publicado sin antelación (at - t = {(at - t)/1000:.1f} ms)", "§7.4")
                if self.lookahead_us and at - t > self.lookahead_us * 4:
                    warn(f"{name} publicado {(at - t)/1000:.0f} ms antes, mucho más que lookahead_us", "§7.4")
            data = msg.get("data") or {}
            if name == "countdown" and self.tempo and isinstance(data.get("target_at"), int) and isinstance(at, int):
                bpm = self.tempo.get("bpm") or 0
                if bpm > 0 and isinstance(data.get("beats_remaining"), int):
                    expected = data["beats_remaining"] * 60e6 / bpm
                    diff = abs((data["target_at"] - at) - expected)
                    if diff > 5000:
                        warn(f"countdown: target_at - at difiere {diff/1000:.1f} ms de beats_remaining × periodo", "§11.4")
            if name == "section.change":
                tid = self.tracks.get(msg.get("deck"))
                st = self.structures.get(tid)
                idx = data.get("section_index")
                if st and isinstance(idx, int):
                    secs = st.get("sections") or []
                    if idx >= len(secs):
                        err(f"section.change: section_index {idx} no existe en el mapa", "§11.3")
                    elif secs[idx].get("role") != data.get("to"):
                        err(f"section.change: to={data.get('to')} pero la sección {idx} es {secs[idx].get('role')}", "§11.3")
        elif mtype == "event.cancel":
            eid = msg.get("event_id")
            if eid not in self.events:
                warn(f"event.cancel de un event_id desconocido {eid}", "§10.6")
            elif eid in self.cancelled:
                warn(f"event.cancel repetido para {eid}", "§10.6")
            else:
                at = self.events[eid].get("at")
                if isinstance(at, int) and isinstance(msg.get("t"), int) and msg["t"] > at:
                    warn(f"event.cancel de {eid} después de su instante", "§7.4")
            self.cancelled.add(eid)
        return issues
