#!/usr/bin/env python3
"""Bake an albedo import or a scene renderer's mesh.

An import writes its named mesh beside the import file. A scene writes each
baked renderer as <scene>.<object>.mesh beside the scene file; build_pack.py
puts both kinds of mesh in the pack.

    python launcher/tools/r3d/mesh_import.py PATH [--mesh NAME]

PATH is an .import.toml, which imports albedo geometry alone, or a
.scene.toml, which writes each placed renderer; renderers marked `bake = true`
use its lights, camera region and tone map. Run from the repository root after
installing tools/r3d/requirements.txt and initializing
third_party/upstream/meshoptimizer. Every mesh is written unless one is named.
"""

import argparse
import pathlib
import sys
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
    build_indirect_cache,
    encode_srgb8,
    face_colours,
    light,
    merge_matching_colours,
    sample_albedo,
    to_srgb8,
    visible_from_path,
    visible_from_region,
)
from r3d.lit_mesh import write_lit_mesh  # noqa: E402
from r3d.obj import load_mtl, load_obj, load_textures  # noqa: E402
from r3d.poses import either_way, sample_camera_path, tracks_file  # noqa: E402
from r3d.simplify import densify, simplify  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]


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


INDIRECT_CACHES = {}


def indirect_cache_for(src, settings, renderer, scene, intersector):
    """The indirect light cache of an import's source, built once per run: every
    variant and the reference see the same source, lights and settings."""
    if not renderer.bake or not scene.bake.indirect or not renderer.indirect:
        return None
    key = (str(settings.path), repr(vars(scene.bake.indirect)), repr(scene.lights), scene.bake.ray_offset,
           repr(vars(scene.indirect)))
    if key not in INDIRECT_CACHES:
        double = {index for index, name in enumerate(src.names) if name in settings.double_sided}
        INDIRECT_CACHES[key] = build_indirect_cache(
            src.p, src.tri_v, src.tri_m, range(len(src.names)), double,
            lambda centres, spacing, material: albedo_at(src, centres, spacing, material), intersector, scene.lights,
            scene.bake.ray_offset, scene.bake.indirect, scene.indirect.intensity, scene.indirect.albedo_boost)
    return INDIRECT_CACHES[key]


