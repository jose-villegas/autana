"""The LUVS entry's layout and sibling names, without numpy: the firmware build checks a textured bundle with
these alone (mesh_bundle.decode), and uv_transfer.py writes and reads the arrays they frame.
The layout constants belong to render/texture/r3d_texture.h."""
import pathlib
import struct

from build.c_header import read_define
from r3d.lcol_asset import sibling_name
from r3d.mesh_asset import checked_struct

OWNER = pathlib.Path(__file__).resolve().parents[3] / "main/render/texture/r3d_texture.h"
VERSION = read_define(OWNER, "R3D_TEXTURE_UVS_VERSION")
NAME_SUFFIX = read_define(OWNER, "R3D_TEXTURE_UVS_NAME_SUFFIX")
NONE = read_define(OWNER, "R3D_TEXTURE_NONE")
MATERIALS_MAX = NONE
TEXTURE_SUFFIX = read_define(OWNER, "R3D_TEXTURE_MATERIAL_SUFFIX")
CHARTS = read_define(OWNER, "R3D_TEXTURE_CHARTS")
TRIANGLES_PER_BYTE = read_define(OWNER, "R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE")
HEADER_FIELDS = (("VERSION", "H"), ("MATERIAL_COUNT", "H"), ("VERTEX_COUNT", "I"), ("CLUSTER_COUNT", "I"),
                 ("TRIANGLE_COUNT", "I"), ("UV_COUNT", "I"), ("UVS_OFF", "I"),
                 ("SHIFTS_OFF", "I"), ("CLUSTERS_OFF", "I"), ("TRIANGLES_OFF", "I"))
HEADER = checked_struct(OWNER, "R3D_TEXTURE_UVS", HEADER_FIELDS)
PAIR = struct.Struct("<2h")
SHIFT = struct.Struct("<i")


def header(entry):
    """An LUVS entry's header fields, after checking its version and that its sections fit the entry end to end."""
    fields = HEADER.unpack_from(entry)
    (version, _, _, clusters, triangles, uv_count, uvs_off, shifts_off, clusters_off, triangles_off) = fields
    if version != VERSION:
        raise ValueError("LUVS version is invalid")
    if (uvs_off != HEADER.size or shifts_off != uvs_off + uv_count * PAIR.size
            or clusters_off != shifts_off + clusters * SHIFT.size or triangles_off != clusters_off + clusters * CHARTS
            or triangles_off + -(-triangles // TRIANGLES_PER_BYTE) != len(entry)):
        raise ValueError("LUVS sections do not match the entry size")
    return fields


def name_for_mesh(mesh):
    return sibling_name(mesh, NAME_SUFFIX)


def texture_name(mesh, index):
    """The pack id of the mesh's `index`th material texture: a sibling of the mesh, as a material's own name may
    not fit a pack name."""
    return sibling_name(mesh, f"{TEXTURE_SUFFIX}{index}")
