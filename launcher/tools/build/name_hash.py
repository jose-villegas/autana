"""Stable name keys shared with the name_hash package."""

from ctypes import c_uint32, c_uint64
from pathlib import Path

from build.c_header import read_define

OWNER = Path(__file__).resolve().parents[2] / "packages/name_hash/include/name_hash/name_hash.h"
OFFSET_BASIS = read_define(OWNER, "NAME_HASH_OFFSET_BASIS")
PRIME = read_define(OWNER, "NAME_HASH_PRIME")
OFFSET_BASIS64 = read_define(OWNER, "NAME_HASH64_OFFSET_BASIS")
PRIME64 = read_define(OWNER, "NAME_HASH64_PRIME")
NONE = read_define(OWNER, "NAME_HASH_NONE")


def name_hash_n(data: bytes) -> int:
    """FNV-1a 32 over a byte span, including any embedded NUL."""
    value = OFFSET_BASIS
    for byte in data:
        value = c_uint32((value ^ byte) * PRIME).value
    return value


def name_hash(text: str) -> int:
    """FNV-1a 32 over the UTF-8 bytes of a name."""
    return name_hash_n(text.encode("utf-8"))


def name_hash64_n(seed: int, data: bytes) -> int:
    """FNV-1a 64 over a byte span, continuing from seed."""
    for byte in data:
        seed = c_uint64((seed ^ byte) * PRIME64).value
    return seed
