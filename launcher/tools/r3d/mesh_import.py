#!/usr/bin/env python3
"""Bake the meshes a scene file places.

    python launcher/tools/r3d/mesh_import.py SCENE.scene.toml [--mesh NAME]

Run from the repository root after installing tools/r3d/requirements.txt and
initializing third_party/upstream/meshoptimizer. Each mesh renderer names an
import file, which brings the mesh in as authored unless it opts into
processing steps; the steps that light read this scene's lights, camera region
and exposure. Every renderer's mesh is baked unless one is named.
"""

import argparse
import pathlib
import sys
import textwrap
from types import SimpleNamespace

import numpy as np
import trimesh
from trimesh.ray.ray_pyembree import RayMeshIntersector

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.fetch import fetch_zip  # noqa: E402
from r3d.geometry import compact, corner_normals, weld_keeping  # noqa: E402
from r3d.import_settings import SettingsError, load_scene  # noqa: E402
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

REPO = pathlib.Path(__file__).resolve().parents[3]


def banner_lines(scene, settings, variant):
    return ["GENERATED FILE - do not edit.", "",
            f"    python launcher/tools/r3d/mesh_import.py {scene.path.relative_to(REPO).as_posix()} --mesh {variant.name}", "",
            *textwrap.wrap(settings.source["credit"], 72)]


def vertex_spacing(vpos, vtris):
    """Mean length of the edges meeting at each vertex, for the texture's level of detail."""
    edge_len = np.linalg.norm(vpos[vtris] - vpos[np.roll(vtris, 1, axis=1)], axis=2)
    spacing, count = np.zeros(len(vpos)), np.zeros(len(vpos))
    np.add.at(spacing, vtris.reshape(-1), edge_len.reshape(-1))
    np.add.at(count, vtris.reshape(-1), 1)
    return spacing / np.maximum(count, 1)


def load_source(settings):
    root = fetch_zip(settings.source["url"], settings.source["sha256"], settings.source["cache"])
    obj_path = root / settings.source["path"]
    materials = load_mtl(obj_path.with_suffix(".mtl"))
    p, uv, tri_v, tri_t, tri_m, names = load_obj(obj_path)
    log(f"loaded {len(p)} vertices, {len(tri_v)} triangles, {len(names)} materials")
    textures = load_textures(obj_path.parent, materials, names)
    return SimpleNamespace(p=p, uv=uv, tri_v=tri_v, tri_t=tri_t, tri_m=tri_m, names=names, materials=materials,
                           textures=textures)


def albedo_at(src, points, spacing, material):
    kd = src.materials.get(src.names[material], {}).get("Kd", (1.0, 1.0, 1.0))
    return sample_albedo(points, spacing, material, src.p, src.uv, src.tri_v, src.tri_t, src.tri_m, src.textures, kd)


