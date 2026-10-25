"""Small shared helpers: persistent ids, local addresses, JSON encoding, terminal colours."""
from __future__ import annotations

import json
import os
import random
import secrets
import socket
import sys
from pathlib import Path


def data_dir() -> Path:
    base = os.environ.get("VERTEXAV_HOME")
    path = Path(base) if base else Path.home() / ".vertexav"
    path.mkdir(parents=True, exist_ok=True)
    return path


def load_or_create_id(kind: str, prefix: str = "vx-") -> str:
    """Stable identifier generated once per installation (§5.5)."""
    f = data_dir() / f"{kind}-id"
    if f.exists():
        value = f.read_text(encoding="utf-8").strip()
        if value:
            return value
    value = prefix + secrets.token_hex(4)
    f.write_text(value, encoding="utf-8")
    return value


def dumps(msg: dict) -> str:
    return json.dumps(msg, ensure_ascii=False, separators=(",", ":"))


def jittered(base: float) -> float:
    """Value ± 25 % random, used for reconnection back-off (§5.5, §6.5)."""
    return base * random.uniform(0.75, 1.25)


def local_ipv4_addresses() -> list[str]:
    try:
        import ifaddr

        out = []
        for adapter in ifaddr.get_adapters():
            for ip in adapter.ips:
                if isinstance(ip.ip, str) and not ip.ip.startswith(("127.", "169.254.")):
                    out.append(ip.ip)
        if out:
            return sorted(set(out))
    except Exception:
        pass
    try:
        return [socket.gethostbyname(socket.gethostname())]
    except OSError:
        return []


class C:
    """ANSI colours (enabled on Windows 10+ terminals by os.system(''))."""

    on = sys.stdout.isatty()
    RESET = "\x1b[0m"
    DIM = "\x1b[2m"
    RED = "\x1b[31m"
    GREEN = "\x1b[32m"
    YELLOW = "\x1b[33m"
    BLUE = "\x1b[34m"
    MAGENTA = "\x1b[35m"
    CYAN = "\x1b[36m"
    BOLD = "\x1b[1m"

    @classmethod
    def c(cls, color: str, text: str) -> str:
        return f"{color}{text}{cls.RESET}" if cls.on else text


def enable_ansi() -> None:
    if sys.platform == "win32":
        os.system("")
    try:
        sys.stdout.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
    except Exception:
        pass
