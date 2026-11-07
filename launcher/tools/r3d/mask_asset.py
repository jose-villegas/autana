"""LMSK v1 mask atlas; layout constants belong to render/lmsk_format.h."""
import pathlib
import struct

from build.c_header import read_define
from r3d.lcol_asset import sibling_name
from r3d.mesh_asset import BLOB_HEADER, CLUSTER, LOD_NONE, checked_struct

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/render/lmsk_format.h"
TYPE = b"LMSK"
VERSION = read_define(OWNER, "LMSK_VERSION")
MASK_WIDTH = read_define(OWNER, "LMSK_MASK_WIDTH")
UV_FRACTION_BITS = read_define(OWNER, "LMSK_UV_FRACTION_BITS")
NAME_SUFFIX = read_define(OWNER, "LMSK_NAME_SUFFIX")
HEADER_FIELDS = (("VERSION", "H"), ("MASK_WIDTH", "H"), ("VERTEX_FIRST", "I"), ("MASK_ROWS", "I"))
HEADER = checked_struct(OWNER, "LMSK", HEADER_FIELDS)
UV_BITS = 16
UV = struct.Struct("<2H")
MAX_ROWS = (1 << UV_BITS) >> UV_FRACTION_BITS
ROW = struct.Struct("<I")


def name_for_mesh(mesh):
    return sibling_name(mesh, NAME_SUFFIX)


def decode(entry, vertex_count):
    if len(entry) < HEADER.size:
        raise ValueError("LMSK header is truncated")
    version, width, first, rows = HEADER.unpack_from(entry)
    if version != VERSION or width != MASK_WIDTH:
        raise ValueError("LMSK version or width is invalid")
    if not 0 <= first <= vertex_count:
        raise ValueError("LMSK vertex_first is invalid")
    atlas_at = HEADER.size + (vertex_count - first) * UV.size
    if atlas_at + rows * ROW.size != len(entry):
        raise ValueError("LMSK records do not match the entry size")
    uvs = list(UV.iter_unpack(entry[HEADER.size:atlas_at]))
    if any(u >= width << UV_FRACTION_BITS or v >= rows << UV_FRACTION_BITS for u, v in uvs):
        raise ValueError("LMSK UV is outside the atlas")
    return first, uvs, [row for row, in ROW.iter_unpack(entry[atlas_at:])]


def encode(vertex_first, uvs, rows):
    if len(rows) > MAX_ROWS:
        raise ValueError(f"LMSK atlas has {len(rows)} rows; limit is {MAX_ROWS}")
    entry = (HEADER.pack(VERSION, MASK_WIDTH, vertex_first, len(rows)) +
             b"".join(UV.pack(*uv) for uv in uvs) + b"".join(ROW.pack(row) for row in rows))
    decode(entry, vertex_first + len(uvs))
    return entry


def validate_mesh(entry, mesh, levels=None):
    vertices, _, count, _, _, _, _, _, at, _, _ = BLOB_HEADER.unpack_from(mesh)
    clusters = [CLUSTER.unpack_from(mesh, at + i * CLUSTER.size) for i in range(count)]
    masked = [i for i, cluster in enumerate(clusters) if cluster[-1]]
    if not masked or masked != list(range(masked[0], count)):
        raise ValueError("LMSK needs a final run of masked clusters")
    first, _, _ = decode(entry, vertices)
    if first != clusters[masked[0]][0]:
        raise ValueError("LMSK vertex_first disagrees with LMSH")
    if levels is not None:
        from r3d.lod_asset import decode as decode_levels
        pairs, _ = decode_levels(levels)
        if any(pairs[i] != (LOD_NONE, LOD_NONE) for i in masked):
            raise ValueError("LMSK clusters must be finest and ungrouped")
