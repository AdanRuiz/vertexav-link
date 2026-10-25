"""Unit tests for the wire format, security, tempo, throttling, mix logic and validator."""
from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from vertexav import proto, security
from vertexav.emitter import ObjectThrottle
from vertexav.mixlogic import MixAnalyzer
from vertexav.proto import PacketError, SeqWindow, TempoPacket
from vertexav.schema import CONTINUOUS, validate_message
from vertexav.tempo import Tempo, TempoFollower

SPEC = Path(__file__).resolve().parents[2] / "spec" / "vertexav-link-spec.md"


# ---------------------------------------------------------------- binary packets
def _tempo_pkt() -> TempoPacket:
    return TempoPacket(126.0, 50_100_000, 1024.0, 50_400_000, 51_000_000, 4, 1, True, False,
                       "stable", False, 1.0)


def test_header_and_lengths():
    body = _tempo_pkt().pack_body()
    data = proto.pack_packet(proto.KIND_TEMPO, 7, 1234, 50_100_000, body)
    assert len(data) == 68
    hdr, b = proto.validate_datagram(data, 1234, None, require_signature=False)
    assert hdr.kind == proto.KIND_TEMPO and hdr.seq == 7 and hdr.t == 50_100_000
    pkt = proto.unpack_body(hdr.kind, b)
    assert pkt == _tempo_pkt()


def test_signed_packet_roundtrip_and_tamper():
    key = b"k" * 32
    data = proto.pack_packet(proto.KIND_TEMPO, 1, 99, 0, _tempo_pkt().pack_body(), key)
    assert len(data) == 76
    proto.validate_datagram(data, 99, key, require_signature=True)
    bad = bytearray(data)
    bad[30] ^= 1
    with pytest.raises(PacketError) as e:
        proto.validate_datagram(bytes(bad), 99, key, require_signature=True)
    assert e.value.step == 4
    unsigned = proto.pack_packet(proto.KIND_TEMPO, 1, 99, 0, _tempo_pkt().pack_body())
    with pytest.raises(PacketError) as e:
        proto.validate_datagram(unsigned + b"\0" * 8, 99, key, require_signature=True)
    assert e.value.step == 4


@pytest.mark.parametrize("mutate,step", [
    (lambda d: d[:20], 1),
    (lambda d: b"XXXX" + d[4:], 2),
    (lambda d: d[:4] + b"\x01" + d[5:], 2),
    (lambda d: d[:12] + (5).to_bytes(4, "little") + d[16:], 3),
    (lambda d: d + b"\0", 5),
    (lambda d: d[:6] + b"\x02\x00" + d[8:], 5),
])
def test_validation_steps(mutate, step):
    data = proto.pack_packet(proto.KIND_TEMPO, 1, 77, 0, _tempo_pkt().pack_body())
    with pytest.raises(PacketError) as e:
        proto.validate_datagram(mutate(data), 77, None, require_signature=False)
    assert e.value.step == step


def test_ping_pong_lengths():
    ping = proto.pack_packet(proto.KIND_CLOCK_PING, 1, 5, 0, proto.PING_BODY.pack(123, 9))
    pong = proto.pack_packet(proto.KIND_CLOCK_PONG, 1, 5, 0, proto.PONG_BODY.pack(123, 9, 1, 2))
    assert len(ping) == 36 and len(pong) == 52


# ---------------------------------------------------------------- sequence windows
def test_seq_window_table():
    w = SeqWindow()
    assert w.accept(100)
    assert w.accept(101)
    assert not w.accept(101)  # duplicate
    assert not w.accept(60)  # 41 behind: out of order
    assert w.accept(357)  # 256 ahead is fine
    assert not w.accept(357 - 65)  # more than 64 behind: always dropped
    # big jump ahead: needs 8 consecutive
    for s in range(10_000, 10_007):
        assert not w.accept(s)
    assert w.accept(10_007)
    assert w.last == 10_007 and w.resyncs == 1


def test_seq_window_wraps():
    w = SeqWindow()
    assert w.accept(0xFFFFFFFE)
    assert w.accept(0xFFFFFFFF)
    assert w.accept(0)
    assert w.accept(1)


