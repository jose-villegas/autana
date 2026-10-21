"""Stable name keys shared with the firmware's core/name_hash.h."""

from ctypes import c_uint32
from pathlib import Path

from build.c_header import read_define

OWNER = Path(__file__).resolve().parents[2] / "main/core/name_hash.h"
OFFSET_BASIS = read_define(OWNER, "NAME_HASH_OFFSET_BASIS")
PRIME = read_define(OWNER, "NAME_HASH_PRIME")
NONE = read_define(OWNER, "NAME_HASH_NONE")


def name_hash_bytes(data: bytes) -> int:
    """FNV-1a 32 over a byte span, including any embedded NUL."""
    value = OFFSET_BASIS
    for byte in data:
        value = c_uint32((value ^ byte) * PRIME).value
    return value


def name_hash(text: str) -> int:
    """FNV-1a 32 over the UTF-8 bytes of a name."""
    return name_hash_bytes(text.encode("utf-8"))
