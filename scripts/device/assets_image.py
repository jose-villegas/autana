#!/usr/bin/env python3
"""Make a flashable image of the asset pack: the build half of `autana flash assets`.

    python scripts/device/assets_image.py --project PATH --out DIR

Reads PATH/launcher/assets/assets.bin and the offset of the `assets` partition
in PATH/launcher/partitions.csv, refuses a pack the firmware would refuse
(magic, version, size, CRC-32), and leaves in DIR what device.py snapshots and
flash_image.sh writes: assets.bin, a flash_args naming it at that offset, and
build_id.txt. Standard library only; it touches no board.
"""

import argparse
import csv
import struct
import sys
import zlib
from pathlib import Path

LABEL = "assets"
MAGIC = b"APAK"
VERSION = 1
HEADER = struct.Struct("<4sIIII12x")
ALIGN = 0x10000


class AssetsError(RuntimeError):
    """The pack or the partition table cannot be flashed as they are."""


def partition_offset(table, label=LABEL):
    """(offset, size) of the partition `label` in a partitions.csv, which must
    state both as numbers: a blank offset is for the build to place, and the
    flash then has nowhere certain to write."""
    for row in csv.reader(line for line in Path(table).read_text(encoding="utf-8").splitlines()
                          if line.strip() and not line.lstrip().startswith("#")):
        cells = [cell.strip() for cell in row]
        if cells[0] != label:
            continue
        if len(cells) < 5 or not cells[3] or not cells[4]:
            raise AssetsError(f"{table}: partition {label!r} needs an explicit offset and size")
        offset, size = int(cells[3], 0), int(cells[4], 0)
        if offset % ALIGN:
            raise AssetsError(f"{table}: partition {label!r} is not 64 KB aligned")
        return offset, size
    raise AssetsError(f"{table}: no partition named {label!r}")


def checked_pack(pack, partition_size):
    """The pack's CRC-32 once its header, size and checksum are right."""
    if len(pack) < HEADER.size:
        raise AssetsError("the pack is shorter than a header")
    magic, version, _, total, crc = HEADER.unpack_from(pack)
    if magic != MAGIC or version != VERSION:
        raise AssetsError(f"not an asset pack this tool knows (magic {magic!r}, version {version})")
    if total != len(pack) or zlib.crc32(pack[HEADER.size:]) != crc:
        raise AssetsError("the pack's size or checksum is wrong: run launcher/tools/r3d/build_pack.py")
    if len(pack) > partition_size:
        raise AssetsError(f"the pack is {len(pack)} bytes and the partition {partition_size}")
    return crc


def make_image(project, out):
    project, out = Path(project), Path(out)
    launcher = project / "launcher"
    offset, size = partition_offset(launcher / "partitions.csv")
    pack_path = launcher / "assets" / "assets.bin"
    if not pack_path.is_file():
        raise AssetsError(f"no pack at {pack_path}")
    pack = pack_path.read_bytes()
    crc = checked_pack(pack, size)
    out.mkdir(parents=True, exist_ok=True)
    (out / "assets.bin").write_bytes(pack)
    (out / "flash_args").write_text(f"--flash_size 16MB\n0x{offset:x} assets.bin\n", encoding="utf-8", newline="\n")
    (out / "build_id.txt").write_text(f"assets-{crc:08x}\n", encoding="ascii", newline="\n")
    print(f"asset pack: {len(pack)} bytes for 0x{offset:x} (partition {size} bytes), assets-{crc:08x}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    try:
        make_image(args.project, args.out)
    except AssetsError as error:
        print(f"assets_image: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
