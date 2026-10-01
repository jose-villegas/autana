#!/usr/bin/env python3
"""Bake a mesh from an import-settings TOML file.

    python launcher/tools/r3d/mesh_import.py SETTINGS.import.toml [--variant NAME]

Run from the repository root after installing tools/r3d/requirements.txt and
initializing third_party/upstream/meshoptimizer. The settings name the source,
its material rules, the bake options and the variants to bake, and a scene file
with the lights and the camera region. Every variant is baked unless one is named.
"""

import argparse
import os
import pathlib
import sys
import textwrap

import numpy as np
import trimesh
from trimesh.ray.ray_pyembree import RayMeshIntersector

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.fetch import fetch_zip  # noqa: E402
from r3d.geometry import compact, corner_normals, weld, weld_keeping  # noqa: E402
from r3d.import_settings import SettingsError, load_import_settings  # noqa: E402
from r3d.light import (  # noqa: E402
    drop_masked,
    face_colours,
    light,
    merge_matching_colours,
    sample_albedo,
    to_srgb8,
    visible_from_region,
)
from r3d.lit_mesh import write_lit_mesh  # noqa: E402
from r3d.obj import load_mtl, load_obj, load_textures  # noqa: E402
from r3d.simplify import densify, simplify  # noqa: E402


def banner_lines(settings, variant):
    relative = settings.path.relative_to(pathlib.Path(__file__).resolve().parents[3]).as_posix()
    return ["GENERATED FILE - do not edit.", "",
            f"    python launcher/tools/r3d/mesh_import.py {relative} --variant {variant.name}", "",
            *textwrap.wrap(settings.source["credit"], 72)]


def bake(settings, variant):
    rng = np.random.default_rng(settings.seed)
    lights = settings.scene.lights
    tonemap_white = settings.scene.tonemap_white
    root = fetch_zip(settings.source["url"], settings.source["sha256"], settings.source["cache"])
    obj_path = root / settings.source["path"]
    materials = load_mtl(obj_path.with_suffix(".mtl"))
    p, uv, tri_v, tri_t, tri_m, names = load_obj(obj_path)
    log(f"loaded {len(p)} vertices, {len(tri_v)} triangles, {len(names)} materials")
    textures = load_textures(obj_path.parent, materials, names)
    tri_v, tri_t, tri_m = drop_masked(p, uv, tri_v, tri_t, tri_m, textures, settings.mask_keep_alpha)
    intersector = RayMeshIntersector(trimesh.Trimesh(p, tri_v, process=False))
    double = np.array([names[material] in settings.double_sided for material in tri_m])
    seen = visible_from_region(p, tri_v, double, intersector, settings.visibility_rounds, rng, settings.scene.lo,
                               settings.scene.hi)
    leaf = np.isin(tri_m, [index for index, name in enumerate(names) if name == settings.leaf_material])
    seen &= ~leaf | (rng.random(len(tri_v)) < settings.leaf_keep)
    shown_v, shown_m = tri_v[seen], tri_m[seen]
    log("splitting evenly")
    wp, wt = weld_keeping(p, shown_v)
    dp, dt, dm = densify(wp, wt, shown_m, settings.dense_edge)
    parts = [(material, *compact(dp, dt[dm == material])) for material in range(len(names)) if np.any(dm == material)]
    all_pos, all_rgb, all_tris, all_double, all_mat = [], [], [], [], []
    base = 0
    for material, mp, mt in parts:
        double = names[material] in settings.double_sided
        normals = corner_normals(mp, mt)
        corner_pos, corner_n = mp[mt].reshape(-1, 3), normals.reshape(-1, 3)
        key = np.concatenate([np.round(corner_pos * 16), np.round(corner_n * 64)], axis=1).astype(np.int64)
        _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
        vpos, vn, vtris = corner_pos[first], corner_n[first], inverse.reshape(-1, 3)
        edge_len = np.linalg.norm(vpos[vtris] - vpos[np.roll(vtris, 1, axis=1)], axis=2)
        spacing, count = np.zeros(len(vpos)), np.zeros(len(vpos))
        np.add.at(spacing, vtris.reshape(-1), edge_len.reshape(-1))
        np.add.at(count, vtris.reshape(-1), 1)
        spacing /= np.maximum(count, 1)
        kd = materials.get(names[material], {}).get("Kd", (1.0, 1.0, 1.0))
        albedo = sample_albedo(vpos, spacing, material, p, uv, tri_v, tri_t, tri_m, textures, kd)
        radiance = light(vpos, vn, np.full(len(vpos), double), intersector, lights, settings.ray_offset, rng)
        vrgb = to_srgb8(albedo * radiance, tonemap_white)
        vpos, vrgb, vtris = merge_matching_colours(vpos, vrgb, vtris, settings.colour_merge_step)
        all_pos.append(vpos)
        all_rgb.append(vrgb)
        all_tris.append(vtris + base)
        all_double.append(np.full(len(vtris), int(double)))
        all_mat.append(np.full(len(vtris), material))
        base += len(vpos)
        log(f"  lit {names[material]}: {len(vpos)} vertices")
    positions, rgb, tris = np.concatenate(all_pos), np.concatenate(all_rgb), np.concatenate(all_tris)
    tri_double, tri_mat = np.concatenate(all_double), np.concatenate(all_mat)
    props = [(frozenset(index for index, name in enumerate(names) if name in settings.props), settings.props_share)]
    positions, rgb, tris, tri_mat = simplify(positions, rgb.astype(np.float64), tris, tri_mat, variant.triangles, props,
                                             seal_seams=settings.seal_seams, position_scale=settings.position_scale)
    rgb = np.clip(np.round(rgb), 0, 255).astype(np.int64)
    tri_double = np.isin(tri_mat, [index for index, name in enumerate(names) if name in settings.double_sided]).astype(np.int64)
    face_rgb = None
    if variant.flat:
        samples, sample_min, sample_max, sample_area = variant.face_samples
        double_materials = {index for index, name in enumerate(names) if name in settings.double_sided}

        def albedo_of(centres, spacing, material):
            kd = materials.get(names[material], {}).get("Kd", (1.0, 1.0, 1.0))
            return sample_albedo(centres, spacing, material, p, uv, tri_v, tri_t, tri_m, textures, kd)

        face_rgb = face_colours(positions, tris, tri_mat, range(len(names)), double_materials, albedo_of, intersector,
                                lights, settings.ray_offset, tonemap_white, samples, settings.flat_sky_rays,
                                sample_max, sample_area, sample_min)
    mesh = write_lit_mesh(settings.out_dir, variant.name, positions, None if variant.flat else rgb, tris, tri_double,
                          banner_lines(settings, variant), position_scale=settings.position_scale, face_rgb=face_rgb)
    log(f"emitted {len(mesh.pos)} vertices, {len(mesh.tris)} triangles, {len(mesh.clusters)} clusters, {len(mesh.nodes)} nodes")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("settings", help="one .import.toml file")
    parser.add_argument("--variant", help="bake only the variant with this name")
    args = parser.parse_args(argv)
    try:
        settings = load_import_settings(args.settings)
        variants = [variant for variant in settings.variants if args.variant in (None, variant.name)]
        if not variants:
            raise SettingsError(f"no variant named {args.variant!r}")
    except SettingsError as error:
        parser.error(str(error))
    for variant in variants:
        log(f"variant {variant.name}")
        bake(settings, variant)
    return 0


if __name__ == "__main__":
    sys.exit(main())
