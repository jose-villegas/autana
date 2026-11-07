"""LMOR v1 vertex morph targets; layout constants belong to render/lmor_format.h.

Per cluster a shift, per vertex row an int8 delta per axis: the row's target
is its position plus delta << shift, in ticks. With LMOR_COLOURS, per vertex
row the colour it takes at that target follows."""
import pathlib
import struct

from build.c_header import read_define
from r3d.lcol_asset import sibling_name
from r3d.mesh_asset import checked_struct, BLOB_HEADER

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/render/lmor_format.h"
TYPE = b"LMOR"
VERSION = read_define(OWNER, "LMOR_VERSION")
COLOURS = read_define(OWNER, "LMOR_COLOURS")
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


def sizes(clusters, vertices, colours):
    """Byte offsets of the shifts, deltas and colours, and the entry's size."""
    shifts_at = HEADER.size
    deltas_at = shifts_at + _padded(clusters)
    colours_at = deltas_at + _padded(vertices * AXES)
    end = colours_at + (_padded(vertices * AXES) if colours else 0)
    return shifts_at, deltas_at, colours_at, end


def decode(entry):
    """(shifts, deltas, colours or None) as bytes-like lists: shifts per cluster, deltas and colours per row."""
    if len(entry) < HEADER.size:
        raise ValueError("LMOR header is truncated")
    version, flags, clusters, vertices = HEADER.unpack_from(entry)
    if version != VERSION or flags & ~COLOURS:
        raise ValueError("LMOR version or flags are invalid")
    shifts_at, deltas_at, colours_at, end = sizes(clusters, vertices, flags & COLOURS)
    if end != len(entry):
        raise ValueError("LMOR parts do not match the entry size")
    shifts = list(entry[shifts_at:shifts_at + clusters])
    if any(s > SHIFT_MAX for s in shifts):
        raise ValueError("LMOR shift is out of range")
    deltas = list(struct.unpack_from(f"<{vertices * AXES}b", entry, deltas_at))
    if min(deltas, default=0) < DELTA_MIN:
        raise ValueError("LMOR delta is out of range")
    colours = list(entry[colours_at:colours_at + vertices * AXES]) if flags & COLOURS else None
    return shifts, deltas, colours


def encode(shifts, deltas, colours=None):
    """shifts per cluster, deltas (rows x AXES ints) and optional colours (rows x AXES)."""
    clusters, vertices = len(shifts), len(deltas) // AXES
    _, deltas_at, colours_at, end = sizes(clusters, vertices, colours is not None)
    out = bytearray(end)
    HEADER.pack_into(out, 0, VERSION, COLOURS if colours is not None else 0, clusters, vertices)
    out[HEADER.size:HEADER.size + clusters] = bytes(shifts)
    struct.pack_into(f"<{vertices * AXES}b", out, deltas_at, *deltas)
    if colours is not None:
        out[colours_at:colours_at + vertices * AXES] = bytes(colours)
    entry = bytes(out)
    decode(entry)
    return entry


def validate_mesh(entry, mesh):
    version, flags, clusters, vertices = HEADER.unpack_from(entry)
    del version, flags
    _, _, mesh_clusters, *_ = BLOB_HEADER.unpack_from(mesh)
    if clusters != mesh_clusters or vertices != BLOB_HEADER.unpack_from(mesh)[0]:
        raise ValueError("LMOR counts disagree with LMSH")
    decode(entry)
