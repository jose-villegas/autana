"""LUVS v1 mesh texturing: a baked mesh's UVs, carried over from the source's, and each cluster's material.
The layout constants belong to render/texture/r3d_texture.h.

A cluster takes the material most of its area lies on, when that is at least TEXTURED_SHARE of it, and one planar
chart: the source triangle under the largest of its triangles of that material. Every vertex of the cluster takes
its UV from that triangle's plane, so a cluster's UVs agree at every vertex and a planar wall keeps the source's
mapping exactly."""
import dataclasses
import fnmatch
import pathlib
import struct

import numpy as np

from build.c_header import read_define
from r3d.geometry import nearest_triangles, triangle_search
from r3d.lcol_asset import sibling_name
from r3d.mesh_asset import checked_struct
from r3d.texture import texture_asset

OWNER = pathlib.Path(__file__).resolve().parents[3] / "main/render/texture/r3d_texture.h"
TYPE = texture_asset.UVS_TYPE
VERSION = read_define(OWNER, "R3D_TEXTURE_UVS_VERSION")
NAME_SUFFIX = read_define(OWNER, "R3D_TEXTURE_UVS_NAME_SUFFIX")
NONE = read_define(OWNER, "R3D_TEXTURE_NONE")
UV_SHIFT = read_define(OWNER, "R3D_TEXTURE_UV_SHIFT")
MATERIALS_MAX = NONE
TEXTURE_SUFFIX = read_define(OWNER, "R3D_TEXTURE_MATERIAL_SUFFIX")
HEADER_FIELDS = (("VERSION", "H"), ("MATERIAL_COUNT", "H"), ("VERTEX_COUNT", "I"), ("CLUSTER_COUNT", "I"),
                 ("UVS_OFF", "I"), ("ORIGINS_OFF", "I"), ("CLUSTERS_OFF", "I"))
HEADER = checked_struct(OWNER, "R3D_TEXTURE_UVS", HEADER_FIELDS)
PAIR = struct.Struct("<2h")
INT16 = np.iinfo(np.int16)
# A baked triangle takes its nearest source triangle's material when their normals agree within this.
NORMAL_DEGREES = 10.0
# A cluster is textured when this share of its area lies on one textured material.
TEXTURED_SHARE = 0.5


@dataclasses.dataclass
class Luvs:
    uvs: np.ndarray        # (vertices, 2) int16, 1/16 texel of level 0 from the vertex's cluster's origin
    origins: np.ndarray    # (clusters, 2) int16, whole texture sides
    materials: np.ndarray  # (clusters,) uint8, an index into names or NONE
    material_count: int    # its materials' LTEX entries are the mesh's siblings texture_name(mesh, 0..count-1)


def matched(names, patterns):
    """The indices of `names` matching any of `patterns`, as import tables match objects."""
    return [i for i, name in enumerate(names) if any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)]


def unit_normals(corners):
    normal = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    length = np.linalg.norm(normal, axis=1)
    return normal / np.maximum(length, np.finfo(np.float64).tiny)[:, None], 0.5 * length


def texel_uvs(source, size):
    """Each source triangle's corner UVs in texels of a `size` texture, v down the image as obj.Texture samples."""
    uv = np.asarray(source.uv, dtype=np.float64)[np.asarray(source.tri_t)]
    return np.stack([uv[..., 0] * size, (1.0 - uv[..., 1]) * size], axis=-1)


def plane_uv(points, corners, uv):
    """UVs of `points` projected on the plane of triangle `corners` (3, 3) with corner UVs `uv` (3, 2), by the
    barycentrics of the projection, unclamped: exact for a planar mapping, outside the triangle too. None for a
    triangle with no area."""
    e1, e2 = corners[1] - corners[0], corners[2] - corners[0]
    gram = np.array([[e1 @ e1, e1 @ e2], [e1 @ e2, e2 @ e2]])
    if abs(np.linalg.det(gram)) <= np.finfo(np.float64).eps * max(gram[0, 0] * gram[1, 1], 1.0):
        return None
    d = points - corners[0]
    st = np.linalg.solve(gram, np.stack([d @ e1, d @ e2]))
    return uv[0] + st[0][:, None] * (uv[1] - uv[0]) + st[1][:, None] * (uv[2] - uv[0])


