"""The pack directory main/asset/asset_directory.c reads, little-endian.

Header: magic, CRC-32 of version/count/rows, version, pack count (16 bytes).
Rows: NUL-padded name (32 bytes), partition offset, pack size (40 bytes).
Version 2 starts each pack on an MMU page, after the rows, in row order.
Separate pages keep pack mapping lifetimes independent; padding costs less
than a page per pack. The reader header owns the page size and version.
"""

import struct
import zlib
from pathlib import Path

from asset.asset_pack import PackError, NAME_BYTES, padded
from device.panel_size import read_define

DIRECTORY_READER = Path(__file__).resolve().parents[2] / "main/asset/asset_directory.h"
_header = DIRECTORY_READER.read_text(encoding="utf-8")

DIRECTORY_MAGIC = b"ABDR"
DIRECTORY_VERSION = read_define(_header, "ASSET_DIRECTORY_VERSION")
DIRECTORY_HEADER = struct.Struct("<4sIII")
DIRECTORY_ROW = struct.Struct(f"<{NAME_BYTES}sII")
DIRECTORY_CRC_START = struct.calcsize("<4sI")
MMU_PAGE_SIZE = read_define(_header, "ASSET_MMU_PAGE_SIZE")


def build_directory(packs):
    """The partition image for `packs`, (name, pack bytes) in order: the
    directory, then each pack on its own MMU page."""
    rows_end = DIRECTORY_HEADER.size + len(packs) * DIRECTORY_ROW.size
    image = bytearray(padded(rows_end, MMU_PAGE_SIZE))
    names = set()
    for index, (name, pack) in enumerate(packs):
        encoded = name.encode("ascii")
        if not encoded or len(encoded) >= NAME_BYTES or name in names:
            raise PackError(f"pack name {name!r} is empty, too long or repeated")
        names.add(name)
        offset = padded(len(image), MMU_PAGE_SIZE)
        image.extend(bytes(offset - len(image)))
        DIRECTORY_ROW.pack_into(image, DIRECTORY_HEADER.size + index * DIRECTORY_ROW.size,
                                encoded, offset, len(pack))
        image.extend(pack)
    DIRECTORY_HEADER.pack_into(image, 0, DIRECTORY_MAGIC, 0, DIRECTORY_VERSION, len(packs))
    checksum = zlib.crc32(image[DIRECTORY_CRC_START:rows_end])
    DIRECTORY_HEADER.pack_into(image, 0, DIRECTORY_MAGIC, checksum, DIRECTORY_VERSION, len(packs))
    return bytes(image)


def parse_directory(image):
    """{name: pack bytes} after validating the header, checksum and rows.
    Packs start on MMU pages, inside the image, after the directory and
    preceding pack. Names are unique; packs themselves are not opened."""
    if len(image) < DIRECTORY_HEADER.size:
        raise PackError("shorter than a directory header")
    magic, checksum, version, count = DIRECTORY_HEADER.unpack_from(image)
    if version != DIRECTORY_VERSION:
        raise PackError(f"directory version {version}, this reads {DIRECTORY_VERSION}")
    if magic != DIRECTORY_MAGIC:
        raise PackError("bad directory magic")
    end = DIRECTORY_HEADER.size + count * DIRECTORY_ROW.size
    rows = image[DIRECTORY_HEADER.size:end]
    if len(rows) != count * DIRECTORY_ROW.size:
        raise PackError("the directory's rows leave the image")
    actual_checksum = zlib.crc32(image[DIRECTORY_CRC_START:end])
    if checksum != actual_checksum:
        raise PackError("directory CRC-32 mismatch")
    packs = {}
    for raw, offset, size in DIRECTORY_ROW.iter_unpack(rows):
        name = raw.rstrip(b"\0").decode("ascii")
        if not name or raw[-1] or name in packs:
            raise PackError(f"pack name {name!r} is empty, fills its {NAME_BYTES} bytes or is repeated")
        if offset % MMU_PAGE_SIZE != 0:
            raise PackError(f"pack {name!r} does not start on a MMU page")
        if offset < end or size > len(image) - offset:
            raise PackError(f"pack {name!r} is outside the image or overlaps another")
        end = offset + size
        packs[name] = image[offset:end]
    return packs
