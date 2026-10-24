"""LCOL v0 colour sets; layout constants belong to render/lcol_format.h."""
import pathlib
import struct
from asset.asset_pack import NAME_BYTES, PackError
from build.c_header import read_define

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/render/lcol_format.h"
TYPE = b"LCOL"
VERSION = read_define(OWNER, "LCOL_VERSION")
RGB888 = read_define(OWNER, "LCOL_ENCODING_RGB888")
RGB565 = read_define(OWNER, "LCOL_ENCODING_RGB565")
STRIDES = {RGB888: read_define(OWNER, "LCOL_RGB888_BYTES"), RGB565: read_define(OWNER, "LCOL_RGB565_BYTES")}
MESH_SUFFIX = read_define(OWNER, "LCOL_MESH_SUFFIX")
NAME_SUFFIX = read_define(OWNER, "LCOL_NAME_SUFFIX")
HEADER = struct.Struct("<HHII")
assert HEADER.size == read_define(OWNER, "LCOL_HEADER_SIZE")


def name_for_mesh(mesh):
    stem = mesh.removesuffix(MESH_SUFFIX)
    name = stem + NAME_SUFFIX
    if not stem or "\0" in name:
        raise ValueError("LCOL name has an empty stem or embedded NUL")
    if len(name.encode("ascii")) >= NAME_BYTES:
        raise PackError("LCOL name does not fit the pack name field")
    return name


def decode(entry):
    if len(entry) < HEADER.size:
        raise ValueError("LCOL header is truncated")
    version, encoding, sets, count = HEADER.unpack_from(entry)
    if version != VERSION or encoding not in STRIDES or not sets:
        raise ValueError("LCOL version, encoding or set count is invalid")
    size = count * STRIDES[encoding]
    if HEADER.size + sets * size != len(entry):
        raise ValueError("LCOL sets do not match the entry size")
    return encoding, count, [entry[HEADER.size + k * size:HEADER.size + (k + 1) * size] for k in range(sets)]


def encode(encoding, count, sets):
    entry = HEADER.pack(VERSION, encoding, len(sets), count) + b"".join(sets)
    decoded = decode(entry)
    if list(sets) != decoded[2]:
        raise ValueError("LCOL set size does not match count and encoding")
    return entry


def validate_mesh(entry, mesh, colour_set):
    from r3d.mesh_asset import BLOB_HEADER
    encoding, count, sets = decode(entry)
    if len(mesh) < BLOB_HEADER.size:
        raise ValueError("LMSH header is truncated")
    vertices, triangles, _, _, _, _, colors_at, _, _, _, face_at = BLOB_HEADER.unpack_from(mesh)
    expected = vertices if encoding == RGB888 else triangles
    if colour_set >= len(sets) or count != expected or colors_at or face_at:
        raise ValueError("LCOL set, count or mesh colour ownership is invalid")
