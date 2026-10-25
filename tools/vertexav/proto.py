"""Wire format of VertexAV Link 0.6: constants, binary UDP packets and sequence windows.

References in comments (§x.y) point to spec/vertexav-link-spec.md.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

from . import security

PROTOCOL = "vertexav"
VERSION = "0.6"
CONTROL_PORT = 9470
REALTIME_PORT = 9471
MULTICAST_GROUP = "239.255.94.70"
WS_PATH = "/vertexav"
SERVICE_TYPE = "_vertexav._tcp.local."

MAX_SAFE_INT = 2**53  # §7.1: JSON numbers must stay below this
MAX_HOPS = 4  # §5.5

# §15.7 connection limits
HELLO_TIMEOUT_S = 2.0
AUTH_RESPONSE_TIMEOUT_S = 3.0
WELCOME_TIMEOUT_S = 5.0
MAX_UNAUTH_PER_IP = 4
MAX_UNAUTH_TOTAL = 32
MAX_MSG_PREAUTH = 4 * 1024
MAX_MSG_RECEIVER_TO_EMITTER = 64 * 1024
MAX_MSG_EMITTER_TO_RECEIVER = 512 * 1024
MAX_JSON_DEPTH = 32
MAX_RECEIVERS_DEFAULT = 64

HEARTBEAT_INTERVAL_S = 1.0  # §6.5
HEARTBEAT_TIMEOUT_S = 3.0

# §8.3 binary format
MAGIC = b"VXAV"
BIN_VERSION = 0
HEADER = struct.Struct("<4sBBHIIq")  # magic, version, kind, flags, seq, session, t
assert HEADER.size == 24
FLAG_SIGNED = 0x0001
SIG_LEN = security.SIG_LEN
UDP_RECV_BUFFER = 1500  # §8.3 step 1
UDP_MAX_PACKET = 1200

KIND_TEMPO = 0x01
KIND_CLOCK_PING = 0x02
KIND_CLOCK_PONG = 0x03
KIND_FEATURES = 0x10
KIND_AUDIO = 0x20
KIND_NAMES = {
    KIND_TEMPO: "tempo",
    KIND_CLOCK_PING: "clock.ping",
    KIND_CLOCK_PONG: "clock.pong",
    KIND_FEATURES: "features",
    KIND_AUDIO: "audio",
}

TEMPO_BODY = struct.Struct("<dqdqqBBBB")  # §9.2
PING_BODY = struct.Struct("<qI")  # §7.2
PONG_BODY = struct.Struct("<qIqq")
FEATURES_BODY = struct.Struct("<BBHfff")  # §12.3, followed by band_count float32
AUDIO_BODY = struct.Struct("<BBHIQ")  # §12.5, followed by samples
assert HEADER.size + TEMPO_BODY.size == 68
assert HEADER.size + PING_BODY.size == 36
assert HEADER.size + PONG_BODY.size == 52
assert HEADER.size + FEATURES_BODY.size == 40
assert HEADER.size + AUDIO_BODY.size == 40

STABILITY = ("stable", "unstable", "freewheel")


@dataclass
class Header:
    magic: bytes
    version: int
    kind: int
    flags: int
    seq: int
    session: int
    t: int

    @property
    def signed(self) -> bool:
        return bool(self.flags & FLAG_SIGNED)


@dataclass
class TempoPacket:
    bpm: float
    anchor_t: int
    anchor_beat: float
    next_beat_t: int
    next_downbeat_t: int
    beats_per_bar: int
    source_deck: int
    playing: bool
    phase_reset: bool
    stability: str
    link_aligned: bool
    confidence: float  # 0..1

    def pack_body(self) -> bytes:
        flags = (
            (1 if self.playing else 0)
            | (2 if self.phase_reset else 0)
            | (STABILITY.index(self.stability) << 2)
            | (16 if self.link_aligned else 0)
        )
        conf = max(0, min(255, round(self.confidence * 255)))
        return TEMPO_BODY.pack(
            self.bpm, self.anchor_t, self.anchor_beat, self.next_beat_t,
            self.next_downbeat_t, self.beats_per_bar & 0xFF, self.source_deck & 0xFF, flags, conf,
        )

    @classmethod
    def unpack_body(cls, body: bytes) -> "TempoPacket":
        bpm, at, ab, nb, nd, bpb, deck, flags, conf = TEMPO_BODY.unpack(body)
        stab = (flags >> 2) & 3
        if stab > 2:
            raise ValueError("stability inválida (bits 2-3 = 3)")
        return cls(bpm, at, ab, nb, nd, bpb, deck, bool(flags & 1), bool(flags & 2),
                   STABILITY[stab], bool(flags & 16), conf / 255.0)


@dataclass
class PingPacket:
    t0: int
    ping_id: int


@dataclass
class PongPacket:
    t0: int
    ping_id: int
    t1: int
    t2: int


@dataclass
class FeaturesPacket:
    source_id: int
    onsets: int
    rms: float
    peak: float
    onset_strength: float
    bands: list


def expected_length(kind: int, body: bytes) -> Optional[int]:
    """Exact unsigned length for a packet kind (§8.3 step 5); None for unknown kinds."""
    if kind == KIND_TEMPO:
        return 68
    if kind == KIND_CLOCK_PING:
        return 36
    if kind == KIND_CLOCK_PONG:
        return 52
    if kind == KIND_FEATURES:
        if len(body) < 2:
            return -1
        return 40 + 4 * body[1]
    if kind == KIND_AUDIO:
        if len(body) < 16:
            return -1
        channels = body[1]
        frames = struct.unpack_from("<H", body, 2)[0]
        # Format is negotiated in audio.config; accept either sample size here.
        return 40 + frames * channels * 2  # s16le default; f32le checked by caller
    return None


def pack_packet(kind: int, seq: int, session: int, t: int, body: bytes,
                mac_key: Optional[bytes] = None) -> bytes:
    flags = FLAG_SIGNED if mac_key else 0
    data = HEADER.pack(MAGIC, BIN_VERSION, kind, flags, seq & 0xFFFFFFFF, session & 0xFFFFFFFF, t) + body
    if mac_key:
        data += security.sign(mac_key, data)
    return data


def parse_header(data: bytes) -> Header:
    return Header(*HEADER.unpack_from(data, 0))


def serial_diff(a: int, b: int) -> int:
    """Signed distance a - b with RFC 1982 serial arithmetic on 32 bits."""
    d = (a - b) & 0xFFFFFFFF
    return d - 0x100000000 if d >= 0x80000000 else d


class SeqWindow:
    """Acceptance windows for `seq` (§8.3)."""

    AHEAD = 256
    BEHIND = 64
    RESYNC_RUN = 8

    def __init__(self) -> None:
        self.last: Optional[int] = None
        self._run_last: Optional[int] = None
        self._run_len = 0
        self.dropped_dup = 0
        self.dropped_back = 0
        self.held_jump = 0
        self.resyncs = 0
        self.gaps = 0  # packets presumed lost (holes in accepted sequence)

    def reset(self) -> None:
        self.__init__()

    def accept(self, seq: int) -> bool:
        if self.last is None:
            self.last = seq
            return True
        d = serial_diff(seq, self.last)
        if 1 <= d <= self.AHEAD:
            self.gaps += d - 1
            self.last = seq
            self._run_last, self._run_len = None, 0
            return True
        if -self.BEHIND <= d <= 0:
            self.dropped_dup += 1
            return False
        if d < -self.BEHIND:
            self.dropped_back += 1  # never resync backwards within a session
            return False
        # Big jump forward: adopt only after RESYNC_RUN consecutive seq values.
        if self._run_last is not None and serial_diff(seq, self._run_last) == 1:
            self._run_len += 1
        else:
            self._run_len = 1
        self._run_last = seq
        if self._run_len >= self.RESYNC_RUN:
            self.last = seq
            self._run_last, self._run_len = None, 0
            self.resyncs += 1
            return True
        self.held_jump += 1
        return False


class PacketError(Exception):
    """Raised when a datagram fails validation; `step` is the §8.3 step number."""

    def __init__(self, step: int, msg: str):
        super().__init__(f"paso {step}: {msg}")
        self.step = step


def validate_datagram(data: bytes, session_key: int, mac_key: Optional[bytes],
                      require_signature: bool, truncated: bool = False,
                      audio_sample_bytes: int = 2) -> tuple[Header, bytes]:
    """Steps 1-5 of §8.3. Returns (header, body without signature). Raises PacketError."""
    min_len = HEADER.size + (SIG_LEN if require_signature else 0)
    if truncated:
        raise PacketError(1, "datagrama truncado")
    if len(data) < min_len:
        raise PacketError(1, f"longitud {len(data)} < mínimo {min_len}")
    hdr = parse_header(data)
    if hdr.magic != MAGIC:
        raise PacketError(2, f"magic {hdr.magic!r} incorrecto")
    if hdr.version != BIN_VERSION:
        raise PacketError(2, f"versión binaria {hdr.version} no soportada")
    if hdr.session != session_key:
        raise PacketError(3, f"session {hdr.session} != session_key {session_key}")
    payload = data
    if require_signature:
        if not hdr.signed:
            raise PacketError(4, "falta la firma (bit 0 de flags = 0)")
        if not mac_key or not security.verify(mac_key, data[:-SIG_LEN], data[-SIG_LEN:]):
            raise PacketError(4, "firma incorrecta")
        payload = data[:-SIG_LEN]
    elif hdr.signed:
        # Open session but packet claims a signature: strip it, it cannot be checked.
        payload = data[:-SIG_LEN] if len(data) >= HEADER.size + SIG_LEN else data
    if hdr.flags & ~FLAG_SIGNED:
        raise PacketError(5, f"bits reservados de flags activos: {hdr.flags:#06x}")
    body = payload[HEADER.size:]
    exp = expected_length(hdr.kind, body)
    if exp is None:
        raise PacketError(5, f"kind desconocido {hdr.kind:#04x}")
    if hdr.kind == KIND_AUDIO and audio_sample_bytes == 4 and exp >= 0:
        exp = 40 + (exp - 40) * 2
    if len(payload) != exp:
        raise PacketError(5, f"longitud {len(payload)} != {exp} para {KIND_NAMES[hdr.kind]}")
    return hdr, body


def unpack_body(kind: int, body: bytes):
    if kind == KIND_TEMPO:
        return TempoPacket.unpack_body(body)
    if kind == KIND_CLOCK_PING:
        return PingPacket(*PING_BODY.unpack(body))
    if kind == KIND_CLOCK_PONG:
        return PongPacket(*PONG_BODY.unpack(body))
    if kind == KIND_FEATURES:
        sid, n, onsets, rms, peak, ostr = FEATURES_BODY.unpack_from(body)
        bands = list(struct.unpack_from(f"<{n}f", body, FEATURES_BODY.size))
        return FeaturesPacket(sid, onsets, rms, peak, ostr, bands)
    return body


def json_depth_exceeds(text: str, limit: int = MAX_JSON_DEPTH) -> bool:
    """Cheap pre-parse scan of bracket nesting (§15.7), ignoring brackets inside strings."""
    depth = 0
    in_str = False
    esc = False
    for ch in text:
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch in "[{":
            depth += 1
            if depth > limit:
                return True
        elif ch in "]}":
            depth -= 1
    return False
