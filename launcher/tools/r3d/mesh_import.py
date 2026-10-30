#!/usr/bin/env python3
"""Bake an albedo import or a scene renderer's mesh.

An import writes its named mesh, and a scene each baked renderer as
<scene>.<object>.mesh, into --out DIR, or beside the file when it is outside
this repository (a scratch copy). Inside it a mesh is a bake product: made by
bake/bake.py into the bake cache and locked, so without --out the bake refuses.

    python launcher/tools/r3d/mesh_import.py PATH [--mesh NAME] [--out DIR]

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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf.gltf_read import ASSET_SUFFIXES  # noqa: E402
from r3d import lcol_asset, mesh_bundle, mesh_asset, log  # noqa: E402
from r3d.gltf_mesh import load_gltf_mesh  # noqa: E402
from r3d.geometry import compact, corner_normals, weld_keeping  # noqa: E402
from r3d.import_settings import (  # noqa: E402
    BLEND_SUFFIX, SettingsError, albedo_jobs, match_meshes, variant_meshes, lfs_pointer_oid, load_import_settings, load_scene, source_files,
)
from r3d.light import (  # noqa: E402
    drop_masked,
    encode_srgb8,
    face_colours,
    light,
    merge_matching_colours,
    sample_albedo,
    to_srgb8,
    visible_from_path,
    visible_from_region,
)
from r3d.lit_mesh import POSITION_SCALE, WELD_PER_TICK, panel_colour, write_lit_mesh  # noqa: E402
from r3d.process_budget import NULL_RECORDER
from r3d.path_bake import PathLight  # noqa: E402
from r3d.obj import load_mtl, load_obj, load_textures  # noqa: E402
from r3d.poses import either_way, sample_camera_path  # noqa: E402
from r3d import ray_query  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.scene_name import SUFFIX  # noqa: E402
from r3d.simplify import Allocation, check_ceiling, densify, simplify  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]


def vertex_spacing(vpos, vtris):
    """Mean length of the edges meeting at each vertex, for the texture's level of detail."""
    edge_len = np.linalg.norm(vpos[vtris] - vpos[np.roll(vtris, 1, axis=1)], axis=2)
    spacing, count = np.zeros(len(vpos)), np.zeros(len(vpos))
    np.add.at(spacing, vtris.reshape(-1), edge_len.reshape(-1))
    np.add.at(count, vtris.reshape(-1), 1)
    return spacing / np.maximum(count, 1)


def load_source(settings, texture_dtype=np.float64):
    if settings.source["path"].suffix.lower() == BLEND_SUFFIX:
        raise SettingsError(f"{settings.source['path'].name}: a .blend is exported by launcher/tools/bake/bake.py "
                            "bake, which hands this import the export")
    for path in source_files(settings):
        if lfs_pointer_oid(path) is not None:
            raise SettingsError(f'{path}: Git LFS source is not pulled; run git lfs pull --exclude=""')
    if settings.source["path"].suffix.lower() in ASSET_SUFFIXES:
        return load_glb_source(settings)
    obj_path = settings.source["path"]
    materials = load_mtl(obj_path.with_suffix(".mtl"))
    p, uv, tri_v, tri_t, tri_m, names, tri_o, object_names = load_obj(obj_path)
    log(f"loaded {len(p)} vertices, {len(tri_v)} triangles, {len(names)} materials")
    textures = load_textures(obj_path.parent, materials, names, texture_dtype)
    return SimpleNamespace(p=p, uv=uv, tri_v=tri_v, tri_t=tri_t, tri_m=tri_m, names=names, materials=materials,
                           textures=textures, colors=None, tri_o=tri_o, object_names=object_names)


def load_glb_source(settings):
    """A glTF binary, or an FBX, as the OBJ path's source: untextured, its colour per vertex."""
    mesh = load_gltf_mesh(settings.source["path"])
    tri_v = np.array(mesh.tri_v, dtype=np.int64)
    log(f"loaded {len(mesh.positions)} vertices, {len(tri_v)} triangles, {len(mesh.names)} materials")
    return SimpleNamespace(p=np.array(mesh.positions, dtype=np.float64), uv=np.zeros((0, 2)), tri_v=tri_v,
                           tri_t=tri_v.copy(), tri_m=np.array(mesh.tri_m, dtype=np.int64), names=mesh.names,
                           materials={}, textures=[None] * len(mesh.names),
                           tri_o=np.asarray(mesh.tri_o, dtype=np.int64), object_names=mesh.object_names,
                           colors=np.array(mesh.colors, dtype=np.float64))


