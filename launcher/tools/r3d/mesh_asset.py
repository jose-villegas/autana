"""The lit mesh pack entry's layout, which main/render/r3d_lit_mesh.c checks the
C structs against: the entry type, then a header of counts, the position scale
and the offset of each array from the entry's first byte (0 for an array the
mesh has none of), then the cluster and node rows. Standard library only, so
the pack builder runs without the numeric environment."""

import struct
from pathlib import Path
from typing import NamedTuple

from asset.asset_pack import PackError
from build.c_header import read_define

MESH_HEADER = Path(__file__).resolve().parents[2] / "main" / "render" / "r3d_lit_mesh.h"

TYPE = b"LMSH"
BLOB_HEADER = struct.Struct("<11I")
CLUSTER = struct.Struct("<4H6hBx")
NODE = struct.Struct("<6hHBB")
# A cluster's own and parent group; a group's bounds (centre and radius, ticks) and error (ticks).
CLUSTER_LOD = struct.Struct("<2H")
LOD_GROUP = struct.Struct("<4hHh")
LOD_NONE = read_define(MESH_HEADER, "R3D_LOD_NONE")
LOD_ENCLOSE_MARGIN = read_define(MESH_HEADER, "R3D_LOD_ENCLOSE_MARGIN")


class MeshHeader(NamedTuple):
    vertices: int
    triangles: int
    colors_at: int
    face_at: int


def read_mesh_header(mesh):
    """Counts and colour-array offsets from an LMSH header."""
    if len(mesh) < BLOB_HEADER.size:
        raise PackError("LMSH header is truncated")
    vertices, triangles, _, _, _, _, colors_at, _, _, _, face_at, *_ = BLOB_HEADER.unpack_from(mesh)
    return MeshHeader(vertices, triangles, colors_at, face_at)

def checked_struct(owner, prefix, fields, define=read_define, *, size_suffix="HEADER_SIZE"):
    """A little-endian struct with offsets and size from its C owner."""
    offset, formats = 0, ["<"]
    for field, layout in fields:
        start = define(owner, prefix + "_AT_" + field)
        if start < offset:
            raise ValueError(f"{prefix}.{field} overlaps the preceding field in {owner}")
        formats.extend((f"{start - offset}x", layout))
        offset = start + struct.calcsize("<" + layout)
    size = define(owner, prefix + "_" + size_suffix)
    if size < offset:
        raise ValueError(f"{prefix} size {size} does not hold its fields in {owner}")
    formats.append(f"{size - offset}x")
    return struct.Struct("".join(formats))
