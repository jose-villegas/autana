#!/usr/bin/env python3
"""Generate main/apps/render_lab/<name>_mesh_generated.h/.c - Crytek Sponza as a
coloured, lit triangle mesh small enough for the board.

    python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab [--name NAME] [options]

Run from launcher/, in an environment with launcher/tools/mesh/requirements.txt
installed (see that folder's README). The model is Crytek Sponza from McGuire's
Computer Graphics Archive (casual-effects.com/data), fetched once into
launcher/tools/mesh/.cache and checked against its SHA-256; --sponza-dir points
at an already unpacked copy instead.

The device does no lighting. Everything a pixel's colour depends on is baked
here into one sRGB colour per vertex, with the mesh tools in launcher/tools/mesh:

1. Triangles no camera inside the building can see are dropped, and each
   material is decimated on its own to a share of its triangles.
2. The welded whole is split only where the sun's exposure changes along an
   edge, so a per-vertex light can carry shadow edges without tessellating
   evenly lit surfaces.
3. Albedo is sampled from the diffuse texture at the closest point of the
   original mesh; light is a sun with soft shadows plus sky light, both cast
   against the full-resolution original.
4. Triangles are grouped into an octree whose leaves are clusters.

What stays here is Sponza's own: which materials are thin sheets, how hard to
decimate each, where a camera may stand, and the lit_mesh_t output format.
The generator validates its own output before emitting anything.
"""

import argparse
import os
import pathlib
import sys

import numpy as np
import trimesh
from trimesh.ray.ray_pyembree import RayMeshIntersector

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools"))

from mesh import log  # noqa: E402
from mesh.decimate import decimate  # noqa: E402
from mesh.fetch import fetch_zip  # noqa: E402
from mesh.geometry import compact, corner_normals, weld, weld_keeping  # noqa: E402
from mesh.light import (  # noqa: E402
    drop_masked,
    light,
    merge_matching_colours,
    sample_albedo,
    sun_exposure,
    to_srgb8,
    visible_from_region,
)
from mesh.obj import load_mtl, load_obj, load_textures  # noqa: E402
from mesh.octree import build_octree, flatten_octree, node_bounds  # noqa: E402
from mesh.tessellate import adaptive_split  # noqa: E402

SPONZA_URL = "https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip"
SPONZA_SHA256 = "da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169a27c2770c"

POSITION_SCALE = 8  # int16 ticks per model unit
INT16_MAX = 32767
MAX_VERTICES = 65535  # uint16 indices
DOUBLE_SIDED = {"fabric_a", "fabric_c", "fabric_d", "fabric_e", "fabric_f", "fabric_g", "leaf", "chain", "Material__57"}
MASK_KEEP_ALPHA = 0.5
# (share of triangles kept by decimation relative to --keep, longest edge
# relative to --max-edge): the floor carries the sharpest shadows, the roof
# is seen only edge-on from inside.
MATERIAL_TUNING = {
    "floor": (1.0, 0.6),
    "roof": (0.5, 3.0),
    "arch": (3.0, 1.0),
    "ceiling": (3.0, 1.0),
    "column_a": (3.0, 1.0),
    "column_b": (1.5, 1.0),
    "column_c": (2.0, 1.0),
}
FABRIC_TUNING = (0.5, 1.0)
# A material this coarse is flat architecture: decimating it only loses shape.
DECIMATE_ABOVE = 1000
COLOUR_MERGE_STEP = 6  # sRGB levels; below what RGB565 shows
# Where a camera may stand: the atrium and its galleries, inside the outer
# walls. A triangle no point in here can see is dropped.
INTERIOR_LO = (-1400.0, 20.0, -620.0)
INTERIOR_HI = (1270.0, 1250.0, 550.0)


def tuning(name):
    if name.startswith("fabric_"):
        return FABRIC_TUNING
    return MATERIAL_TUNING.get(name, (1.0, 1.0))