def albedo_at(src, points, spacing, material):
    kd = src.materials.get(src.names[material], {}).get("Kd", (1.0, 1.0, 1.0))
    return sample_albedo(points, spacing, material, src.p, src.uv, src.tri_v, src.tri_t, src.tri_m, src.textures, kd,
                         src.colors)


PATH_LIGHTS = {}


def path_light_for(src, job, scene, variant=None):
    """The bounced-light tracer of a renderer's source on `variant` (`ray_query.VARIANT` when None), built once per
    run: renderers of one import with the same lights, settings and variant share it. None when the renderer takes no
    bounced light. Only the latest is kept: each holds the source's textures a second time."""
    if job.bake is None or job.bake.indirect is None:
        return None
    settings = job.settings
    boost = scene.indirect.albedo_boost
    variant = variant or ray_query.VARIANT
    key = (str(settings.path), repr(vars(job.bake.indirect)), repr(scene.lights), boost, variant)
    if key not in PATH_LIGHTS:
        PATH_LIGHTS.clear()
        PATH_LIGHTS[key] = PathLight(src, scene.lights, settings.double_sided, job.bake.indirect, boost, variant)
    return PATH_LIGHTS[key]


def shade_lit(src, job, scene, material, mp, mt, double, intersector, bounce, *, labels, preserve_corners=False):
    """Vertices split along creases and lit on their own normals, near colours merged."""
    normals = corner_normals(mp, mt)
    corner_pos, corner_n = mp[mt].reshape(-1, 3), normals.reshape(-1, 3)
    grid = WELD_PER_TICK * (job.settings.position_scale or POSITION_SCALE)
    key = np.concatenate([np.round(corner_pos * grid), np.round(corner_n * 64)], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    vpos, vn, vtris = corner_pos[first], corner_n[first], inverse.reshape(-1, 3)
    albedo = albedo_at(src, vpos, vertex_spacing(vpos, vtris), material)
    welded = np.unique(np.round(vpos * grid).astype(np.int64), axis=0, return_inverse=True)[1].reshape(-1)
    radiance = light(vpos, vn, np.full(len(vpos), double), intersector, scene.lights, job.bake.ray_offset, bounce,
                     bounce_groups=welded, ao=job.bake.ao, bounce_intensity=scene.indirect.intensity)
    vrgb = to_srgb8(albedo * radiance, scene.tonemap_white)
    if preserve_corners:
        return corner_pos, vrgb[inverse], np.arange(len(corner_pos)).reshape(-1, 3), labels
    return merge_matching_colours(vpos, vrgb, vtris, grid, job.bake.colour_merge_step, labels=labels)


def shade_unlit(src, material, mp, mt):
    """The material's albedo, with no light: the colour the source authored."""
    return mp, encode_srgb8(albedo_at(src, mp, vertex_spacing(mp, mt), material)), mt


def camera_path_poses(scene, visibility, every_ms=None, either_way_up=True):
    """The scene camera's path sampled every `every_ms` (the visibility
    step's own when None) at the step's size, through a square view that
    covers the panel held either way up unless `either_way_up` is False."""
    camera = scene.camera.component
    width, height = visibility.size
    poses = sample_camera_path(camera.path.animation, camera.path.node,
                               every_ms or visibility.every_ms, width, height, camera.half_fov_short_tan, camera.near_z)
    return either_way(*poses) if either_way_up else poses


def fidelity_poses(scene, object_name, frames, dt):
    """frames + 1 poses of the scene camera's path, dt ms apart from time zero,
    at the pose size of OBJECT_fitted's `fit.poses.size`, so the traced reference matches the
    one that renderer was fitted against."""
    from anim import track_host
    job = next(item for item in scene.renderers if item.object.name == object_name + "_fitted")
    camera = scene.camera.component
    return track_host.poses(camera.path.animation, camera.path.node, dt, *job.renderer.fit.size,
                            camera.half_fov_short_tan, camera.near_z, until_ms=(frames + 1) * dt,
                            number_format=".9g")


def visible_triangles(visibility, scene, p, tri_v, double, intersector, rng):
    """Which source triangles the camera can see, by visibility's source."""
    if visibility.source == "camera_path":
        width, height, lens, near, poses = camera_path_poses(scene, visibility)
        return visible_from_path(p, tri_v, double, intersector, poses, width, height, lens, near, visibility.samples,
                                 visibility.margin)
    return visible_from_region(p, tri_v, double, intersector, visibility.rounds, rng, *scene.region)


def thin_meshes(src, selected, thin, seed):
    """Thin shared shapes once, mapping their local face choices to every copy."""
    from r3d.objects.instances import match_instances, source_objects

    keep = np.ones(len(src.tri_v), dtype=bool)
    if thin is None:
        return keep
    names = tuple(str(i) for i in range(len(src.object_names)))
    objects = source_objects(src.p, src.tri_v, src.tri_m, src.tri_o, names)
    shapes = match_instances([obj for obj in objects if int(obj.name) in selected])
    rng = np.random.default_rng(seed)
    materials = [i for i, name in enumerate(src.names) if name == thin.material]
    for shape in shapes:
        retained = ~np.isin(shape.materials, materials) | (rng.random(len(shape.triangles)) < thin.keep)
        for instance in shape.instances:
            rows = np.flatnonzero(src.tri_o == int(instance.object_name))
            keep[rows[np.asarray(instance.triangle_order, dtype=int)]] = retained
    return keep


def bake_geometry(job, scene, recorder=None, *, carve=False):
    """Everything a mesh needs before its colours are final: the source, its
    ray intersector and the simplified geometry with the colours a smooth bake
    keeps. `scene` is None for a bare import."""
    settings, renderer = job.settings, job.renderer
    rng = np.random.default_rng(settings.seed)
    recorder = NULL_RECORDER if recorder is None else recorder
    with recorder.step("source load", 0) as step:
        src = load_source(settings)
        step["triangles_out"] = len(src.tri_v)
    scale = {} if settings.position_scale is None else {"position_scale": settings.position_scale}
    tri_v, tri_t, tri_m = src.tri_v, src.tri_t, src.tri_m
    with recorder.step("alpha mask", len(tri_v)) as step:
        if settings.alpha_keep is not None:
            tri_v, tri_t, tri_m, keep = drop_masked(src.p, src.uv, tri_v, tri_t, tri_m, src.textures, settings.alpha_keep)
            src.tri_o = src.tri_o[keep]
            src.tri_v, src.tri_t, src.tri_m = tri_v, tri_t, tri_m
        step["triangles_out"] = len(tri_v)
    meshes = variant_meshes(settings.meshes, renderer.variant)
    selected = match_meshes(src.tri_o, tri_m, src.object_names, src.names, meshes) if meshes else {}
    carved_keep = thin_meshes(src, selected, settings.thin, settings.seed) if selected else None
    with recorder.step("visibility", len(tri_v)) as step:
        intersector = None
        visibility = renderer.visibility
        if visibility or job.bake:
            intersector = RayQuery(src.p, tri_v)
        double_names = settings.double_sided
        seen = np.ones(len(tri_v), dtype=bool)
        if visibility:
            double = np.array([src.names[material] in double_names for material in tri_m])
            seen = visible_triangles(visibility, scene, src.p, tri_v, double, intersector, rng)
        step["triangles_out"] = int(seen.sum())
    with recorder.step("thin", int(seen.sum())) as step:
        if settings.thin:
            thin = np.isin(tri_m, [index for index, name in enumerate(src.names) if name == settings.thin.material])
            seen &= ~thin | (rng.random(len(tri_v)) < settings.thin.keep)
        if selected:
            carved = np.isin(src.tri_o, list(selected))
            seen[carved] = carved_keep[carved]
        step["triangles_out"] = int(seen.sum())
    shown_v, shown_m, shown_o = tri_v[seen], tri_m[seen], src.tri_o[seen]
    mesh_report = {}
    shapes = [] if settings.meshes and carve else None
    with recorder.step("light", len(shown_v)) as step:
        bounce = path_light_for(src, job, scene)
        if settings.simplify:
            log("splitting evenly")
            wp, wt = weld_keeping(src.p, shown_v)
            dp, dt, ownership = densify(wp, wt, np.column_stack([shown_m, shown_o]), settings.simplify.dense_edge)
            dm, do = ownership.T
            parts = [(material, do[dm == material], *compact(dp, dt[dm == material]))
                     for material in range(len(src.names)) if np.any(dm == material)]
        else:
            parts = [(material, shown_o[shown_m == material], *compact(src.p, shown_v[shown_m == material]))
                     for material in range(len(src.names))
                     if np.any(shown_m == material)]
        if selected:
            split_parts = []
            for material, owners, mp, mt in parts:
                carved = np.isin(owners, list(selected))
                for mask in (carved, ~carved):
                    if np.any(mask):
                        split_parts.append((material, owners[mask], *compact(mp, mt[mask])))
            parts = split_parts
        all_pos, all_rgb, all_tris, all_double, all_mat, all_obj = [], [], [], [], [], []
        base = 0
        for material, owners, mp, mt in parts:
            double = src.names[material] in double_names
            if job.bake and not np.all(np.isin(owners, list(selected))):
                vpos, vrgb, vtris, owners = shade_lit(src, job, scene, material, mp, mt, double, intersector, bounce,
                                                         labels=owners)
            else:
                vpos, vrgb, vtris = shade_unlit(src, material, mp, mt)
            all_pos.append(vpos)
            all_rgb.append(vrgb)
            all_tris.append(vtris + base)
            all_double.append(np.full(len(vtris), int(double)))
            all_mat.append(np.full(len(vtris), material))
            all_obj.append(owners)
            base += len(vpos)
            log(f"  {src.names[material]}: {len(vpos)} vertices")
        positions, rgb, tris = np.concatenate(all_pos), np.concatenate(all_rgb), np.concatenate(all_tris)
        tri_double, tri_mat = np.concatenate(all_double), np.concatenate(all_mat)
        step["triangles_out"] = len(tris)
    with recorder.step("simplify", len(tris)) as step:
        if settings.simplify:
            steps = settings.simplify
            positions, rgb, tris, tri_mat = simplify(positions, rgb.astype(np.float64), tris, tri_mat, renderer.variant.triangles,
                                                   allocation=Allocation(tri_o=np.concatenate(all_obj), object_names=src.object_names,
                                                                         material_names=src.names, meshes=meshes, selection=selected,
                                                                         ceiling=getattr(renderer.variant, 'ceiling', None),
                                                                         shade=(lambda p, c, t, m: shade_placed(src, job, scene, p, c, t, m,
                                                                                                             intersector, bounce)) if job.bake else None,
                                                                         shade_shared=(lambda p, c, t, m: shade_placed(src, job, scene, p, c, t, m,
                                                                                                             intersector, bounce, preserve_corners=True))
                                                                                      if job.bake and carve else None,
                                                                         report=mesh_report, shapes=shapes),
                                                   reserved=[(frozenset(i for i, name in enumerate(src.names) if name in steps.props),
                                                              steps.props_share)],
                                                   colour_deviation=steps.colour_deviation, seal_seams=steps.seal_seams, **scale)
            rgb = np.clip(np.round(rgb), 0, 255).astype(np.int64)
            tri_double = np.isin(tri_mat, [index for index, name in enumerate(src.names) if name in double_names]).astype(np.int64)
        step["triangles_out"] = len(tris)
    check_ceiling(len(tris), getattr(renderer.variant, 'ceiling', None))
    return SimpleNamespace(src=src, intersector=intersector, positions=positions, rgb=rgb, tris=tris, tri_double=tri_double,
                           tri_mat=tri_mat, scale=scale, bounce=bounce, mesh_report=mesh_report, shapes=shapes)


def shade_placed(src, job, scene, positions, rgb, tris, labels, intersector, bounce, *, preserve_corners=False):
    """Light the simplified placed geometry against the full source occluder."""
    out_p, out_c, out_t, out_m = [], [], [], []
    base = 0
    for material in np.unique(labels):
        selected = labels == material
        p, t = compact(positions, tris[selected])
        double = src.names[material] in job.settings.double_sided
        p, c, t, m = shade_lit(src, job, scene, material, p, t, double, intersector, bounce,
                               labels=labels[selected], preserve_corners=preserve_corners)
        out_p.append(p)
        out_c.append(c)
        out_t.append(t + base)
        out_m.append(m)
        base += len(p)
    return np.concatenate(out_p), np.concatenate(out_c), np.concatenate(out_t), np.concatenate(out_m)


def flat_colours(job, scene, geometry, face_samples, **knobs):
    """One colour per triangle of `geometry` for a flat variant's
    (samples, min, max, area) options. `knobs` are face_colours' own: placement; `sky_rays` replaces every sky
    light's own ray count."""
    src, settings = geometry.src, job.settings
    samples, sample_min, sample_max, sample_area = face_samples
    double_materials = {index for index, name in enumerate(src.names) if name in settings.double_sided}
    lights = scene.lights
    if "sky_rays" in knobs:
        rays = knobs.pop("sky_rays")
        lights = [{**item, "rays": rays} if item["type"] == "sky" else item for item in lights]
    return face_colours(geometry.positions, geometry.tris, geometry.tri_mat, range(len(src.names)), double_materials,
                        lambda centres, spacing, material: albedo_at(src, centres, spacing, material), geometry.intersector,
                        lights, job.bake.ray_offset, scene.tonemap_white, samples, max_samples=sample_max,
                        sample_area=sample_area, min_samples=sample_min, bounce=geometry.bounce,
                        ao=job.bake.ao, bounce_intensity=scene.indirect.intensity, **knobs)


def write_baked(job, scene, out_dir, name, geometry=None, recorder=None):
    """Writes `job`'s bake as <name>.mesh in `out_dir`, flat when the renderer
    is and smooth otherwise, from `geometry` when bake_geometry already made it.
    Returns the baked mesh."""
    renderer = job.renderer
    out_dir = pathlib.Path(out_dir)
    recorder = NULL_RECORDER if recorder is None else recorder
    geometry = bake_geometry(job, scene, recorder, carve=True) if geometry is None else geometry
    face_rgb = None
    if renderer.face_samples and len(geometry.tris):
        with recorder.step("face colours", len(geometry.tris)):
            face_rgb = flat_colours(job, scene, geometry, renderer.face_samples)
    mesh = SimpleNamespace(pos=geometry.positions, tris=geometry.tris, clusters=[], nodes=[])
    if len(geometry.tris):
        mesh = write_lit_mesh(out_dir, name, geometry.positions, None if renderer.face_samples else geometry.rgb,
                              geometry.tris, geometry.tri_double, face_rgb=face_rgb, recorder=recorder,
                              meshlet_triangles=job.settings.meshlet_triangles, **geometry.scale)
    if getattr(geometry, "shapes", None) is not None:
        entries = [(name, mesh_asset.TYPE, (out_dir / f"{name}.mesh").read_bytes())] if len(mesh.tris) else []
        copies = []
        for shape in geometry.shapes:
            products, placed = write_shape(job, scene, out_dir, name, geometry, shape, recorder)
            entries.extend(products)
            copies.extend(placed)
        (out_dir / f"{name}{mesh_bundle.SUFFIX}").write_bytes(mesh_bundle.encode(entries, copies))
    return mesh


def write_shape(job, scene, out_dir, name, geometry, shape, recorder):
    """One local mesh per handedness, with colour sets following its baked array order."""
    from r3d.objects.instances import place

    entries, rows = [], []
    for mirrored in (False, True):
        copies = [(instance, lit) for instance, lit in shape['copies'] if instance.mirrored == mirrored]
        if not copies:
            continue
        first, lit = copies[0]
        first_name = geometry.src.object_names[int(first.object_name)]
        mesh_id = mesh_bundle.shape_id(name, shape['rule'], first_name)
        reflect = np.diag([-1 if mirrored else 1, 1, 1])
        p, _, t, labels = lit
        local = place(p[t].reshape(-1, 3) - first.position, first.linear.T, 0.0) * np.diag(reflect)
        tris = np.arange(len(local)).reshape(-1, 3)
        double = np.isin(labels, [i for i, material in enumerate(geometry.src.names)
                                if material in job.settings.double_sided]).astype(int)
        colours = []
        for instance, (p, rgb, t, labels) in copies:
            if job.renderer.face_samples:
                placed = SimpleNamespace(**vars(geometry))
                placed.positions, placed.tris, placed.tri_mat = p, t, labels
                colours.append(flat_colours(job, scene, placed, job.renderer.face_samples))
            else:
                colours.append(rgb[t].reshape(-1, 3))
        keys = np.concatenate(colours, axis=1)
        mesh = write_lit_mesh(out_dir, mesh_id, local, None if job.renderer.face_samples else keys,
                              tris, double, face_rgb=keys if job.renderer.face_samples else None,
                              colourless=True, recorder=recorder,
                              meshlet_triangles=job.settings.meshlet_triangles, **geometry.scale)
        encoding = lcol_asset.RGB565 if job.renderer.face_samples else lcol_asset.RGB888
        keys = mesh.face_keys if job.renderer.face_samples else mesh.colour_keys
        channels = lcol_asset.STRIDES[lcol_asset.RGB888]
        sets = [panel_colour(keys[:, k:k + channels]).tobytes() if job.renderer.face_samples
                else keys[:, k:k + channels].astype('u1').tobytes() for k in range(0, keys.shape[1], channels)]
        entries.append((mesh_id, mesh_asset.TYPE, (out_dir / f"{mesh_id}.mesh").read_bytes()))
        entries.append((lcol_asset.name_for_mesh(mesh_id), lcol_asset.TYPE,
                        lcol_asset.encode(encoding, len(keys), sets)))
        for index, (instance, _) in enumerate(copies):
            rows.append(dict(name=mesh_bundle.copy_name(job.object.name if job.object else name, mesh_id, index), mesh=mesh_id,
                             matrix=(instance.linear @ reflect).tolist(), position=instance.position.tolist(),
                             colour_set=index, owner=job.object.name if job.object else name,
                             match=shape["rule"]["match"], first=first_name,
                             source=geometry.src.object_names[int(instance.object_name)]))
    return entries, rows


def bake(job, scene, out_dir):
    """Bakes one mesh into `out_dir`. `scene` is None for a bare import. A fitted variant is made by
    fitted_variant.py on a CUDA GPU, which bake/bake.py runs."""
    if job.renderer.fit:
        log(f"{job.asset_name}: a fit, made by fitted_variant.py on a CUDA GPU (bake/bake.py bake --kind fit)")
        return
    mesh = write_baked(job, scene, out_dir, job.asset_name)
    log(f"emitted {len(mesh.pos)} vertices, {len(mesh.tris)} triangles, {len(mesh.clusters)} clusters, {len(mesh.nodes)} nodes")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", help="an .import.toml or a .scene.toml file")
    parser.add_argument("--mesh", help="bake only the mesh with this name")
    parser.add_argument("--out", help="the folder to write into; beside PATH when it is outside the repository")
    args = parser.parse_args(argv)
    path = pathlib.Path(args.path).resolve()
    if args.out is None and path.is_relative_to(REPO):
        parser.error(f"{path.name} is in the repository, where a mesh is a bake product: bake it with "
                     "launcher/tools/bake/bake.py bake, or pass --out DIR for a scratch bake")
    out_dir = pathlib.Path(args.out) if args.out else path.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    scene = None
    try:
        if path.name.endswith(SUFFIX):
            scene = load_scene(path)
            jobs = scene.renderers
        elif path.name.endswith(".import.toml"):
            jobs = albedo_jobs(load_import_settings(path))
        else:
            raise SettingsError(f"PATH must end in .import.toml or {SUFFIX}")
        jobs = [job for job in jobs if args.mesh in (None, job.renderer.variant.name, job.asset_name)]
        if not jobs:
            raise SettingsError(f"no mesh named {args.mesh!r}")
    except SettingsError as error:
        parser.error(str(error))
    for job in jobs:
        log(f"mesh {job.asset_name}")
        bake(job, scene, out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
