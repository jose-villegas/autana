"""LLOD v1 mesh levels; layout constants belong to render/llod_format.h."""
import pathlib
from build.c_header import read_define
from r3d.lcol_asset import sibling_name
from r3d.mesh_asset import checked_struct, BLOB_HEADER, CLUSTER_LOD, LOD_GROUP, LOD_NONE, LOD_ENCLOSE_MARGIN

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/render/llod_format.h"
TYPE = b"LLOD"
VERSION = read_define(OWNER, "LLOD_VERSION")
NAME_SUFFIX = read_define(OWNER, "LLOD_NAME_SUFFIX")
HEADER_FIELDS = (("VERSION", "H"), ("GROUP_COUNT", "H"), ("CLUSTER_COUNT", "I"))
HEADER = checked_struct(OWNER, "LLOD", HEADER_FIELDS)


def name_for_mesh(mesh):
    return sibling_name(mesh, NAME_SUFFIX)


def decode(entry):
    if len(entry) < HEADER.size:
        raise ValueError("LLOD header is truncated")
    version, groups, clusters = HEADER.unpack_from(entry)
    if version != VERSION or not 0 < groups < LOD_NONE:
        raise ValueError("LLOD version or group count is invalid")
    at = HEADER.size + clusters * CLUSTER_LOD.size
    if at + groups * LOD_GROUP.size != len(entry):
        raise ValueError("LLOD records do not match the entry size")
    pairs = list(CLUSTER_LOD.iter_unpack(entry[HEADER.size:at]))
    rows = list(LOD_GROUP.iter_unpack(entry[at:]))
    if any(row[3] < 0 for row in rows):
        raise ValueError("LLOD group radius is negative")
    for own, parent in pairs:
        if any(value != LOD_NONE and value >= groups for value in (own, parent)):
            raise ValueError("LLOD cluster names an invalid group")
        if LOD_NONE in (own, parent):
            continue
        inner, outer = rows[own], rows[parent]
        room = outer[3] - inner[3] - LOD_ENCLOSE_MARGIN
        distance = sum((a - b) ** 2 for a, b in zip(outer[:3], inner[:3]))
        if inner[4] > outer[4] or room < 0 or distance > room * room:
            raise ValueError("LLOD group does not hold one simplified into it")
    return pairs, rows


def encode(cluster_lod, groups):
    entry = (HEADER.pack(VERSION, len(groups), len(cluster_lod)) +
             b"".join(CLUSTER_LOD.pack(*pair) for pair in cluster_lod) +
             b"".join(LOD_GROUP.pack(*(int(v) for v in row)) for row in groups))
    decode(entry)
    return entry


def validate_mesh(entry, mesh):
    cluster_lod, _ = decode(entry)
    if len(cluster_lod) != BLOB_HEADER.unpack_from(mesh)[2]:
        raise ValueError("LLOD cluster count disagrees with LMSH")
