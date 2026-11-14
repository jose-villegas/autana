"""Frame record streams: the packets a board's FRAMEREC sends over the console,
and the records inside them. A pure library, imported by
scripts/device/device.py's own `record` subcommand (`autana record`) and by
the tests; run as a script it summarises a saved stream:

    python framerec.py summary FILE [--json]

The board writes each packet as one line and its bytes in one console write
(main/shell/console/console_framerec.c):

    FRAMEREC_BIN <bytes> <crc32 hex>\\n<bytes>

Log lines from other tasks land between packets, never inside one. The
bytes, joined in order, are the record stream, the same bytes a host render
writes with --record; its layout is main/render/record/frame_record_format.h,
which RECORD_TYPES and the struct formats below follow, a test holding them
to that header. Standard library only, as screenshot.py, for the same
reason: it runs under ESP-IDF's Python.
"""

import argparse
import json
import struct
import sys
import zlib
from collections import Counter
from dataclasses import dataclass, field

BIN_PREFIX = b"FRAMEREC_BIN "

# frame_record_type_t
RECORD_TYPES = {
    "HEADER": 1, "PACK": 2, "FRAME": 3, "VIEW": 4, "CHOICE": 5,
    "MESH": 6, "POSE": 7, "DYNMESH": 8, "TUNE": 9, "CHECK": 10,
}
RECORD_NAMES = {number: name for name, number in RECORD_TYPES.items()}
# frame_record_kind_t: where a MESH's mesh came from
MESH_KINDS = {"STATIC": 0, "BATCH": 1, "SKINNED": 2, "BUILT": 3}

# FRAME flags
KEYFRAME = 1
AFTER_DROP = 2
GENERATED = 4

HEAD = struct.Struct("<HI")
HEADER = struct.Struct("<HHHH32s")
PACK = struct.Struct("<BI32s")
FRAME = struct.Struct("<IIBHH")
CHOICE = struct.Struct("<hBBH")
MESH = struct.Struct("<HBB" + "BI" * 3 + "16f")
POSE = struct.Struct("<HH")
DYNMESH = struct.Struct("<HBBiII")
TUNE = struct.Struct("<32si")
CHECK = struct.Struct("<IHHI")
MATRIX = struct.Struct("<16f")
FIXED = {"HEADER": HEADER, "PACK": PACK, "FRAME": FRAME, "CHOICE": CHOICE, "MESH": MESH,
         "TUNE": TUNE, "CHECK": CHECK}


class StreamError(ValueError):
    """Bytes that are not a frame record stream, or a packet that arrived damaged."""


class Packets:
    """Splits what a board writes into its text lines and the stream bytes the
    FRAMEREC_BIN packets carry. feed() takes any chunk as it was read and
    returns ("line", bytes) and ("data", bytes) in arrival order; a line is
    returned once its newline has arrived, a packet once all its bytes have.
    A packet whose bytes do not match its crc32 raises StreamError."""

    def __init__(self):
        self.pending = bytearray()

    def feed(self, chunk):
        self.pending.extend(chunk)
        out = []
        while True:
            end = self.pending.find(b"\n")
            if end < 0:
                return out
            line = bytes(self.pending[:end]).rstrip(b"\r")
            if not line.startswith(BIN_PREFIX):
                del self.pending[:end + 1]
                out.append(("line", line))
                continue
            try:
                _, count, crc = line.split()
                count, crc = int(count), int(crc, 16)
            except ValueError as error:
                raise StreamError(f"not a packet line: {line!r}") from error
            if len(self.pending) < end + 1 + count:
                return out
            data = bytes(self.pending[end + 1:end + 1 + count])
            del self.pending[:end + 1 + count]
            if zlib.crc32(data) != crc:
                raise StreamError(f"a {count}-byte packet arrived damaged: crc32 {zlib.crc32(data):08x}, "
                                  f"the board sent {crc:08x}")
            out.append(("data", data))

    def rest(self):
        """What is left after the last whole line or packet: a line cut off at the end."""
        return bytes(self.pending)


