"""TABL: named rows of little-endian int16 values, read in place by asset_table.c."""

import struct
from pathlib import Path

from build.c_header import read_define

TABLE_READER = Path(__file__).resolve().parents[2] / "main/asset/asset_table_format.h"

TYPE = b"TABL"
VERSION = read_define(TABLE_READER, "ASSET_TABLE_VERSION")
NAME_SIZE = read_define(TABLE_READER, "ASSET_TABLE_NAME_SIZE")
HEADER_SIZE = read_define(TABLE_READER, "ASSET_TABLE_HEADER_SIZE")
HEADER = struct.Struct(f"<HHHBB{HEADER_SIZE - struct.calcsize('<HHHBB')}x")
AT_VERSION, AT_ROWS, AT_COLUMNS, AT_RESERVED = 0, 2, 4, 7
VALUE = struct.Struct("<h")
COUNT_MAX = 0xFFFF
FRAC_MAX = 0xFF
VALUE_MIN = -(1 << 15)
VALUE_MAX = (1 << 15) - 1


def encode(rows: dict[str, list[int]], frac_bits: int) -> bytes:
    """Encode a nonempty rectangular table, preserving row order."""
    columns = len(next(iter(rows.values()))) if rows else 0
    if not 1 <= len(rows) <= COUNT_MAX or not 1 <= columns <= COUNT_MAX \
            or type(frac_bits) is not int or not 0 <= frac_bits <= FRAC_MAX:
        raise ValueError("invalid table dimensions or fractional bits")
    out = bytearray(HEADER.pack(VERSION, len(rows), columns, frac_bits, 0))
    for name, values in rows.items():
        name = name.encode("utf-8")
        if len(name) >= NAME_SIZE or b"\0" in name:
            raise ValueError("row name must fit with its NUL terminator")
        if len(values) != columns or any(type(value) is not int or not VALUE_MIN <= value <= VALUE_MAX
                                        for value in values):
            raise ValueError("ragged rows or values outside int16")
        out += name.ljust(NAME_SIZE, b"\0")
        out += struct.pack(f"<{columns}h", *values)
    return bytes(out)


def decode(entry):
    """Return (named rows, fractional bits), checking the firmware's layout."""
    if len(entry) < HEADER.size:
        raise ValueError("short table header")
    version, rows, columns, frac_bits, reserved = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ValueError("unsupported table version")
    stride = NAME_SIZE + VALUE.size * columns
    if not rows or not columns or reserved or len(entry) != HEADER.size + rows * stride:
        raise ValueError("invalid table dimensions, reserved byte or size")
    result = {}
    for index in range(rows):
        offset = HEADER.size + index * stride
        name = bytes(entry[offset:offset + NAME_SIZE])
        if b"\0" not in name:
            raise ValueError("unterminated row name")
        result[name.split(b"\0", 1)[0].decode("utf-8", errors="surrogateescape")] = list(
            struct.unpack_from(f"<{columns}h", entry, offset + NAME_SIZE))
    return result, frac_bits
