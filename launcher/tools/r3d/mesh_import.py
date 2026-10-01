#!/usr/bin/env python3
"""Bake meshes from an import file, or from the scene file that places them.

    python launcher/tools/r3d/mesh_import.py PATH [--mesh NAME]

PATH is an .import.toml, which bakes alone unless one of its steps needs a
scene (light, visibility), or a .scene.toml, which bakes every mesh it places
with its own lights, camera region and tone map. Run from the repository root
after installing tools/r3d/requirements.txt and initializing
third_party/upstream/meshoptimizer. Every mesh is baked unless one is named.
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
from r3d.import_settings import SettingsError, load_import_settings, load_scene  # noqa: E402
from r3d.light import (  # noqa: E402
    drop_masked,
    encode_srgb8,
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


def banner_lines(origin, settings, variant):
    return ["GENERATED FILE - do not edit.", "",
            f"    python launcher/tools/r3d/mesh_import.py {origin.relative_to(REPO).as_posix()} --mesh {variant.name}", "",
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
    vrgb = to_srgb8(albedo * radiance, scene.tonemap_white)
    return merge_matching_colours(vpos, vrgb, vtris, settings.light.colour_merge_step)


def shade_unlit(src, material, mp, mt):
    """The material's albedo, with no light: the colour the source authored."""
    return mp, encode_srgb8(albedo_at(src, mp, vertex_spacing(mp, mt), material)), mt


def bake_geometry(settings, variant, scene):
    """Everything a mesh needs before its colours are final: the source, its
    ray intersector and the simplified geometry with the colours a smooth bake
    keeps. `scene` is None for an import that needs none."""
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
    seen = np.ones(len(tri_v), dtype=bool)
    if settings.visibility:
        double = np.array([src.names[material] in double_names for material in tri_m])
        seen = visible_from_region(src.p, tri_v, double, intersector, settings.visibility.rounds, rng, *scene.region)
    if settings.thin:
        thin = np.isin(tri_m, [index for index, name in enumerate(src.names) if name == settings.thin.material])
        seen &= ~thin | (rng.random(len(tri_v)) < settings.thin.keep)
    shown_v, shown_m = tri_v[seen], tri_m[seen]
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
    return SimpleNamespace(src=src, intersector=intersector, positions=positions, rgb=rgb, tris=tris, tri_double=tri_double,
                           tri_mat=tri_mat, scale=scale)


def flat_colours(settings, scene, geometry, face_samples, **knobs):
    """One colour per triangle of `geometry` for a flat variant's
    (samples, min, max, area) options. `knobs` are face_colours' own: sky_rays,
    placement and sun_centre."""
    src = geometry.src
    samples, sample_min, sample_max, sample_area = face_samples
    double_materials = {index for index, name in enumerate(src.names) if name in settings.double_sided}
    knobs.setdefault("sky_rays", settings.light.flat_sky_rays)
    return face_colours(geometry.positions, geometry.tris, geometry.tri_mat, range(len(src.names)), double_materials,
                        lambda centres, spacing, material: albedo_at(src, centres, spacing, material), geometry.intersector,
                        scene.lights, settings.light.ray_offset, scene.tonemap_white, samples, max_samples=sample_max,
                        sample_area=sample_area, min_samples=sample_min, **knobs)


def bake(settings, variant, scene, origin):
    """Bakes one mesh. `scene` is None for an import that needs none; `origin`
    is the file the command was run on."""
    geometry = bake_geometry(settings, variant, scene)
    positions, rgb, tris, scale = geometry.positions, geometry.rgb, geometry.tris, geometry.scale
    face_rgb = flat_colours(settings, scene, geometry, variant.face_samples) if variant.face_samples else None
    mesh = write_lit_mesh(settings.out_dir, variant.name, positions, None if variant.face_samples else rgb, tris, geometry.tri_double,
                          banner_lines(origin, settings, variant), face_rgb=face_rgb, **scale)
    log(f"emitted {len(mesh.pos)} vertices, {len(mesh.tris)} triangles, {len(mesh.clusters)} clusters, {len(mesh.nodes)} nodes")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", help="an .import.toml or a .scene.toml file")
    parser.add_argument("--mesh", help="bake only the mesh with this name")
    args = parser.parse_args(argv)
    path = pathlib.Path(args.path).resolve()
    try:
        if path.name.endswith(".scene.toml"):
            scene = load_scene(path)
            jobs = [(item.settings, item.variant, scene) for item in scene.renderers]
        elif path.name.endswith(".import.toml"):
            settings = load_import_settings(path)
            if settings.scene_dependent:
                raise SettingsError(f"{path.name} needs a scene: run the .scene.toml that places it")
            jobs = [(settings, variant, None) for variant in settings.variants]
        else:
            raise SettingsError("PATH must end in .import.toml or .scene.toml")
        jobs = [job for job in jobs if args.mesh in (None, job[1].name)]
        if not jobs:
            raise SettingsError(f"no mesh named {args.mesh!r}")
    except SettingsError as error:
        parser.error(str(error))
    for settings, variant, scene in jobs:
        log(f"mesh {variant.name}")
        bake(settings, variant, scene, path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