@dataclass
class Frame:
    index: int
    dt_ms: int
    flags: int
    instances: int
    batches: int
    records: list = field(default_factory=list)  # (type name, body bytes) after the FRAME

    @property
    def keyframe(self):
        return bool(self.flags & KEYFRAME)


@dataclass
class Stream:
    header: dict
    packs: dict = field(default_factory=dict)    # slot -> {"name", "crc32"}
    frames: list = field(default_factory=list)
    checks: dict = field(default_factory=dict)   # frame index -> {"width", "height", "crc32"}
    counts: Counter = field(default_factory=Counter)


def records(data):
    """Each (type name, body) in `data`, a type this reader does not know as
    its number, in order; a record cut short raises StreamError."""
    at = 0
    while at < len(data):
        if at + HEAD.size > len(data):
            raise StreamError(f"a record head cut short at byte {at}")
        number, length = HEAD.unpack_from(data, at)
        at += HEAD.size
        if at + length > len(data):
            raise StreamError(f"a {RECORD_NAMES.get(number, number)} record of {length} bytes cut short at byte {at}")
        name = RECORD_NAMES.get(number, number)
        body = data[at:at + length]
        fixed = FIXED.get(name)
        if fixed is not None and length != fixed.size:
            raise StreamError(f"a {name} record of {length} bytes; the format's is {fixed.size}")
        yield name, body
        at += length


def text(raw):
    return raw.rstrip(b"\0").decode("ascii", errors="replace")


def decode_tune(body):
    name, value = TUNE.unpack(body)
    return text(name), value


def decode_mesh(body):
    fields = MESH.unpack(body)
    index, kind, placed = fields[:3]
    where = [(fields[3 + 2 * i], fields[4 + 2 * i]) for i in range(3)]
    kinds = {number: name for name, number in MESH_KINDS.items()}
    return {"index": index, "kind": kinds.get(kind, kind), "placed": bool(placed), "positions": where[0], "colours": where[1],
            "batch": where[2], "model": list(fields[9:])}


def parse(data):
    """The stream in `data`: its header, packs, frames with their records, and
    the CHECK hashes by the frame they hash."""
    stream = None
    for name, body in records(data):
        if stream is None:
            if name != "HEADER":
                raise StreamError(f"a stream opens with HEADER, not {name}")
            version, width, height, keyframe_every, build_id = HEADER.unpack(body)
            stream = Stream({"version": version, "panel_width": width, "panel_height": height,
                             "keyframe_every": keyframe_every, "build_id": text(build_id)})
            continue
        stream.counts[name] += 1
        if name == "FRAME":
            stream.frames.append(Frame(*FRAME.unpack(body)))
            continue
        if name == "PACK":
            slot, crc, pack_name = PACK.unpack(body)
            stream.packs[slot] = {"name": text(pack_name), "crc32": crc}
        elif name == "CHECK":
            index, width, height, crc = CHECK.unpack(body)
            stream.checks[index] = {"width": width, "height": height, "crc32": crc}
        if stream.frames:
            stream.frames[-1].records.append((name, body))
    if stream is None:
        raise StreamError("an empty stream: no HEADER")
    return stream


def summary(stream):
    frames = stream.frames
    return {
        **stream.header,
        "frames": len(frames),
        "keyframes": sum(f.keyframe for f in frames),
        "after_drop": sum(bool(f.flags & AFTER_DROP) for f in frames),
        "generated": sum(bool(f.flags & GENERATED) for f in frames),
        "first_index": frames[0].index if frames else None,
        "last_index": frames[-1].index if frames else None,
        "packs": {str(slot): pack for slot, pack in sorted(stream.packs.items())},
        "records": dict(sorted(stream.counts.items(), key=lambda item: str(item[0]))),
        "checks": len(stream.checks),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(prog="framerec.py", description="Summarise a saved frame record stream.")
    sub = parser.add_subparsers(dest="command", required=True)
    summarise = sub.add_parser("summary", help="the header, frame and record counts of FILE")
    summarise.add_argument("file")
    summarise.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    with open(args.file, "rb") as source:
        stream = parse(source.read())
    result = summary(stream)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for key, value in result.items():
            print(f"{key}: {value}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, StreamError) as error:
        print(f"framerec: {error}", file=sys.stderr)
        sys.exit(1)
