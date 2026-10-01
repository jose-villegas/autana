"""The asset pack: the container main/asset/asset_pack.h reads in place.

A pack is a header, an entry table and the entries' bytes, all little-endian:

    header   32 bytes  magic "APAK", version, entry count, total size, CRC-32
                       of every byte after the header, three reserved words
    table    48 bytes per entry: name (32, NUL padded), type, offset, size,
                       alignment; offsets count from the start of the pack
    data     each entry at an offset that is a multiple of its alignment

An entry never holds a pointer: it names its own parts by offset from its
own first byte, so the mapped pack is usable as it is. Standard library only.
"""

import struct
import zlib

MAGIC = b"APAK"
VERSION = 1
HEADER = struct.Struct("<4sIIII12x")
ENTRY = struct.Struct("<32sIIII")
NAME_BYTES = 32
DEFAULT_ALIGN = 16


class PackError(ValueError):
    """A pack that is not the format, or entries that cannot be packed."""


def padded(offset, align):
    return -(-offset // align) * align


def build_pack(entries):
    """The pack's bytes for `entries`, (name, type, data[, align]) in order.
    Names are unique ASCII of at most 31 characters; aligns are powers of two."""
    seen = set()
    for entry in entries:
        name = entry[0]
        if not name or len(name.encode("ascii")) >= NAME_BYTES or name in seen:
            raise PackError(f"entry name {name!r} is empty, too long or repeated")
        seen.add(name)
    table_end = HEADER.size + ENTRY.size * len(entries)
    table, data, offset = [], bytearray(), table_end
    for entry in entries:
        name, kind, blob = entry[:3]
        align = entry[3] if len(entry) > 3 else DEFAULT_ALIGN
        if align & (align - 1) or align == 0:
            raise PackError(f"entry {name!r}: alignment {align} is not a power of two")
        start = padded(offset, align)
        data += bytes(start - offset) + blob
        table.append(ENTRY.pack(name.encode("ascii"), kind, start, len(blob), align))
        offset = start + len(blob)
    body = b"".join(table) + bytes(data)
    header = HEADER.pack(MAGIC, VERSION, len(entries), HEADER.size + len(body), zlib.crc32(body))
    return header + body


def parse_pack(pack):
    """{name: (type, data)} of `pack` after the same checks the firmware makes:
    magic, version, size, CRC-32, and every entry inside the pack and aligned."""
    if len(pack) < HEADER.size:
        raise PackError("shorter than a header")
    magic, version, count, total, crc = HEADER.unpack_from(pack)
    if magic != MAGIC:
        raise PackError("bad magic")
    if version != VERSION:
        raise PackError(f"format version {version}, this reads {VERSION}")
    if total != len(pack) or HEADER.size + ENTRY.size * count > total:
        raise PackError("the size in the header is not the pack's size")
    if zlib.crc32(pack[HEADER.size :]) != crc:
        raise PackError("CRC-32 mismatch")
    entries = {}
    for index in range(count):
        raw, kind, offset, size, align = ENTRY.unpack_from(pack, HEADER.size + ENTRY.size * index)
        name = raw.rstrip(b"\0").decode("ascii")
        if offset + size > total or align == 0 or offset % align:
            raise PackError(f"entry {name!r} is outside the pack or misaligned")
        entries[name] = (kind, pack[offset : offset + size])
    return entries
