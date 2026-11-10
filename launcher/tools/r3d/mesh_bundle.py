"""Cache-internal carved mesh products and their versioned copy manifest."""

import json
import math
import struct

from asset.asset_pack import NAME_BYTES, PackError, build_pack, parse_pack
from build.name_hash import name_hash
from r3d import lcol_asset, lod_asset, mask_asset, mesh_asset
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
    for name, (kind, blob) in entries.items():
        if kind == mesh_asset.TYPE:
            levels = entries.get(lod_asset.name_for_mesh(name))
            if levels is not None and levels[0] != lod_asset.TYPE:
                raise PackError("LLOD sibling has the wrong type")
        if kind == lod_asset.TYPE:
            mesh = entries.get(name.removesuffix(lod_asset.NAME_SUFFIX))
            if mesh is None or mesh[0] != mesh_asset.TYPE or not name.endswith(lod_asset.NAME_SUFFIX):
                raise PackError("LLOD sibling has no LMSH")
            lod_asset.validate_mesh(blob, mesh[1])
        if kind == mask_asset.TYPE:
            mesh = entries.get(name.removesuffix(mask_asset.NAME_SUFFIX))
            if mesh is None or mesh[0] != mesh_asset.TYPE or not name.endswith(mask_asset.NAME_SUFFIX):
                raise PackError("LMSK sibling has no LMSH")
            levels = entries.get(lod_asset.name_for_mesh(name.removesuffix(mask_asset.NAME_SUFFIX)))
            mask_asset.validate_mesh(blob, mesh[1], None if levels is None else levels[1])
        if kind == mesh_asset.TYPE:
            try:
                mask = entries.get(mask_asset.name_for_mesh(name))
            except PackError:
                mask = None
            if mask is not None and mask[0] != mask_asset.TYPE:
                raise PackError("LMSK sibling has the wrong type")
            if mask is None:
                header = mesh_asset.BLOB_HEADER.unpack_from(blob)
                if any(mesh_asset.CLUSTER.unpack_from(blob, header[8] + i * mesh_asset.CLUSTER.size)[-1]
                       for i in range(header[2])):
                    raise PackError("masked LMSH has no LMSK")
    for row in copies:
        mesh = entries.get(row["mesh"])
        colours = entries.get(lcol_asset.name_for_mesh(row["mesh"]))
        if mesh is None or mesh[0] != mesh_asset.TYPE or colours is None or colours[0] != lcol_asset.TYPE:
            raise PackError("mesh bundle copy needs an LMSH and LCOL")
        lcol_asset.validate_mesh(colours[1], mesh[1], row["colour_set"])
    return [(name, kind, blob) for name, (kind, blob) in sorted(entries.items())], copies


def mesh_entries(name, mesh):
    """The LMSH and optional levels and mask siblings of one assembled mesh."""
    from r3d.lit_mesh import mesh_blob, lod_blob
    entries = [(name, mesh_asset.TYPE, mesh_blob(mesh))]
    levels = lod_blob(mesh)
    if levels is not None:
        entries.append((lod_asset.name_for_mesh(name), lod_asset.TYPE, levels))
    if getattr(mesh, "mask_uvs", None) is not None:
        mask = mask_asset.encode(mesh.mask_vertex_first, mesh.mask_uvs, mesh.mask_rows)
        mask_asset.validate_mesh(mask, entries[0][2], levels)
        entries.append((mask_asset.name_for_mesh(name), mask_asset.TYPE, mask))
    return entries


def read_mesh(entries, name):
    """Read a bundle mesh with its optional levels and mask siblings."""
    from r3d.lit_mesh import read_lit_mesh
    blobs = {key: blob for key, _, blob in entries}
    try:
        mask = blobs.get(mask_asset.name_for_mesh(name))
    except PackError:
        mask = None
    return read_lit_mesh(blobs[name], blobs.get(lod_asset.name_for_mesh(name)), mask)


def read_one_mesh(data, name):
    """Mesh `name` of the bundle `data`, with its LLOD sibling if any, decoding no other entry: a fit's rest
    beside the copies its start kept, which may hold an older LLOD."""
    entries = parse_pack(data)
    if name not in entries or entries[name][0] != mesh_asset.TYPE:
        raise PackError(f"mesh bundle has no mesh {name}")
    return read_mesh([(key, kind, blob) for key, (kind, blob) in entries.items()], name)


def rest_name(entries, copies):
    """The unique mesh not used as a copy prototype."""
    shapes = {row["mesh"] for row in copies}
    rests = [key for key, kind, _ in entries if kind == mesh_asset.TYPE and key not in shapes]
    if len(rests) != 1:
        raise PackError("a mesh bundle needs one rest mesh")
    return rests[0]


def rename_rest(entries, copies, name):
    """Retarget a replacement bundle's sole rest mesh and its siblings to a pack entry id."""
    old = rest_name(entries, copies)
    if old == name:
        return entries
    names = {old: name}
    for key, _, _ in entries:
        if key.startswith(old) and key[len(old):] in (lcol_asset.NAME_SUFFIX, lod_asset.NAME_SUFFIX, mask_asset.NAME_SUFFIX):
            names[key] = lcol_asset.sibling_name(name, key[len(old):])
    return [(names.get(key, key), kind, blob) for key, kind, blob in entries]
