"""LMOR v1 vertex morph targets; layout constants belong to render/lmor_format.h.

Per cluster a shift, per vertex row an int8 delta per axis: the row's target
is its position plus delta << shift, in ticks. With LMOR_LANDING, per vertex
row the point it lands on follows: a coarse triangle and two corner weights,
from which the draw blends toward the colours the mesh draws with."""
import pathlib
import struct

from build.c_header import read_define
from r3d.lcol_asset import sibling_name
from r3d.mesh_asset import checked_struct, BLOB_HEADER

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/render/lmor_format.h"
TYPE = b"LMOR"
VERSION = read_define(OWNER, "LMOR_VERSION")
LANDING = read_define(OWNER, "LMOR_LANDING")
LANDING_SIZE = read_define(OWNER, "LMOR_LANDING_SIZE")
WEIGHT_WHOLE = read_define(OWNER, "LMOR_WEIGHT_WHOLE")
LANDING_POINT = struct.Struct("<HBB")
AXES = read_define(OWNER, "LMOR_AXES")
NAME_SUFFIX = read_define(OWNER, "LMOR_NAME_SUFFIX")
HEADER_FIELDS = (("VERSION", "H"), ("FLAGS", "H"), ("CLUSTER_COUNT", "I"), ("VERTEX_COUNT", "I"))
HEADER = checked_struct(OWNER, "LMOR", HEADER_FIELDS)
ALIGN = 4
# Symmetric, so mirroring an axis (asset/engine_frame.py) still fits int8.
DELTA_MAX = 127
DELTA_MIN = -DELTA_MAX
SHIFT_MAX = read_define(OWNER, "LMOR_SHIFT_MAX")


def name_for_mesh(mesh):
    return sibling_name(mesh, NAME_SUFFIX)


def _padded(size):
    return size + (-size % ALIGN)


def sizes(clusters, vertices, landing):
    """Byte offsets of the shifts, deltas and landing points, and the entry's size."""
    shifts_at = HEADER.size
    deltas_at = shifts_at + _padded(clusters)
    landing_at = deltas_at + _padded(vertices * AXES)
    end = landing_at + (vertices * LANDING_SIZE if landing else 0)
    return shifts_at, deltas_at, landing_at, end


def decode(entry):
    """(shifts, deltas, landing or None): shifts per cluster, deltas per row and axis, a
    (triangle, weight, weight) landing point per row."""
    if len(entry) < HEADER.size:
        raise ValueError("LMOR header is truncated")
    version, flags, clusters, vertices = HEADER.unpack_from(entry)
    if version != VERSION or flags & ~LANDING:
        raise ValueError("LMOR version or flags are invalid")
    shifts_at, deltas_at, landing_at, end = sizes(clusters, vertices, flags & LANDING)
    if end != len(entry):
        raise ValueError("LMOR parts do not match the entry size")
    shifts = list(entry[shifts_at:shifts_at + clusters])
    if any(s > SHIFT_MAX for s in shifts):
        raise ValueError("LMOR shift is out of range")
    deltas = list(struct.unpack_from(f"<{vertices * AXES}b", entry, deltas_at))
    if min(deltas, default=0) < DELTA_MIN:
        raise ValueError("LMOR delta is out of range")
    landing = None
    if flags & LANDING:
        landing = [LANDING_POINT.unpack_from(entry, landing_at + row * LANDING_SIZE) for row in range(vertices)]
        if any(a + b > WEIGHT_WHOLE for _, a, b in landing):
            raise ValueError("LMOR landing weights exceed the whole")
    return shifts, deltas, landing


def encode(shifts, deltas, landing=None):
    """shifts per cluster, deltas (rows x AXES ints) and optional (triangle, weight, weight) per row."""
    clusters, vertices = len(shifts), len(deltas) // AXES
    _, deltas_at, landing_at, end = sizes(clusters, vertices, landing is not None)
    out = bytearray(end)
    HEADER.pack_into(out, 0, VERSION, LANDING if landing is not None else 0, clusters, vertices)
    out[HEADER.size:HEADER.size + clusters] = bytes(shifts)
    struct.pack_into(f"<{vertices * AXES}b", out, deltas_at, *deltas)
    for row, point in enumerate(landing or ()):
        LANDING_POINT.pack_into(out, landing_at + row * LANDING_SIZE, *(int(v) for v in point))
    entry = bytes(out)
    decode(entry)
    return entry


def validate_mesh(entry, mesh):
    version, flags, clusters, vertices = HEADER.unpack_from(entry)
    del version, flags
    mesh_vertices, triangles, mesh_clusters, *_ = BLOB_HEADER.unpack_from(mesh)
    if clusters != mesh_clusters or vertices != mesh_vertices:
        raise ValueError("LMOR counts disagree with LMSH")
    landing = decode(entry)[2]
    if landing is not None and any(triangle >= triangles for triangle, _, _ in landing):
        raise ValueError("LMOR lands on a triangle LMSH lacks")