def cluster_uvs(mesh, source, material_ids, sizes):
    """The Luvs of `mesh` (lit_mesh.read_lit_mesh's) over `source` (mesh_source.load_source's, in the bake's frame)
    for source materials `material_ids`, each `sizes[k]` texels a side."""
    if len(material_ids) > MATERIALS_MAX:
        raise ValueError(f"{len(material_ids)} textured materials; LUVS holds {MATERIALS_MAX}")
    points = np.asarray(mesh.pos, dtype=np.float64) / float(mesh.position_scale)
    tris = np.asarray(mesh.tris)
    source_corners = np.asarray(source.p, dtype=np.float64)[np.asarray(source.tri_v)]
    near, _bary, _distance = nearest_triangles(points[tris].mean(axis=1), triangle_search(source_corners))
    baked_normal, area = unit_normals(points[tris])
    source_normal, _ = unit_normals(source_corners)
    facing = (baked_normal * source_normal[near]).sum(axis=1) >= np.cos(np.radians(NORMAL_DEGREES))
    slot = np.full(len(source.names), NONE)
    slot[list(material_ids)] = np.arange(len(material_ids))
    material = np.where(facing, slot[np.asarray(source.tri_m)[near]], NONE)
    uvs = np.zeros((len(points), 2), dtype=np.int16)
    origins = np.zeros((len(mesh.clusters), 2), dtype=np.int16)
    materials = np.full(len(mesh.clusters), NONE, dtype=np.uint8)
    for c, (vb, vc, tb, tc, *_box) in enumerate(mesh.clusters):
        own, own_area = material[tb:tb + tc], area[tb:tb + tc]
        shares = np.bincount(own[own != NONE], own_area[own != NONE], minlength=len(material_ids))
        if not len(shares) or shares.max() < TEXTURED_SHARE * own_area.sum():
            continue
        k = int(shares.argmax())
        size = sizes[k]
        texels = texel_uvs(source, size)
        for t in tb + np.argsort(-np.where(own == k, own_area, -1.0)):
            chart = near[t]
            uv = plane_uv(points[vb:vb + vc], source_corners[chart], texels[chart])
            if uv is not None and material[t] == k:
                break
        else:
            continue
        origin = np.floor(uv.mean(axis=0) / size)
        stored = np.rint((uv - origin * size) * (1 << UV_SHIFT))
        if stored.min() < INT16.min or stored.max() > INT16.max or np.abs(origin).max() > INT16.max:
            raise ValueError(f"cluster {c} spans more texels than an int16 UV holds; lower the texture size")
        uvs[vb:vb + vc], origins[c], materials[c] = stored, origin, k
    return Luvs(uvs, origins, materials, len(material_ids))


def pack_luvs(luvs):
    """The LUVS entry: header, UVs, origins, then cluster materials."""
    uvs_off = HEADER.size
    origins_off = uvs_off + len(luvs.uvs) * PAIR.size
    clusters_off = origins_off + len(luvs.origins) * PAIR.size
    return (HEADER.pack(VERSION, luvs.material_count, len(luvs.uvs), len(luvs.origins), uvs_off, origins_off,
                        clusters_off)
            + luvs.uvs.astype("<i2").tobytes() + luvs.origins.astype("<i2").tobytes()
            + luvs.materials.astype(np.uint8).tobytes())


def decode(entry):
    """The Luvs an LUVS entry holds."""
    version, count, vertices, clusters, uvs_off, origins_off, clusters_off = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ValueError("LUVS version is invalid")
    uvs = np.frombuffer(entry, "<i2", vertices * 2, uvs_off).reshape(-1, 2)
    origins = np.frombuffer(entry, "<i2", clusters * 2, origins_off).reshape(-1, 2)
    materials = np.frombuffer(entry, np.uint8, clusters, clusters_off)
    if clusters_off + clusters != len(entry):
        raise ValueError("LUVS sections do not match the entry size")
    return Luvs(uvs.copy(), origins.copy(), materials.copy(), count)


def name_for_mesh(mesh):
    return sibling_name(mesh, NAME_SUFFIX)


def texture_name(mesh, index):
    """The pack id of the mesh's `index`th material texture: a sibling of the mesh, as a material's own name may
    not fit a pack name."""
    return sibling_name(mesh, f"{TEXTURE_SUFFIX}{index}")


def textured_entries(mesh, mesh_id, source, patterns, size):
    """The pack rows (name, type, bytes) texturing `mesh_id`: one LTEX per source material matching `patterns` that
    has a texture, and the mesh's LUVS."""
    chosen = [i for i in matched(source.names, patterns) if source.textures[i] is not None]
    if not chosen:
        raise ValueError(f"no textured source material matches {patterns}")
    luvs = cluster_uvs(mesh, source, chosen, [size] * len(chosen))
    rows = [(texture_name(mesh_id, k), texture_asset.TYPE,
             texture_asset.pack_ltex(texture_asset.material_texture(source.textures[i].levels[0], size,
                                                                    drawn_tint(mesh, luvs, k))))
            for k, i in enumerate(chosen)]
    return rows + [(name_for_mesh(mesh_id), TYPE, pack_luvs(luvs))]


def drawn_tint(mesh, luvs, material):
    """The mean vertex colour of the clusters `luvs` gives `material`: the hue its colormap lights the pattern
    with, so the light level keeps the fit's colour. None when no cluster takes it."""
    vertices = [np.arange(vb, vb + vc) for (vb, vc, *_), m in zip(mesh.clusters, luvs.materials) if m == material]
    if not vertices or mesh.rgb is None:
        return None
    return np.asarray(mesh.rgb, dtype=np.float64)[np.concatenate(vertices)].mean(axis=0)

