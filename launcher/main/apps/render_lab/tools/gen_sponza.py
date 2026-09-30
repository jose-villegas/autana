#!/usr/bin/env python3
"""Generate main/apps/render_lab/<name>_mesh_generated.h/.c - Crytek Sponza as a
coloured, lit triangle mesh small enough for the board.

    python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab [--name NAME] [options]

Run from launcher/, in an environment with launcher/tools/r3d/requirements.txt
installed and the meshoptimizer submodule checked out:

    git submodule update --init ../third_party/upstream/meshoptimizer
    python -m venv tools/r3d/.cache/venv
    tools/r3d/.cache/venv/Scripts/python -m pip install -r tools/r3d/requirements.txt

(bin/python on Linux). Each generated file's banner records the
exact command that produced it.

The model is Crytek Sponza from McGuire's Computer Graphics Archive
(casual-effects.com/data), fetched once into launcher/tools/r3d/.cache and
checked against its SHA-256; --sponza-dir points at an already unpacked copy
instead.

The device does no lighting. Everything a pixel's colour depends on is baked
here into one sRGB colour per vertex, with the mesh tools in launcher/tools/r3d:

1. Triangles no camera inside the building can see are dropped.
2. The rest is welded, split evenly (--dense-edge) and lit per vertex:
   albedo sampled from the diffuse texture at the closest point of the
   original mesh, light a sun with soft shadows plus sky light, both cast
   against the full-resolution original.
3. One meshoptimizer pass simplifies the whole lit mesh to --triangles,
   with colour as an attribute so shadow edges and texture detail hold
   vertices, and small props keep a reserved share (--props-share).
   --simplifier quadric runs per-material decimation, then splits where the
   sun's exposure changes, for comparison.
4. The triangles become meshlets under an octree (tools/r3d/lit_mesh.py).

What is here is Sponza's own: which materials are thin sheets, how hard to
decimate each and where a camera may stand. tools/r3d/lit_mesh.py clusters,
quantizes and validates the result before emitting anything.
"""

import argparse
import os
import pathlib
import sys

import numpy as np
import trimesh
from trimesh.ray.ray_pyembree import RayMeshIntersector

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools"))

from r3d import log  # noqa: E402
from r3d.decimate import decimate  # noqa: E402
from r3d.fetch import fetch_zip  # noqa: E402
from r3d.geometry import compact, corner_normals, weld, weld_keeping  # noqa: E402
from r3d.light import (  # noqa: E402
    drop_masked,
    light,
    merge_matching_colours,
    sample_albedo,
    sun_exposure,
    to_srgb8,
    visible_from_region,
)
from r3d.lit_mesh import write_lit_mesh  # noqa: E402
from r3d.obj import load_mtl, load_obj, load_textures  # noqa: E402
from r3d.simplify import densify, simplify  # noqa: E402
from r3d.tessellate import adaptive_split  # noqa: E402

SPONZA_URL = "https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip"
SPONZA_SHA256 = "da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169a27c2770c"

