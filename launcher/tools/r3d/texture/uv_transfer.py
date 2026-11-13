"""LUVS v2 mesh texturing: a baked mesh's UVs, carried over from the source's, and each cluster's charts.
The layout constants belong to render/texture/r3d_texture.h.

A cluster takes up to CHARTS planar charts. Each is a textured material and the source triangle under the largest of
the cluster's triangles of that material no earlier chart took; chart 0's material is the one most of the cluster's
area lies on, the next chart's the one most of what is left lies on. Every vertex of the cluster takes a UV per chart
from that triangle's plane, so a chart's UVs agree at every vertex and a planar wall keeps the source's mapping
exactly. A triangle draws with the first chart whose material it lies on and whose plane it faces; the rest of the
cluster draws plain, so a texture ends where its material does, not at a cluster's edge."""
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
CHARTS = read_define(OWNER, "R3D_TEXTURE_CHARTS")
PLAIN = read_define(OWNER, "R3D_TEXTURE_PLAIN")
TRIANGLES_PER_BYTE = read_define(OWNER, "R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE")
CODE_BITS = read_define(OWNER, "R3D_TEXTURE_UVS_CODE_BITS")
HEADER_FIELDS = (("VERSION", "H"), ("MATERIAL_COUNT", "H"), ("VERTEX_COUNT", "I"), ("CLUSTER_COUNT", "I"),
                 ("TRIANGLE_COUNT", "I"), ("UV_COUNT", "I"), ("UVS_OFF", "I"), ("ORIGINS_OFF", "I"),
                 ("SHIFTS_OFF", "I"), ("CLUSTERS_OFF", "I"), ("TRIANGLES_OFF", "I"))
HEADER = checked_struct(OWNER, "R3D_TEXTURE_UVS", HEADER_FIELDS)
PAIR = struct.Struct("<2h")
INT16 = np.iinfo(np.int16)
SHIFT = np.dtype("<i4")
# A baked triangle takes its nearest source triangle's material, and draws textured on a chart, when their normals
# agree within this.
NORMAL_DEGREES = 10.0


@dataclasses.dataclass
class Luvs:
    uvs: np.ndarray        # (uv count, 2) int16, 1/16 texel of level 0 from its chart's origin: each vertex's on
                           # chart 0, then each cluster's vertices' on chart 1 for the clusters with one
    origins: np.ndarray    # (clusters, CHARTS, 2) int16, whole texture sides
    shifts: np.ndarray     # (clusters,) int32, a vertex's UV on its cluster's chart 1 is uvs[vertex + shift]
    materials: np.ndarray  # (clusters, CHARTS) uint8, an index into the materials or NONE; a NONE chart has none after
    codes: np.ndarray      # (triangles,) uint8, PLAIN or 1 + the chart it draws with
    vertex_count: int
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
    agree = np.cos(np.radians(NORMAL_DEGREES))
    facing = (baked_normal * source_normal[near]).sum(axis=1) >= agree
    slot = np.full(len(source.names), NONE)
    slot[list(material_ids)] = np.arange(len(material_ids))
    material = np.where(facing, slot[np.asarray(source.tri_m)[near]], NONE)
    uvs = [np.zeros((len(points), 2), dtype=np.int16)]
    uv_count = len(points)
    origins = np.zeros((len(mesh.clusters), CHARTS, 2), dtype=np.int16)
    shifts = np.zeros(len(mesh.clusters), dtype=np.int32)
    materials = np.full((len(mesh.clusters), CHARTS), NONE, dtype=np.uint8)
    codes = np.full(len(tris), PLAIN, dtype=np.uint8)
    for c, (vb, vc, tb, tc, *_box) in enumerate(mesh.clusters):
        own, own_area, own_code = material[tb:tb + tc], area[tb:tb + tc], codes[tb:tb + tc]
        for chart in range(CHARTS):
            found = cluster_chart(points[vb:vb + vc], own, own_area, own_code == PLAIN, near[tb:tb + tc],
                                  source_corners, source, sizes)
            if found is None:
                break
            k, plane, uv = found
            size = sizes[k]
            origin = np.floor(uv.mean(axis=0) / size)
            stored = np.rint((uv - origin * size) * (1 << UV_SHIFT))
            if stored.min() < INT16.min or stored.max() > INT16.max or np.abs(origin).max() > INT16.max:
                raise ValueError(f"cluster {c} spans more texels than an int16 UV holds; lower the texture size")
            if chart == 0:
                uvs[0][vb:vb + vc] = stored
            else:
                shifts[c] = uv_count - vb
                uvs.append(stored.astype(np.int16))
                uv_count += vc
            origins[c, chart], materials[c, chart] = origin, k
            on_plane = baked_normal[tb:tb + tc] @ source_normal[plane] >= agree
            own_code[(own_code == PLAIN) & (own == k) & on_plane] = chart + 1
    return Luvs(np.concatenate(uvs), origins, shifts, materials, codes, len(points), len(material_ids))


def cluster_chart(points, own, own_area, left, near, source_corners, source, sizes):
    """The next chart of a cluster whose `points` are its vertices and whose triangles lie on materials `own` with
    areas `own_area` and nearest source triangles `near`, of those `left` no chart took: (material, source triangle,
    the vertices' texel UVs), or None when no textured material is left."""
    shares = np.bincount(own[left & (own != NONE)], own_area[left & (own != NONE)], minlength=1)
    if shares.max() <= 0.0:
        return None
    k = int(shares.argmax())
    texels = texel_uvs(source, sizes[k])
    for t in np.argsort(-np.where(left & (own == k), own_area, -1.0)):
        if not left[t] or own[t] != k:
            break
        uv = plane_uv(points, source_corners[near[t]], texels[near[t]])
        if uv is not None:
            return k, near[t], uv
    return None