def test_seq_window_isolated_forged_packet_does_not_block():
    w = SeqWindow()
    w.accept(10)
    assert not w.accept(2_000_000)  # forged huge seq
    assert w.accept(11)  # real flow continues


# ---------------------------------------------------------------- security
def test_hkdf_rfc5869_case1():
    ikm = bytes([0x0B] * 22)
    salt = bytes(range(0x0D))
    info = bytes(range(0xF0, 0xFA))
    okm = security.hkdf_sha256(ikm, salt, info, 42)
    assert okm.hex() == ("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                         "34007208d5b887185865")


def test_pairing_code():
    psk = security.generate_psk()
    code = security.psk_to_code(psk)
    assert len(code) == 52
    assert security.code_to_psk(code.lower()) == psk


def test_proofs_differ_by_direction():
    psk = b"p" * 32
    a = security.client_proof(psk, b"s" * 16, b"c" * 16, "id")
    b = security.server_proof(psk, b"c" * 16, b"s" * 16, "id")
    assert a != b and len(a) == 32


# ---------------------------------------------------------------- tempo
def test_tempo_next_beats():
    tp = Tempo(bpm=120, anchor_t=0, anchor_beat=0.0, beats_per_bar=4, bar_offset=0)
    assert tp.next_beat_t(0) == 500_000
    assert tp.next_beat_t(250_000) == 500_000
    assert tp.next_downbeat_t(100_000) == 2_000_000
    tp2 = Tempo(bpm=120, anchor_t=0, anchor_beat=1.0, beats_per_bar=4, bar_offset=1)
    assert tp2.next_downbeat_t(0) == 2_000_000


def test_follower_smooth_and_jump():
    f = TempoFollower()
    base = Tempo(bpm=120, anchor_t=0, anchor_beat=0.0, playing=True)
    assert f.update(base, 0) == "init"
    # 0.1 beat ahead: corrected gradually (2 % per beat)
    assert f.update(Tempo(bpm=120, anchor_t=1_000_000, anchor_beat=2.1, playing=True), 1_000_000) == "smooth"
    assert abs(f.beat_at(1_000_000) - 2.0) < 1e-9
    assert abs(f.beat_at(1_500_000) - (3.1 - 0.08)) < 1e-6  # one beat later: 0.02 corrected
    assert abs(f.beat_at(4_000_000) - 8.1) < 1e-6  # fully corrected after 5 beats
    # more than 1/4 beat: jump
    assert f.update(Tempo(bpm=120, anchor_t=5_000_000, anchor_beat=10.5, playing=True), 5_000_000) == "jump"
    assert abs(f.beat_at(5_000_000) - 10.5) < 1e-9


def test_follower_unstable_freezes_then_stops_beats():
    f = TempoFollower()
    f.update(Tempo(bpm=120, anchor_t=0, playing=True), 0)
    assert f.update(Tempo(bpm=90, anchor_t=100_000, anchor_beat=7.0, playing=True, stability="unstable"), 100_000) == "frozen"
    assert abs(f.beat_at(1_000_000) - 2.0) < 1e-9  # still the old timeline
    assert f.beats_enabled(4_000_000)  # 8 beats
    assert not f.beats_enabled(4_600_000)  # more than 8 beats unstable


# ---------------------------------------------------------------- throttling (§8.4)
def test_throttle_rules():
    th = ObjectThrottle(CONTINUOUS["deck.state"])
    assert th.offer({"audible": 0.5}, 0.0) == {"audible": 0.5}  # first value: immediate
    assert th.offer({"audible": 0.6}, 0.01) is None  # batched (20 Hz)
    assert th.offer({"audible": 0.605}, 0.06) == {"audible": 0.605}  # 50 ms later
    assert th.offer({"audible": 0.61}, 0.12) is None  # dead zone (< 0.01)
    assert th.poll(0.18) == {"audible": 0.61}  # final value after 50 ms quiet
    assert th.offer({"audible": 0.0}, 0.181) == {"audible": 0.0}  # edge: immediate
    assert th.offer({"audible": 0.001}, 0.182) == {"audible": 0.001}  # leaves 0: immediate
    assert th.offer({"playing": True, "audible": 0.002}, 0.183) == {"playing": True, "audible": 0.002}


def test_throttle_signed_edges():
    th = ObjectThrottle(CONTINUOUS["mix.state"])
    th.offer({"crossfader": 0.5}, 0.0)
    assert th.offer({"crossfader": -1.0}, 0.001) == {"crossfader": -1.0}


# ---------------------------------------------------------------- mix logic (§10.2)
def test_mix_blend():
    m = MixAnalyzer()
    beat = 500_000
    m.update({1: 1.0, 2: 0.0}, 0, beat)
    assert m.on_air == 1
    r = m.update({1: 1.0, 2: 0.2}, 10_000, beat)
    assert ("transition.start", 2, {"from_deck": 1, "to_deck": 2}) in r.events
    r = m.update({1: 0.4, 2: 0.9}, 2_000_000, beat)
    assert r.on_air_change == (1, 2)
    assert not any(e[0] == "mix.cut" for e in r.events)
    m.update({1: 0.0, 2: 1.0}, 3_000_000, beat)
    r = m.update({1: 0.0, 2: 1.0}, 3_600_000, beat)
    assert any(e[0] == "transition.end" for e in r.events)


def test_mix_cut_and_gate():
    m = MixAnalyzer()
    beat = 500_000
    m.update({1: 1.0, 2: 0.0}, 0, beat)
    r = m.update({1: 0.0, 2: 1.0}, 5_000, beat)
    names = [e[0] for e in r.events]
    assert "mix.cut" in names and "on_air.change" in names
    assert ("deck.gate", 1, {"state": "closed"}) in r.events
    assert m.transition["kind"] == "cut"
    r = m.update({1: 1.0, 2: 0.0}, 10_000, beat)
    assert ("deck.gate", 1, {"state": "open"}) in r.events


# ---------------------------------------------------------------- spec examples
def _spec_json_blocks():
    text = SPEC.read_text(encoding="utf-8")
    return re.findall(r"```json\n(.*?)```", text, re.S)


def test_all_spec_examples_are_valid_json():
    blocks = _spec_json_blocks()
    assert len(blocks) > 20
    for b in blocks:
        json.loads(b)


@pytest.mark.parametrize("block", _spec_json_blocks())
def test_spec_examples_validate(block):
    msg = json.loads(block)
    direction = "r2e" if msg.get("type") in {"hello", "subscribe", "audio.subscribe", "receiver.state",
                                             "receiver.capabilities", "latency.update",
                                             "auth.response"} else "e2r"
    errors = [i for i in validate_message(msg, direction) if i.level == "error"]
    assert not errors, f"{msg.get('type')}: {errors}"


def test_validator_catches_problems():
    bad = {"type": "event", "t": 1, "at": 2, "event_id": "x", "name": "countdown",
           "data": {"target": "drop", "beats_remaining": 0, "target_at": 1}}
    errs = [i.msg for i in validate_message(bad) if i.level == "error"]
    assert any("beats_remaining" in e for e in errs)
    assert any("target_at" in e for e in errs)
    big = {"type": "tempo", "t": 2**53, "bpm": 120, "anchor_t": 0, "anchor_beat": 0,
           "beats_per_bar": 4, "playing": True, "stability": "stable"}
    assert any("2^53" in i.msg for i in validate_message(big))
    assert proto.json_depth_exceeds("[" * 33 + "]" * 33)
    assert not proto.json_depth_exceeds('{"a":"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[["}')


def test_mix_chop_back_is_a_cut_and_ends():
    m = MixAnalyzer()
    beat = 500_000
    m.update({1: 1.0, 2: 0.0}, 0, beat)
    m.update({1: 0.0, 2: 1.0}, 10_000, beat)  # cut 1 -> 2
    r = m.update({1: 1.0, 2: 0.0}, 400_000, beat)  # chop back after > 1/2 beat
    assert any(e[0] == "mix.cut" for e in r.events)
    m.update({1: 1.0, 2: 0.0}, 500_000, beat)
    r = m.update({1: 1.0, 2: 0.0}, 1_000_000, beat)
    assert any(e[0] == "transition.end" for e in r.events)
    assert not m.transition["active"]