def shade_lit(src, settings, scene, material, mp, mt, double, intersector, rng):
    """Vertices split along creases and lit on their own normals, near colours merged."""
    normals = corner_normals(mp, mt)
    corner_pos, corner_n = mp[mt].reshape(-1, 3), normals.reshape(-1, 3)
    key = np.concatenate([np.round(corner_pos * 16), np.round(corner_n * 64)], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    vpos, vn, vtris = corner_pos[first], corner_n[first], inverse.reshape(-1, 3)
    albedo = albedo_at(src, vpos, vertex_spacing(vpos, vtris), material)
    radiance = light(vpos, vn, np.full(len(vpos), double), intersector, scene.lights, settings.light.ray_offset, rng)
    vrgb = to_srgb8(albedo * radiance, scene.exposure)
    return merge_matching_colours(vpos, vrgb, vtris, settings.light.colour_merge_step)


def shade_unlit(src, material, mp, mt):
    """The material's albedo, with no light: the colour the source authored."""
    return mp, to_srgb8(albedo_at(src, mp, vertex_spacing(mp, mt), material), 0.0), mt


def bake(scene, renderer):
    settings, variant = renderer.settings, renderer.variant
    rng = np.random.default_rng(settings.seed)
    src = load_source(settings)
    scale = {} if settings.position_scale is None else {"position_scale": settings.position_scale}
    tri_v, tri_t, tri_m = src.tri_v, src.tri_t, src.tri_m
    if settings.alpha_keep is not None:
        tri_v, tri_t, tri_m = drop_masked(src.p, src.uv, tri_v, tri_t, tri_m, src.textures, settings.alpha_keep)
        src.tri_v, src.tri_t, src.tri_m = tri_v, tri_t, tri_m
    intersector = None
    if settings.visibility or settings.light:
        intersector = RayMeshIntersector(trimesh.Trimesh(src.p, tri_v, process=False))
    double_names = settings.double_sided
    if settings.visibility:
        visibility = settings.visibility
        double = np.array([src.names[material] in double_names for material in tri_m])
        seen = visible_from_region(src.p, tri_v, double, intersector, visibility.rounds, rng, *scene.region)
        if visibility.thin_material is not None:
            thin = np.isin(tri_m, [index for index, name in enumerate(src.names) if name == visibility.thin_material])
            seen &= ~thin | (rng.random(len(tri_v)) < visibility.thin_keep)
        shown_v, shown_m = tri_v[seen], tri_m[seen]
    else:
        shown_v, shown_m = tri_v, tri_m
    if settings.simplify:
        log("splitting evenly")
        wp, wt = weld_keeping(src.p, shown_v)
        dp, dt, dm = densify(wp, wt, shown_m, settings.simplify.dense_edge)
        parts = [(material, *compact(dp, dt[dm == material])) for material in range(len(src.names)) if np.any(dm == material)]
    else:
        parts = [(material, *compact(src.p, shown_v[shown_m == material])) for material in range(len(src.names))
                 if np.any(shown_m == material)]
    all_pos, all_rgb, all_tris, all_double, all_mat = [], [], [], [], []
    base = 0
    for material, mp, mt in parts:
        double = src.names[material] in double_names
        if settings.light:
            vpos, vrgb, vtris = shade_lit(src, settings, scene, material, mp, mt, double, intersector, rng)
        else:
            vpos, vrgb, vtris = shade_unlit(src, material, mp, mt)
        all_pos.append(vpos)
        all_rgb.append(vrgb)
        all_tris.append(vtris + base)
        all_double.append(np.full(len(vtris), int(double)))
        all_mat.append(np.full(len(vtris), material))
        base += len(vpos)
        log(f"  {src.names[material]}: {len(vpos)} vertices")
    positions, rgb, tris = np.concatenate(all_pos), np.concatenate(all_rgb), np.concatenate(all_tris)
    tri_double, tri_mat = np.concatenate(all_double), np.concatenate(all_mat)
    if settings.simplify:
        steps = settings.simplify
        props = [(frozenset(index for index, name in enumerate(src.names) if name in steps.props), steps.props_share)]
        positions, rgb, tris, tri_mat = simplify(positions, rgb.astype(np.float64), tris, tri_mat, variant.triangles, props,
                                                 seal_seams=steps.seal_seams, **scale)
        rgb = np.clip(np.round(rgb), 0, 255).astype(np.int64)
        tri_double = np.isin(tri_mat, [index for index, name in enumerate(src.names) if name in double_names]).astype(np.int64)
    face_rgb = None
    if variant.face_samples:
        samples, sample_min, sample_max, sample_area = variant.face_samples
        double_materials = {index for index, name in enumerate(src.names) if name in double_names}
        face_rgb = face_colours(positions, tris, tri_mat, range(len(src.names)), double_materials,
                                lambda centres, spacing, material: albedo_at(src, centres, spacing, material), intersector,
                                scene.lights, settings.light.ray_offset, scene.exposure, samples, settings.light.flat_sky_rays,
                                sample_max, sample_area, sample_min)
    mesh = write_lit_mesh(settings.out_dir, variant.name, positions, None if variant.face_samples else rgb, tris, tri_double,
                          banner_lines(scene, settings, variant), face_rgb=face_rgb, **scale)
    log(f"emitted {len(mesh.pos)} vertices, {len(mesh.tris)} triangles, {len(mesh.clusters)} clusters, {len(mesh.nodes)} nodes")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene", help="one .scene.toml file")
    parser.add_argument("--mesh", help="bake only the mesh with this name")
    args = parser.parse_args(argv)
    try:
        scene = load_scene(args.scene)
        renderers = [renderer for renderer in scene.renderers if args.mesh in (None, renderer.variant.name)]
        if not renderers:
            raise SettingsError(f"no mesh named {args.mesh!r}")
    except SettingsError as error:
        parser.error(str(error))
    for renderer in renderers:
        log(f"mesh {renderer.variant.name}")
        bake(scene, renderer)
    return 0


if __name__ == "__main__":
    sys.exit(main())
