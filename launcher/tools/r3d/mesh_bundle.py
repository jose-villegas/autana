"""Cache-internal carved mesh products and their versioned copy manifest."""

import json
import math
import struct

from asset.asset_pack import NAME_BYTES, PackError, build_pack, parse_pack
from build.name_hash import name_hash
from r3d import lcol_asset, mesh_asset
from r3d.lcol_asset import NAME_SUFFIX

SUFFIX = ".bundle"
MANIFEST = "copies"
MANIFEST_TYPE = b"MCOP"
VERSION = 1
HEADER = struct.Struct("<HHIII")
U16_MAX = (1 << 16) - 1
NAME = struct.Struct(f"<{NAME_BYTES}s")
TRANSFORM_AXES = 3
TRANSFORM = struct.Struct(f"<{TRANSFORM_AXES * (TRANSFORM_AXES + 1)}f")
RENDERER = struct.Struct(f"<H{NAME_BYTES}s")
METADATA = struct.Struct("<4I")
METADATA_FIELDS = ("owner", "first", "source", "match")
HASH_HEX_DIGITS = 8


def shape_id(mesh, rule, first):
    """A bounded shape id, stable under output ordering and copy count."""
    key = name_hash(json.dumps([mesh, rule, first], sort_keys=True))
    result = f"{mesh.split('.', 1)[0]}.{key:0{HASH_HEX_DIGITS}x}"
    if len((result + NAME_SUFFIX).encode("ascii")) >= NAME_BYTES:
        raise PackError(f"shape id {result!r} leaves no room for its colours")
    return result


def copy_name(owner, mesh, index):
    name = f"{owner}.{mesh.rsplit('.', 1)[-1]}.{index}"
    return name if len(name.encode("ascii")) < NAME_BYTES else f"{mesh}.{index}"


def field(value, what):
    raw = value.encode("ascii")
    if not raw or b"\0" in raw or len(raw) >= NAME_BYTES:
        raise PackError(f"{what} {value!r} must fit a NUL-padded {NAME_BYTES}-byte field")
    return raw


def text_at(raw, what):
    value, nul, rest = raw.partition(b"\0")
    if not value or not nul or any(rest):
        raise PackError(f"{what} is not NUL terminated and padded")
    return value.decode("ascii")


def encode_manifest(copies):
    if len(copies) > U16_MAX:
        raise PackError("too many copies in a mesh bundle")
    strings, offsets, records = bytearray(), {}, []
    for row in copies:
        metadata = []
        for key in METADATA_FIELDS:
            value = json.dumps(row[key], sort_keys=True) if key == "match" else row[key]
            if "\0" in value:
                raise PackError("mesh bundle metadata contains NUL")
            if value not in offsets:
                offsets[value] = len(strings)
                strings.extend(value.encode("utf-8") + b"\0")
            metadata.append(offsets[value])
        records.append(NAME.pack(field(row["name"], "copy")) +
                       TRANSFORM.pack(*[v for axis in row["matrix"] for v in axis], *row["position"]) +
                       RENDERER.pack(row["colour_set"], field(row["mesh"], "mesh")) +
                       METADATA.pack(*metadata))
    data = b"".join(records)
    result = HEADER.pack(VERSION, 0, len(copies), HEADER.size + len(data), len(strings)) + data + strings
    decode_manifest(result)
    return result


def decode_manifest(data):
    if len(data) < HEADER.size:
        raise PackError("mesh bundle manifest header is truncated")
    version, reserved, count, at, size = HEADER.unpack_from(data)
    if version != VERSION:
        raise PackError("unknown mesh bundle manifest version")
    stride = NAME.size + TRANSFORM.size + RENDERER.size + METADATA.size
    if reserved or count > U16_MAX or at != HEADER.size + count * stride or at + size != len(data):
        raise PackError("mesh bundle manifest sections are invalid")
    strings = data[at:]
    copies, names = [], set()
    for index in range(count):
        offset = HEADER.size + index * stride
        (raw,) = NAME.unpack_from(data, offset)
        name = text_at(raw, "copy")
        offset += NAME.size
        transform = TRANSFORM.unpack_from(data, offset)
        offset += TRANSFORM.size
        colour_set, mesh = RENDERER.unpack_from(data, offset)
        offset += RENDERER.size
        if name in names or not all(map(math.isfinite, transform)):
            raise PackError("mesh bundle copy row is invalid")
        names.add(name)
        row = dict(name=name, mesh=text_at(mesh, "mesh"), colour_set=colour_set,
                   matrix=[list(transform[i:i + TRANSFORM_AXES])
                           for i in range(0, TRANSFORM_AXES ** 2, TRANSFORM_AXES)],
                   position=list(transform[TRANSFORM_AXES ** 2:]))
        for key, start in zip(METADATA_FIELDS, METADATA.unpack_from(data, offset)):
            end = strings.find(b"\0", start)
            if start >= len(strings) or end < 0 or (start and strings[start - 1]):
                raise PackError("mesh bundle metadata string is invalid")
            text = strings[start:end].decode("utf-8")
            row[key] = json.loads(text) if key == "match" else text
        copies.append(row)
    return copies


def encode(entries, copies):
    return build_pack(sorted([*entries, (MANIFEST, MANIFEST_TYPE, encode_manifest(copies))]))


def decode(data):
    entries = parse_pack(data)
    if MANIFEST not in entries:
        raise PackError("mesh bundle has no manifest")
    kind, manifest = entries.pop(MANIFEST)
    if kind != MANIFEST_TYPE:
        raise PackError("mesh bundle manifest has the wrong type")
    copies = decode_manifest(manifest)
    for row in copies:
        mesh = entries.get(row["mesh"])
        colours = entries.get(lcol_asset.name_for_mesh(row["mesh"]))
        if mesh is None or mesh[0] != mesh_asset.TYPE or colours is None or colours[0] != lcol_asset.TYPE:
            raise PackError("mesh bundle copy needs an LMSH and LCOL")
        lcol_asset.validate_mesh(colours[1], mesh[1], row["colour_set"])
    return [(name, kind, blob) for name, (kind, blob) in sorted(entries.items())], copies
