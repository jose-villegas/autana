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
BLOB_HEADER = struct.Struct("<12I")
# Ranges, box, double_sided, a reserved byte and back_first (r3d_lit_cluster_t).
CLUSTER = struct.Struct("<4H6hBxH")
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
    vertices, triangles, _, _, _, _, colors_at, _, _, _, face_at, _ = BLOB_HEADER.unpack_from(mesh)
    return MeshHeader(vertices, triangles, colors_at, face_at)

def back_runs(clusters, per_vertex):
    """(first, count, back_first) for each cluster (vertex base, vertices, triangle base, triangles, ..., double):
    the run of its own entries, one per vertex (`per_vertex`) or per triangle, and where that run starts in the
    back colours. The double-sided clusters' runs tile the back colours in cluster order; a single-sided
    cluster's count and back_first are 0."""
    end = 0
    for vertex_base, vertices, triangle_base, triangles, *_, double in clusters:
        first, count = (vertex_base, vertices) if per_vertex else (triangle_base, triangles)
        count = count if double else 0
        yield first, count, end if double else 0
        end += count


def back_entries(mesh, per_vertex):
    """The back colours an LMSH entry's double-sided clusters hold: one per vertex (`per_vertex`) or per
    triangle, each cluster's from its back_first in cluster order (r3d_lit_mesh_t.back_colors)."""
    _, _, clusters, _, _, _, _, _, clusters_at, *_ = BLOB_HEADER.unpack_from(mesh)
    rows = [CLUSTER.unpack_from(mesh, clusters_at + index * CLUSTER.size) for index in range(clusters)]
    total = 0
    for row, (_, count, back_first) in zip(rows, back_runs([row[:-1] for row in rows], per_vertex)):
        if row[-1] != back_first:
            raise PackError("LMSH back colour runs do not tile in cluster order")
        total += count
    return total


def checked_struct(owner, prefix, fields, define=read_define):
    """A little-endian header whose field offsets and size match its C defines."""
    header = struct.Struct("<" + "".join(layout for _, layout in fields))
    offset = 0
    for field, layout in fields:
        if offset != define(owner, prefix + "_AT_" + field):
            raise ValueError(prefix + " header field offset disagrees with its C owner")
        offset += struct.calcsize("<" + layout)
    if header.size != define(owner, prefix + "_HEADER_SIZE") or offset != header.size:
        raise ValueError(prefix + " header size disagrees with its C owner")
    return header