def build_display_mesh(p, tri_v, tri_m, names, keep, max_edge, brightness, args):
    """Decimates each material on its own, then splits the light across the
    whole welded mesh at once: an edge two materials share is split for both
    or for neither, so no T-junction opens a crack between them."""
    positions, tris, labels, limits = [], [], [], []
    base = 0
    for m, name in enumerate(names):
        sel = tri_m == m
        if not np.any(sel):
            continue
        mp, mt = compact(p, tri_v[sel])
        mp, mt = weld(mp, mt)
        target = max(8, int(len(mt) * keep * tuning(name)[0]))
        mp, mt = decimate(mp, mt, target, DECIMATE_ABOVE)
        log(f"  {name:14s} {np.count_nonzero(sel):6d} -> {len(mt):5d} decimated")
        positions.append(mp)
        tris.append(mt + base)
        labels.append(np.full(len(mt), m))
        limits.append(np.full(len(mt), max_edge * tuning(name)[1]))
        base += len(mp)

    whole_p = np.concatenate(positions)
    whole_t = np.concatenate(tris)
    attrs = np.column_stack([np.concatenate(limits), np.concatenate(labels)])
    whole_p, whole_t = weld_keeping(whole_p, whole_t)
    before = len(whole_t)
    whole_p, whole_t, attrs = adaptive_split(whole_p, whole_t, attrs, brightness, args.min_edge, args.light_tolerance)
    log(f"  light split: {before} -> {len(whole_t)} triangles")
    parts = []
    for m, name in enumerate(names):
        sel = attrs[:, 1] == m
        if np.any(sel):
            mp, mt = compact(whole_p, whole_t[sel])
            parts.append((m, mp, mt))
    return parts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sponza-dir", help="an unpacked sponza.zip; fetched and cached when omitted")
    parser.add_argument("--keep", type=float, default=0.04, help="share of each material's triangles decimation keeps")
    parser.add_argument("--max-edge", type=float, default=900.0, help="longest edge kept, model units")
    parser.add_argument("--min-edge", type=float, default=60.0, help="shortest edge the light may split")
    parser.add_argument("--light-tolerance", type=float, default=0.3,
                        help="sun exposure (0..1) an edge midpoint may miss by before splitting")
    parser.add_argument("--sun", type=float, nargs=3, default=[-0.25, 1.0, 0.22], help="direction towards the sun")
    parser.add_argument("--sun-disc-deg", type=float, default=1.2)
    parser.add_argument("--sun-rays", type=int, default=8)
    parser.add_argument("--sky-rays", type=int, default=48)
    parser.add_argument("--sun-intensity", type=float, default=3.0)
    parser.add_argument("--sky-intensity", type=float, default=0.9)
    parser.add_argument("--ambient", type=float, default=0.06)
    parser.add_argument("--tonemap-white", type=float, default=0.35)
    parser.add_argument("--ray-offset", type=float, default=0.5)
    parser.add_argument("--leaf-triangles", type=int, default=160, help="most triangles an octree leaf holds")
    parser.add_argument("--max-depth", type=int, default=10)
    parser.add_argument("--out-dir", required=True, help="where <name>_mesh_generated.h and .c are written")
    parser.add_argument("--name", default="sponza", help="prefix of the files and of every symbol they define")
    parser.add_argument("--visibility-rounds", type=int, default=160)
    parser.add_argument("--leaf-keep", type=float, default=0.35, help="fraction of leaf triangles kept")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    rng = np.random.default_rng(args.seed)

    root = args.sponza_dir or str(fetch_zip(SPONZA_URL, SPONZA_SHA256, "crytek_sponza"))
    materials = load_mtl(os.path.join(root, "sponza.mtl"))
    p, uv, tri_v, tri_t, tri_m, names = load_obj(os.path.join(root, "sponza.obj"))
    log(f"loaded {len(p)} vertices, {len(tri_v)} triangles, {len(names)} materials")
    textures = load_textures(root, materials, names)
    tri_v, tri_t, tri_m = drop_masked(p, uv, tri_v, tri_t, tri_m, textures, MASK_KEEP_ALPHA)
    intersector = RayMeshIntersector(trimesh.Trimesh(p, tri_v, process=False))
    double = np.array([names[m] in DOUBLE_SIDED for m in tri_m])
    seen = visible_from_region(p, tri_v, double, intersector, args.visibility_rounds, rng, INTERIOR_LO, INTERIOR_HI)
    leaf = np.isin(tri_m, [i for i, n in enumerate(names) if n == "leaf"])
    seen &= ~leaf | (rng.random(len(tri_v)) < args.leaf_keep)
    shown_v, shown_m = tri_v[seen], tri_m[seen]

    log("decimating")
    def brightness(points, normals):
        return sun_exposure(points, normals, intersector, args)

    parts = build_display_mesh(p, shown_v, shown_m, names, args.keep, args.max_edge, brightness, args)

    all_pos, all_rgb, all_tris, all_double = [], [], [], []
    base = 0
    for m, mp, mt in parts:
        double = names[m] in DOUBLE_SIDED
        normals = corner_normals(mp, mt)
        corner_pos = mp[mt].reshape(-1, 3)
        corner_n = normals.reshape(-1, 3)
        # Weld corners that share a position and a normal: one colour each.
        key = np.concatenate([np.round(corner_pos * 16), np.round(corner_n * 64)], axis=1).astype(np.int64)
        _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
        vpos, vn = corner_pos[first], corner_n[first]
        vtris = inverse.reshape(-1, 3)
        edge_len = np.linalg.norm(vpos[vtris] - vpos[np.roll(vtris, 1, axis=1)], axis=2)
        spacing = np.zeros(len(vpos))
        count = np.zeros(len(vpos))
        np.add.at(spacing, vtris.reshape(-1), edge_len.reshape(-1))
        np.add.at(count, vtris.reshape(-1), 1)
        spacing /= np.maximum(count, 1)
        kd = materials.get(names[m], {}).get("Kd", (1.0, 1.0, 1.0))
        albedo = sample_albedo(vpos, spacing, m, p, uv, tri_v, tri_t, tri_m, textures, kd)
        radiance = light(vpos, vn, np.full(len(vpos), double), intersector, args, rng)
        vrgb = to_srgb8(albedo * radiance, args.tonemap_white)
        vpos, vrgb, vtris = merge_matching_colours(vpos, vrgb, vtris, COLOUR_MERGE_STEP)
        all_pos.append(vpos)
        all_rgb.append(vrgb)
        all_tris.append(vtris + base)
        all_double.append(np.full(len(vtris), int(double)))
        base += len(vpos)
        log(f"  lit {names[m]}: {len(vpos)} vertices")

    positions = np.concatenate(all_pos)
    rgb = np.concatenate(all_rgb)
    tris = np.concatenate(all_tris)
    tri_double = np.concatenate(all_double)

    root = build_octree(positions, tris, args.leaf_triangles, args.max_depth)
    clusters, nodes = flatten_octree(root, tri_double)
    out_pos, out_rgb, out_tris, out_clusters = [], [], [], []
    vbase = 0
    for double, members in clusters:
        ct = tris[members]
        used, local = np.unique(ct, return_inverse=True)
        out_pos.append(positions[used])
        out_rgb.append(rgb[used])
        out_tris.append(local.reshape(-1, 3) + vbase)
        q = np.round(positions[used] * POSITION_SCALE).astype(np.int64)
        tbase = sum(len(t) for t in out_tris[:-1])
        out_clusters.append((vbase, len(used), tbase, len(ct), q.min(axis=0), q.max(axis=0), double))
        vbase += len(used)

    q_pos = np.round(np.concatenate(out_pos) * POSITION_SCALE).astype(np.int64)
    q_rgb = np.concatenate(out_rgb)
    q_tris = np.concatenate(out_tris)
    node_bounds(nodes, out_clusters)
    validate(q_pos, q_rgb, q_tris, out_clusters, nodes)
    emit(args, q_pos, q_rgb, q_tris, out_clusters, nodes)
    log(f"emitted {len(q_pos)} vertices, {len(q_tris)} triangles, {len(out_clusters)} clusters, {len(nodes)} nodes")


