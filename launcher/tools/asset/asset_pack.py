"""The asset pack: the container main/asset/asset_pack.h reads in place.

A pack is a header, an entry table and the entries' bytes, all little-endian:

    header   32 bytes  magic "APAK", version, entry count, total size, CRC-32
                       of every byte after the header, 12 reserved zero bytes
    table    48 bytes per entry: name (32, NUL padded), type (four
                       characters), offset, size, alignment; offsets count from
                       the start of the pack
    data     each entry at an offset that is a multiple of its alignment

An entry never holds a pointer: it names its own parts by offset from its
own first byte, so the mapped pack is usable as it is. What an entry's bytes
mean is the business of the module that owns its type.

Content ships as bundles, each a pack. A flash partition holds them under a
bundle directory, little-endian:

    header   16 bytes  magic "ABDR", CRC-32 of every byte after it to the end
                       of the rows, version, bundle count
    rows     40 bytes per bundle: name (32, NUL padded), offset, size;
                       offsets count from the start of the partition
    bundles  each on its own 4 KB sector, after the rows, in row order

Standard library only.
"""

import struct
import zlib

MAGIC = b"APAK"
VERSION = 1
HEADER = struct.Struct("<4sIIII12x")
ENTRY = struct.Struct("<32s4sIII")
NAME_BYTES = 32
DEFAULT_ALIGN = 16


DIRECTORY_MAGIC = b"ABDR"
DIRECTORY_VERSION = 1
DIRECTORY_HEADER = struct.Struct("<4sIII")
DIRECTORY_ROW = struct.Struct("<32sII")
SECTOR = 4096


class PackError(ValueError):
    """A pack that is not the format, or entries that cannot be packed."""


def padded(offset, align):
    return -(-offset // align) * align


def build_pack(entries):
    """The pack's bytes for `entries`, (name, type, data[, align]) in order.
    Names are unique ASCII of at most 31 characters, types are 4 bytes, aligns
    are powers of two."""
    seen = set()
    for entry in entries:
        name, kind = entry[0], entry[1]
        if not name or len(name.encode("ascii")) >= NAME_BYTES or name in seen:
            raise PackError(f"entry name {name!r} is empty, too long or repeated")
        if not isinstance(kind, bytes) or len(kind) != 4:
            raise PackError(f"entry {name!r}: a type is four bytes, not {kind!r}")
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
    magic, version, size, reserved bytes, CRC-32, and every entry inside the
    pack, after the table and aligned."""
    if len(pack) < HEADER.size:
        raise PackError("shorter than a header")
    magic, version, count, total, crc = HEADER.unpack_from(pack)
    if magic != MAGIC:
        raise PackError("bad magic")
    if version != VERSION:
        raise PackError(f"format version {version}, this reads {VERSION}")
    if total != len(pack) or HEADER.size + ENTRY.size * count > total:
        raise PackError("the size in the header is not the pack's size")
    if any(pack[20:HEADER.size]):
        raise PackError("reserved header bytes are in use")
    if zlib.crc32(pack[HEADER.size :]) != crc:
        raise PackError("CRC-32 mismatch")
    table_end = HEADER.size + ENTRY.size * count
    entries = {}
    for index in range(count):
        raw, kind, offset, size, align = ENTRY.unpack_from(pack, HEADER.size + ENTRY.size * index)
        name = raw.rstrip(b"\0").decode("ascii")
        if offset + size > total or align == 0 or offset % align or offset < table_end:
            raise PackError(f"entry {name!r} is outside the pack or misaligned")
        entries[name] = (kind, pack[offset : offset + size])
    return entries


def build_directory(bundles):
    """The partition image for `bundles`, (name, pack bytes) in order: the
    directory, then each bundle on its own sector."""
    names = [name for name, _ in bundles]
    for name in names:
        if not name or len(name.encode("ascii")) >= NAME_BYTES or names.count(name) > 1:
            raise PackError(f"bundle name {name!r} is empty, too long or repeated")
    first = padded(DIRECTORY_HEADER.size + DIRECTORY_ROW.size * len(bundles), SECTOR)
    rows, body = [], bytearray()
    for name, pack in bundles:
        start = padded(len(body), SECTOR)
        body += bytes(start - len(body)) + pack
        rows.append(DIRECTORY_ROW.pack(name.encode("ascii"), first + start, len(pack)))
    covered = struct.pack("<II", DIRECTORY_VERSION, len(bundles)) + b"".join(rows)
    directory = DIRECTORY_MAGIC + struct.pack("<I", zlib.crc32(covered)) + covered
    return directory + bytes(first - len(directory)) + bytes(body)


def parse_directory(image):
    """{name: pack bytes} of a partition image after the checks the firmware
    makes: magic, version, CRC-32, every row inside the image, on a sector,
    after the rows and the one before it, and no name twice. The packs
    themselves are not opened."""
    if len(image) < DIRECTORY_HEADER.size:
        raise PackError("shorter than a directory header")
    magic, crc, version, count = DIRECTORY_HEADER.unpack_from(image)
    if magic != DIRECTORY_MAGIC:
        raise PackError("bad directory magic")
    if version != DIRECTORY_VERSION:
        raise PackError(f"directory version {version}, this reads {DIRECTORY_VERSION}")
    rows_end = DIRECTORY_HEADER.size + DIRECTORY_ROW.size * count
    if rows_end > len(image):
        raise PackError("the directory's rows leave the image")
    if zlib.crc32(image[8:rows_end]) != crc:
        raise PackError("directory CRC-32 mismatch")
    bundles, end = {}, rows_end
    for index in range(count):
        raw, offset, size = DIRECTORY_ROW.unpack_from(image, DIRECTORY_HEADER.size + DIRECTORY_ROW.size * index)
        name = raw.rstrip(b"\0").decode("ascii")
        if offset % SECTOR:
            raise PackError(f"bundle {name!r} does not start on a sector")
        if offset < end or offset + size > len(image):
            raise PackError(f"bundle {name!r} is outside the image or overlaps another")
        if not name or raw[-1] or name in bundles:
            raise PackError(f"bundle name {name!r} is empty, fills its 32 bytes or is repeated")
        bundles[name] = image[offset : offset + size]
        end = offset + size
    return bundles
