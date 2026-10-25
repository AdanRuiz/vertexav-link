"""Security primitives for VertexAV Link (§15): pairing keys, challenge-response, UDP signatures."""
from __future__ import annotations

import base64
import hashlib
import hmac
import secrets

SIG_LEN = 8
NONCE_LEN = 16
PSK_LEN = 32


def generate_psk() -> bytes:
    return secrets.token_bytes(PSK_LEN)


def psk_to_code(psk: bytes) -> str:
    """Pairing code: base32 without padding, 52 characters for a 256-bit key (§15.3)."""
    return base64.b32encode(psk).decode("ascii").rstrip("=")


def code_to_psk(code: str) -> bytes:
    clean = code.strip().replace(" ", "").replace("-", "").upper()
    clean += "=" * (-len(clean) % 8)
    psk = base64.b32decode(clean)
    if len(psk) != PSK_LEN:
        raise ValueError("el código de emparejamiento debe contener 256 bits")
    return psk


def new_nonce() -> bytes:
    return secrets.token_bytes(NONCE_LEN)


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def unb64(text: str) -> bytes:
    return base64.b64decode(text, validate=True)


def client_proof(psk: bytes, server_nonce: bytes, client_nonce: bytes, client_id: str) -> bytes:
    msg = b"vertexav-auth-client" + server_nonce + client_nonce + client_id.encode("utf-8")
    return hmac.new(psk, msg, hashlib.sha256).digest()


def server_proof(psk: bytes, client_nonce: bytes, server_nonce: bytes, session_id: str) -> bytes:
    msg = b"vertexav-auth-server" + client_nonce + server_nonce + session_id.encode("utf-8")
    return hmac.new(psk, msg, hashlib.sha256).digest()


def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int = 32) -> bytes:
    """RFC 5869."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out = b""
    block = b""
    counter = 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


def mac_key(psk: bytes, server_nonce: bytes, client_nonce: bytes) -> bytes:
    return hkdf_sha256(psk, server_nonce + client_nonce, b"vertexav-udp-mac-v1", 32)


def sign(key: bytes, data: bytes) -> bytes:
    return hmac.new(key, data, hashlib.sha256).digest()[:SIG_LEN]


def verify(key: bytes, data: bytes, sig: bytes) -> bool:
    return hmac.compare_digest(sign(key, data), sig)


def equal(a: bytes, b: bytes) -> bool:
    return hmac.compare_digest(a, b)