def shade_lit(src, settings, renderer, scene, material, mp, mt, double, intersector, rng, indirect_cache):
    """Vertices split along creases and lit on their own normals, near colours merged."""
    normals = corner_normals(mp, mt)
    corner_pos, corner_n = mp[mt].reshape(-1, 3), normals.reshape(-1, 3)
    key = np.concatenate([np.round(corner_pos * 16), np.round(corner_n * 64)], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    vpos, vn, vtris = corner_pos[first], corner_n[first], inverse.reshape(-1, 3)
    albedo = albedo_at(src, vpos, vertex_spacing(vpos, vtris), material)
    welded = np.unique(np.round(vpos * 16).astype(np.int64), axis=0, return_inverse=True)[1].reshape(-1)
    radiance = light(vpos, vn, np.full(len(vpos), double), intersector, scene.lights, scene.bake.ray_offset, rng,
                     indirect=indirect_cache, indirect_groups=welded)
    vrgb = to_srgb8(albedo * radiance, scene.tonemap_white)
    return merge_matching_colours(vpos, vrgb, vtris, scene.bake.colour_merge_step)


def shade_unlit(src, material, mp, mt):
    """The material's albedo, with no light: the colour the source authored."""
    return mp, encode_srgb8(albedo_at(src, mp, vertex_spacing(mp, mt), material)), mt


def camera_path_poses(settings, scene, visibility, every_ms=None, either_way_up=True):
    """The scene camera's path sampled every `every_ms` (the visibility
    step's own when None) at the step's size, through a square view that
    covers the panel held either way up unless `either_way_up` is False."""
    camera = scene.camera.component
    width, height = visibility.size
    poses = sample_camera_path(tracks_file(settings, scene), camera.path.tracks, camera.path.node,
                               every_ms or visibility.every_ms, width, height, camera.half_fov_short_tan, camera.near_z)
    return either_way(*poses) if either_way_up else poses


def visible_triangles(settings, visibility, scene, p, tri_v, double, intersector, rng):
    """Which source triangles the camera can see, by `visibility`'s source."""
    if visibility.source == "camera_path":
        width, height, lens, near, poses = camera_path_poses(settings, scene, visibility)
        return visible_from_path(p, tri_v, double, intersector, poses, width, height, lens, near, visibility.samples,
                                 visibility.margin)
    return visible_from_region(p, tri_v, double, intersector, visibility.rounds, rng, *scene.region)


def bake_geometry(settings, renderer, scene):
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
    visibility = renderer.visibility
    if visibility or renderer.bake:
        intersector = RayMeshIntersector(trimesh.Trimesh(src.p, tri_v, process=False))
    indirect_cache = indirect_cache_for(src, settings, renderer, scene, intersector) if renderer.bake else None
    double_names = settings.double_sided
    seen = np.ones(len(tri_v), dtype=bool)
    if visibility:
        double = np.array([src.names[material] in double_names for material in tri_m])
        seen = visible_triangles(settings, visibility, scene, src.p, tri_v, double, intersector, rng)
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
        if renderer.bake:
            vpos, vrgb, vtris = shade_lit(src, settings, renderer, scene, material, mp, mt, double, intersector, rng, indirect_cache)
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
        positions, rgb, tris, tri_mat = simplify(positions, rgb.astype(np.float64), tris, tri_mat, renderer.variant.triangles, props,
                                                 seal_seams=steps.seal_seams, **scale)
        rgb = np.clip(np.round(rgb), 0, 255).astype(np.int64)
        tri_double = np.isin(tri_mat, [index for index, name in enumerate(src.names) if name in double_names]).astype(np.int64)
    return SimpleNamespace(src=src, intersector=intersector, positions=positions, rgb=rgb, tris=tris, tri_double=tri_double,
                           tri_mat=tri_mat, scale=scale, indirect_cache=indirect_cache)


def flat_colours(settings, renderer, scene, geometry, face_samples, **knobs):
    """One colour per triangle of `geometry` for a flat variant's
    (samples, min, max, area) options. `knobs` are face_colours' own: sky_rays,
    placement and sun_centre."""
    src = geometry.src
    samples, sample_min, sample_max, sample_area = face_samples
    double_materials = {index for index, name in enumerate(src.names) if name in settings.double_sided}
    knobs.setdefault("sky_rays", scene.bake.flat_sky_rays)
    return face_colours(geometry.positions, geometry.tris, geometry.tri_mat, range(len(src.names)), double_materials,
                        lambda centres, spacing, material: albedo_at(src, centres, spacing, material), geometry.intersector,
                        scene.lights, scene.bake.ray_offset, scene.tonemap_white, samples, max_samples=sample_max,
                        sample_area=sample_area, min_samples=sample_min, indirect_cache=geometry.indirect_cache, **knobs)


def check_fitted(settings, renderer, scene, target):
    """A fitted variant is made offline by fitted_variant.py on a GPU; the bake
    only checks that the recipe and the committed mesh are the ones the fit
    recorded."""
    import hashlib

    from r3d.fitted_variant import recipe_digest

    again = "rerun fitted_variant.py prepare|fit and record the hashes it prints"
    if recipe_digest(settings, renderer, scene) != renderer.fit.recipe_sha256:
        raise SystemExit(f"{renderer.variant.name}: recipe changed since the fit; {again}")
    if not target.exists():
        raise SystemExit(f"{target.name} is missing; {again} (it needs a CUDA GPU)")
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    if digest != renderer.fit.sha256:
        raise SystemExit(f"{target.name} has SHA-256 {digest}, not the {renderer.fit.sha256} the fit recorded; {again}")
    log(f"{target.name} matches its fit recipe")


def bake(settings, renderer, scene, asset_name=None, out_dir=None):
    """Bakes one mesh. `scene` is None for an import that needs none."""
    if renderer.fit:
        check_fitted(settings, renderer, scene, (out_dir or settings.mesh_dir) / f"{asset_name or renderer.variant.name}.mesh")
        return
    geometry = bake_geometry(settings, renderer, scene)
    positions, rgb, tris, scale = geometry.positions, geometry.rgb, geometry.tris, geometry.scale
    face_rgb = flat_colours(settings, renderer, scene, geometry, renderer.face_samples) if renderer.face_samples else None
    mesh = write_lit_mesh(out_dir or settings.mesh_dir, asset_name or renderer.variant.name, positions, None if renderer.face_samples else rgb, tris, geometry.tri_double,
                          face_rgb=face_rgb, **scale)
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
            jobs = [(item.settings, item.renderer, scene, item.asset_name, item.asset_path.parent) for item in scene.renderers]
        elif path.name.endswith(".import.toml"):
            settings = load_import_settings(path)
            jobs = [(settings, SimpleNamespace(variant=variant, bake=None, face_samples=None, visibility=None, fit=None,
                                               indirect=True), None, variant.name, settings.mesh_dir) for variant in settings.variants]
        else:
            raise SettingsError("PATH must end in .import.toml or .scene.toml")
        jobs = [job for job in jobs if args.mesh in (None, job[1].variant.name, job[3])]
        if not jobs:
            raise SettingsError(f"no mesh named {args.mesh!r}")
    except SettingsError as error:
        parser.error(str(error))
    for settings, renderer, scene, asset_name, out_dir in jobs:
        log(f"mesh {asset_name}")
        bake(settings, renderer, scene, asset_name, out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