def validate(pos, rgb, tris, clusters, nodes):
    assert len(pos) <= MAX_VERTICES, f"{len(pos)} vertices exceed uint16 indices"
    assert np.abs(pos).max() <= INT16_MAX, "a position does not fit int16"
    assert rgb.min() >= 0 and rgb.max() <= 255
    assert tris.min() >= 0 and tris.max() < len(pos)
    assert np.all((tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2]))
    next_v = next_t = 0
    for vbase, vcount, tbase, tcount, lo, hi, _ in clusters:
        assert vbase == next_v and tbase == next_t, "clusters must tile both arrays in order"
        ct = tris[tbase : tbase + tcount]
        assert ct.min() >= vbase and ct.max() < vbase + vcount, "a triangle reaches outside its cluster"
        cp = pos[vbase : vbase + vcount]
        assert np.all(cp >= lo) and np.all(cp <= hi)
        next_v, next_t = vbase + vcount, tbase + tcount
    assert next_v == len(pos) and next_t == len(tris)
    reached = np.zeros(len(clusters), dtype=np.int64)
    stack = [0]
    while stack:
        node = nodes[stack.pop()]
        assert node["count"] <= 255
        if node["leaf"]:
            reached[node["first"] : node["first"] + node["count"]] += 1
        else:
            stack.extend(range(node["first"], node["first"] + node["count"]))
    assert np.all(reached == 1), "every cluster must hang off exactly one leaf"
    assert len(nodes) <= 65535