def pack_codes(codes):
    """Each triangle's code, CODE_BITS of it, triangle t at bit CODE_BITS × (t % TRIANGLES_PER_BYTE) of byte
    t / TRIANGLES_PER_BYTE."""
    padded = np.full(-(-len(codes) // TRIANGLES_PER_BYTE) * TRIANGLES_PER_BYTE, PLAIN, dtype=np.uint32)
    padded[:len(codes)] = codes
    shift = np.arange(TRIANGLES_PER_BYTE, dtype=np.uint32) * CODE_BITS
    return (padded.reshape(-1, TRIANGLES_PER_BYTE) << shift).sum(axis=1).astype(np.uint8)


def unpack_codes(packed, count):
    shift = np.arange(TRIANGLES_PER_BYTE, dtype=np.uint8) * CODE_BITS
    return ((packed[:, None] >> shift) & ((1 << CODE_BITS) - 1)).ravel()[:count].astype(np.uint8)


def pack_luvs(luvs):
    """The LUVS entry: header, UVs, origins, shifts, cluster materials, then triangle codes."""
    uvs_off = HEADER.size
    origins_off = uvs_off + len(luvs.uvs) * PAIR.size
    shifts_off = origins_off + luvs.origins.size * INT16.bits // 8
    clusters_off = shifts_off + luvs.shifts.size * SHIFT.itemsize
    triangles_off = clusters_off + luvs.materials.size
    return (HEADER.pack(VERSION, luvs.material_count, luvs.vertex_count, len(luvs.origins), len(luvs.codes),
                        len(luvs.uvs), uvs_off, origins_off, shifts_off, clusters_off, triangles_off)
            + luvs.uvs.astype("<i2").tobytes() + luvs.origins.astype("<i2").tobytes()
            + luvs.shifts.astype(SHIFT).tobytes() + luvs.materials.astype(np.uint8).tobytes()
            + pack_codes(luvs.codes).tobytes())


def decode(entry):
    """The Luvs an LUVS entry holds."""
    (version, count, vertices, clusters, triangles, uv_count, uvs_off, origins_off, shifts_off, clusters_off,
     triangles_off) = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ValueError("LUVS version is invalid")
    uvs = np.frombuffer(entry, "<i2", uv_count * 2, uvs_off).reshape(-1, 2)
    origins = np.frombuffer(entry, "<i2", clusters * CHARTS * 2, origins_off).reshape(-1, CHARTS, 2)
    shifts = np.frombuffer(entry, SHIFT, clusters, shifts_off)
    materials = np.frombuffer(entry, np.uint8, clusters * CHARTS, clusters_off).reshape(-1, CHARTS)
    size = -(-triangles // TRIANGLES_PER_BYTE)
    if triangles_off + size != len(entry):
        raise ValueError("LUVS sections do not match the entry size")
    codes = unpack_codes(np.frombuffer(entry, np.uint8, size, triangles_off), triangles)
    return Luvs(uvs.copy(), origins.copy(), shifts.astype(np.int32), materials.copy(), codes, vertices, count)


def name_for_mesh(mesh):
    return sibling_name(mesh, NAME_SUFFIX)


def texture_name(mesh, index):
    """The pack id of the mesh's `index`th material texture: a sibling of the mesh, as a material's own name may
    not fit a pack name."""
    return sibling_name(mesh, f"{TEXTURE_SUFFIX}{index}")


def textured_entries(mesh, mesh_id, source, patterns, size):
    """The pack rows (name, type, bytes) texturing `mesh_id`: one LTEX per source material matching `patterns` that
    has a texture, and the mesh's LUVS."""
    textured = [i for i, texture in enumerate(source.textures) if texture is not None]
    names = [source.names[i] for i in textured]
    for pattern in patterns:
        if not matched(names, [pattern]):
            raise ValueError(f"no textured source material matches {pattern!r}")
    chosen = [textured[k] for k in matched(names, patterns)]
    luvs = cluster_uvs(mesh, source, chosen, [size] * len(chosen))
    rows = [(texture_name(mesh_id, k), texture_asset.TYPE,
             texture_asset.pack_ltex(texture_asset.material_texture(source.textures[i].levels[0], size,
                                                                    drawn_tint(mesh, luvs, k))))
            for k, i in enumerate(chosen)]
    return rows + [(name_for_mesh(mesh_id), TYPE, pack_luvs(luvs))]


def drawn_tint(mesh, luvs, material):
    """The mean vertex colour of the triangles `luvs` draws with `material`: the hue its colormap lights the pattern
    with, so the light level keeps the fit's colour. None when no triangle takes it."""
    if mesh.rgb is None:
        return None
    cluster = np.zeros(len(luvs.codes), dtype=np.int64)
    for c, (_vb, _vc, tb, tc, *_box) in enumerate(mesh.clusters):
        cluster[tb:tb + tc] = c
    drawn = np.nonzero(luvs.codes != PLAIN)[0]
    drawn = drawn[luvs.materials[cluster[drawn], luvs.codes[drawn] - 1] == material]
    if not len(drawn):
        return None
    return np.asarray(mesh.rgb, dtype=np.float64)[np.asarray(mesh.tris)[drawn].ravel()].mean(axis=0)