POSITION_SCALE = 8  # int16 ticks per model unit
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
# Small detailed props: a global simplifier spends its budget on the large
# surfaces around them, so they hold a reserved share of it instead.
PROPS = {"vase", "vase_round", "vase_hanging", "flagpole", "chain", "leaf", "Material__57", "Material__298",
         "Material__25", "Material__47"}


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
    parser.add_argument("--out-dir", required=True, help="where <name>_mesh_generated.h and .c are written")
    parser.add_argument("--name", default="sponza", help="prefix of the files and of every symbol they define")
    parser.add_argument("--visibility-rounds", type=int, default=160)
    parser.add_argument("--leaf-keep", type=float, default=0.35, help="fraction of leaf triangles kept")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--simplifier", choices=("meshopt", "quadric"), default="meshopt",
                        help="meshopt: split evenly, bake, one colour-aware pass (--triangles); "
                        "quadric: per-material decimation then light-driven splits (--keep, --light-tolerance)")
    parser.add_argument("--triangles", type=int, default=17381, help="meshopt: the triangle budget")
    parser.add_argument("--dense-edge", type=float, default=45.0, help="meshopt: longest edge before baking")
    parser.add_argument("--npz", help="also save the final mesh before clustering, for evaluation")
    parser.add_argument("--no-repair", action="store_true",
                        help="meshopt: import the mesh as it is, without the watertight option "
                        "(joined touching pieces, light regularizing, merged colour seams)")
    parser.add_argument("--props-share", type=float, default=0.3, help="meshopt: budget share held for props")
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

    if args.simplifier == "meshopt":
        log("splitting evenly")
        wp, wt = weld_keeping(p, shown_v)
        dp, dt, dm = densify(wp, wt, shown_m, args.dense_edge)
        parts = [(m, *compact(dp, dt[dm == m])) for m in range(len(names)) if np.any(dm == m)]
    else:
        log("decimating")

        def brightness(points, normals):
            return sun_exposure(points, normals, intersector, args)

        parts = build_display_mesh(p, shown_v, shown_m, names, args.keep, args.max_edge, brightness, args)

    all_pos, all_rgb, all_tris, all_double, all_mat = [], [], [], [], []
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
        all_mat.append(np.full(len(vtris), m))
        base += len(vpos)
        log(f"  lit {names[m]}: {len(vpos)} vertices")

    positions = np.concatenate(all_pos)
    rgb = np.concatenate(all_rgb)
    tris = np.concatenate(all_tris)
    tri_double = np.concatenate(all_double)
    watertight = args.simplifier == "meshopt" and not args.no_repair
    if args.simplifier == "meshopt":
        props = [(frozenset(i for i, n in enumerate(names) if n in PROPS), args.props_share)]
        positions, rgb, tris, tri_mat = simplify(positions, rgb.astype(np.float64), tris, np.concatenate(all_mat),
                                                 args.triangles, props, watertight=watertight,
                                                 position_scale=POSITION_SCALE)
        rgb = np.clip(np.round(rgb), 0, 255).astype(np.int64)
        tri_double = np.isin(tri_mat, [i for i, n in enumerate(names) if n in DOUBLE_SIDED]).astype(np.int64)
    if args.npz:
        np.savez_compressed(args.npz, pos=positions, rgb=rgb, tris=tris, double=tri_double)

    mesh = write_lit_mesh(args.out_dir, args.name, positions, rgb, tris, tri_double, banner_lines(args),
                          position_scale=POSITION_SCALE, watertight=watertight)
    log(f"emitted {len(mesh.pos)} vertices, {len(mesh.tris)} triangles, {len(mesh.clusters)} clusters, "
        f"{len(mesh.nodes)} nodes")


def banner_lines(args):
    return [
        "GENERATED FILE - do not edit.",
        "",
        "    python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab \\",
        (f"        --name {args.name} --simplifier meshopt --triangles {args.triangles}"
         f" --props-share {args.props_share:g} --dense-edge {args.dense_edge:g}"
         + (" --no-repair" if args.no_repair else "")
         if args.simplifier == "meshopt" else
         f"        --name {args.name} --simplifier quadric --keep {args.keep:g}"
         f" --light-tolerance {args.light_tolerance:g} --min-edge {args.min_edge:g}"),
        "",
        "Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in",
        "McGuire's Computer Graphics Archive, casual-effects.com/data.",
        "Simplified, lit by a sun and sky with baked shadows, one sRGB colour",
        "per vertex. Other settings:",
        f"  --max-edge {args.max_edge:g} --sun {args.sun[0]:g} {args.sun[1]:g} {args.sun[2]:g}",
        f"  --sun-rays {args.sun_rays} --sky-rays {args.sky_rays}",
    ]


if __name__ == "__main__":
    main()