def emit_rows(out, name, ctype, rows, per_line):
    print(f"static const {ctype} {name}[][{len(rows[0])}] = {{", file=out)
    for i in range(0, len(rows), per_line):
        chunk = rows[i : i + per_line]
        print("    " + " ".join("{" + ",".join(str(int(v)) for v in r) + "}," for r in chunk), file=out)
    print("};", file=out)


def banner(args, out):
    lines = [
        "GENERATED FILE - do not edit.",
        "",
        "    python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab \\",
        f"        --name {args.name} --keep {args.keep:g} --light-tolerance {args.light_tolerance:g} --min-edge {args.min_edge:g}",
        "",
        "Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in",
        "McGuire's Computer Graphics Archive, casual-effects.com/data.",
        "Decimated, lit by a sun and sky with baked shadows, one sRGB colour",
        "per vertex, clusters as the leaves of an octree. Other settings:",
        f"  --max-edge {args.max_edge:g} --sun {args.sun[0]:g} {args.sun[1]:g} {args.sun[2]:g}",
        f"  --sun-rays {args.sun_rays} --sky-rays {args.sky_rays} --leaf-triangles {args.leaf_triangles}",
    ]
    print("/*", file=out)
    for line in lines:
        print((" * " + line).rstrip(), file=out)
    print(" */", file=out)


def triple(v):
    return "{" + ", ".join(str(int(x)) for x in v) + "}"


def c_bool(v):
    return "true" if v else "false"


def emit(args, pos, rgb, tris, clusters, nodes):
    low, up = args.name, args.name.upper()
    header = f"{low}_mesh_generated.h"
    with open(os.path.join(args.out_dir, header), "w", newline="\n") as out:
        banner(args, out)
        print("#pragma once", file=out)
        print(file=out)
        print('#include "lit_mesh.h"', file=out)
        print(file=out)
        print(f"#define {up}_VERTEX_COUNT {len(pos)}", file=out)
        print(f"#define {up}_TRIANGLE_COUNT {len(tris)}", file=out)
        print(f"#define {up}_CLUSTER_COUNT {len(clusters)}", file=out)
        print(f"#define {up}_NODE_COUNT {len(nodes)}", file=out)
        print(f"#define {up}_POSITION_SCALE {POSITION_SCALE}", file=out)
        print(file=out)
        print(f"extern const lit_mesh_t {low}_mesh;", file=out)

    with open(os.path.join(args.out_dir, f"{low}_mesh_generated.c"), "w", newline="\n") as out:
        banner(args, out)
        print(f'#include "{header}"', file=out)
        print(file=out)
        emit_rows(out, f"{low}_positions", "int16_t", pos.tolist(), 8)
        print(file=out)
        emit_rows(out, f"{low}_colors", "uint8_t", rgb.tolist(), 10)
        print(file=out)
        emit_rows(out, f"{low}_triangles", "uint16_t", tris.tolist(), 8)
        print(file=out)
        print(f"static const lit_cluster_t {low}_clusters[] = {{", file=out)
        for vbase, vcount, tbase, tcount, lo, hi, double in clusters:
            print(f"    {{{vbase}, {vcount}, {tbase}, {tcount}, {triple(lo)}, {triple(hi)}, {c_bool(double)}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"static const lit_node_t {low}_nodes[] = {{", file=out)
        for n in nodes:
            print(f"    {{{triple(n['lo'])}, {triple(n['hi'])}, {n['first']}, {n['count']}, {c_bool(n['leaf'])}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"const lit_mesh_t {low}_mesh = {{", file=out)
        print(f"    {low}_positions, {low}_colors, {low}_triangles, {low}_clusters, {low}_nodes,", file=out)
        print(f"    {up}_VERTEX_COUNT, {up}_TRIANGLE_COUNT, {up}_CLUSTER_COUNT, {up}_NODE_COUNT,", file=out)
        print(f"    {up}_POSITION_SCALE,", file=out)
        print("};", file=out)


if __name__ == "__main__":
    main()
