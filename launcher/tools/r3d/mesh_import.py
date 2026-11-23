#!/usr/bin/env python3
"""Bake an albedo import or a scene renderer's mesh.

An import writes its named mesh, and a scene each baked renderer as
<scene>.<object>.bundle, into --out DIR, or beside the file when it is outside
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
from dataclasses import replace
import pathlib
import sys
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import lcol_asset, mesh_bundle, mesh_asset, log  # noqa: E402
from r3d.geometry import compact, corner_normals, weld_keeping  # noqa: E402
from r3d.import_settings import (  # noqa: E402
    SettingsError, albedo_jobs, match_meshes, variant_meshes, load_import_settings, load_scene,
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
from r3d.lod.bake_levels import bake_lit_mesh
from r3d.lod.morph import morph_blob
from r3d.lit_mesh import POSITION_SCALE, WELD_PER_TICK, bundle_entries, panel_colour, build_lit_mesh  # noqa: E402
from r3d.mesh_source import load_source, load_glb_source, path_light_for, camera_path_poses, with_triangles
from r3d.obj import alpha_tested
from r3d.objects.budget import ceiling_split, prototype_groups
from r3d.objects.carve import derived_budgets, detail_report, free_repeats, views
from r3d.cards import emit as card_emit
from r3d.cards.emit import QUAD_CORNERS
from r3d.texture import texture_asset, uv_transfer
from r3d.process_budget import NULL_RECORDER
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.scene_name import SUFFIX  # noqa: E402
from r3d.simplify import Allocation, check_ceiling, densify, simplify  # noqa: E402
from r3d.thin import pieces, thin, thin_keep

REPO = pathlib.Path(__file__).resolve().parents[3]


def vertex_spacing(vpos, vtris):
    """Mean length of the edges meeting at each vertex, for the texture's level of detail."""
    edge_len = np.linalg.norm(vpos[vtris] - vpos[np.roll(vtris, 1, axis=1)], axis=2)
    spacing, count = np.zeros(len(vpos)), np.zeros(len(vpos))
    np.add.at(spacing, vtris.reshape(-1), edge_len.reshape(-1))
    np.add.at(count, vtris.reshape(-1), 1)
    return spacing / np.maximum(count, 1)


def albedo_at(src, points, spacing, material):
    kd = src.materials.get(src.names[material], {}).get("Kd", (1.0, 1.0, 1.0))
    return sample_albedo(points, spacing, material, src.p, src.uv, src.tri_v, src.tri_t, src.tri_m, src.textures, kd,
                         src.colors)


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


def fidelity_poses(scene, object_name, frames, dt):
    """frames + 1 poses of the scene camera's path, dt ms apart from time zero, at the fit size of
    `object_name`'s fitted renderer, so the traced reference matches the one it was fitted against."""
    from anim import track_host
    from r3d.import_settings import SettingsError
    from r3d.poses import flight_camera
    job = next((item for item in scene.renderers if item.object.name == object_name), None)
    if job is None or not job.renderer.fit:
        raise SettingsError(f"{scene.path}: {object_name} is not a fitted renderer")
    camera = flight_camera(scene)
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


def thin_choice(src, tri_v, tri_m, settings, renderer, seen, selected):
    """(per triangle, the share thinning keeps; its piece): the thin rule's material, and with
    simplify on, every aggregate of a cut-out material at the simplify ratio (the budget over the triangles seen).
    Carved objects are left to their own thinning."""
    names = {settings.thin.material} if settings.thin else set()
    cut = alpha_tested(src.textures) if settings.simplify else np.zeros(len(src.names), dtype=bool)
    candidates = np.isin(tri_m, [i for i, name in enumerate(src.names) if cut[i] or name in names])
    piece = np.full(len(tri_v), -1)
    if np.any(candidates):
        piece[candidates] = pieces(src.p, tri_v[candidates], tri_m[candidates])
    ratio = renderer.variant.triangles / max(seen, 1) if settings.simplify else 1
    return thin_keep(tri_m, src.tri_o, piece, settings.thin, cut, ratio, src.names, skip=selected), piece


def carve_selection(src, tri_v, tri_m, selected, settings, job, scene):
    """The rules' objects plus every free repeat, each carried by one rule per shape whose `triangles` is pinned
    or derived (objects/carve.py), and the derived shapes for the report."""
    position_scale = POSITION_SCALE if settings.position_scale is None else settings.position_scale
    masked = np.flatnonzero(alpha_tested(src.textures))
    selected = selected | free_repeats(src.p, tri_v, tri_m, src.tri_o, src.object_names, selected, position_scale,
                                       masked)
    if not any("triangles" not in rule for rule in selected.values()):
        return selected, None
    eyes, focal, near, sight = views(scene, job.renderer)
    _, shapes = derived_budgets(src.p, tri_v, tri_m, src.tri_o, selected, eyes, focal, near, sight)
    per_object = {}
    for shape in shapes:
        for obj in shape["objects"]:
            per_object[obj] = shape
    return selected, SimpleNamespace(shapes=shapes, focal=focal, per_object=per_object)


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
    accepted_cards, card_report = [], None
    # The replaced material leaves the geometry but keeps casting light: the light bake's occluders and bounce
    # source hold it, so the surfaces the cards are drawn over keep its shadows.
    light_only = None
    if settings.cards is not None:
        with recorder.step("cards", len(tri_v)) as step:
            accepted_cards, card_report = card_emit.carded(src, settings, scene)
            step["triangles_out"] = sum(report['triangles'] for _, _, report in accepted_cards)
        for name, report in card_report.items():
            share = "always" if report['forced'] else f"{report['see_through']:.3f}"
            log(f"cards: {name}: see-through={share}, covered={report['covered']:.3f} "
                f"(all area {report['covered_area']:.3f}), "
                f"accepted={report['accepted']}")
        selected_cards = np.isin(tri_m, [material for material, _, _ in accepted_cards])
        if selected_cards.any():
            light_only = tri_v[selected_cards], tri_t[selected_cards], tri_m[selected_cards]
            tri_v, tri_t, tri_m = tri_v[~selected_cards], tri_t[~selected_cards], tri_m[~selected_cards]
            src.tri_o = src.tri_o[~selected_cards]
            src.tri_v, src.tri_t, src.tri_m = tri_v, tri_t, tri_m
    with recorder.step("alpha mask", len(tri_v)) as step:
        if settings.alpha_keep is not None:
            tri_v, tri_t, tri_m, keep = drop_masked(src.p, src.uv, tri_v, tri_t, tri_m, src.textures, settings.alpha_keep)
            if light_only is not None:
                light_only = drop_masked(src.p, src.uv, *light_only, src.textures, settings.alpha_keep)[:3]
            src.tri_o = src.tri_o[keep]
            src.tri_v, src.tri_t, src.tri_m = tri_v, tri_t, tri_m
        step["triangles_out"] = len(tri_v)
    meshes = variant_meshes(settings.meshes, renderer.variant)
    if not carve or scene is None:
        meshes = tuple(rule for rule in meshes if "triangles" in rule)
    selected = match_meshes(src.tri_o, tri_m, src.object_names, src.names, meshes) if meshes else {}
    derived = None
    if carve and scene is not None:
        selected, derived = carve_selection(src, tri_v, tri_m, selected, settings, job, scene)
    carved_keep = thin_meshes(src, selected, settings.thin, settings.seed) if selected else None
    with recorder.step("visibility", len(tri_v)) as step:
        intersector = None
        visibility = renderer.visibility
        if visibility or job.bake:
            intersector = RayQuery(src.p, tri_v) if len(tri_v) else None
        # Visibility is the drawn geometry's own; light also meets the replaced material.
        light_src = src if light_only is None else with_triangles(src, *(np.concatenate([a, b]) for a, b in
                                                                         zip((tri_v, tri_t, tri_m), light_only)))
        occluder = intersector if light_only is None or not job.bake else RayQuery(src.p, light_src.tri_v)
        double_names = settings.double_sided
        seen = np.ones(len(tri_v), dtype=bool)
        if visibility:
            double = np.array([src.names[material] in double_names for material in tri_m])
            seen = visible_triangles(visibility, scene, src.p, tri_v, double, intersector, rng)
        step["triangles_out"] = int(seen.sum())
    with recorder.step("thin", int(seen.sum())) as step:
        points, held = src.p, np.zeros(len(tri_v), dtype=bool)
        keep, piece = thin_choice(src, tri_v, tri_m, settings, renderer, int(seen.sum()), selected)
        if np.any(keep < 1):
            kept, points, tri_v = thin(src.p, tri_v, piece, keep, rng)
            seen &= kept
            held = keep < 1
        if selected:
            carved = np.isin(src.tri_o, list(selected))
            seen[carved] = carved_keep[carved]
        step["triangles_out"] = int(seen.sum())
    shown_v, shown_m, shown_o, shown_h = tri_v[seen], tri_m[seen], src.tri_o[seen], held[seen]
    mesh_report = {}
    shapes = [] if carve else None
    with recorder.step("light", len(shown_v)) as step:
        bounce = path_light_for(light_src, job, scene)
        if settings.simplify:
            log("splitting evenly")
            wp, wt = weld_keeping(points, shown_v)
            dp, dt, ownership = densify(wp, wt, np.column_stack([shown_m, shown_o, shown_h]),
                                        settings.simplify.dense_edge)
            dm, do, dh = ownership.T
        else:
            dp, dt, dm, do, dh = points, shown_v, shown_m, shown_o, shown_h
        carved = np.isin(do, list(selected))
        parts = [(material, do[mask], hold, *compact(dp, dt[mask]))
                 for material in range(len(src.names)) for split in (carved, ~carved) for hold in (False, True)
                 for mask in [(dm == material) & split & (dh.astype(bool) == hold)] if np.any(mask)]
        all_pos, all_rgb, all_tris, all_double, all_mat, all_obj, all_held = [], [], [], [], [], [], []
        base = 0
        for material, owners, hold, mp, mt in parts:
            double = src.names[material] in double_names
            if job.bake and not np.all(np.isin(owners, list(selected))):
                vpos, vrgb, vtris, owners = shade_lit(src, job, scene, material, mp, mt, double, occluder, bounce,
                                                         labels=owners)
            else:
                vpos, vrgb, vtris = shade_unlit(src, material, mp, mt)
            all_pos.append(vpos)
            all_rgb.append(vrgb)
            all_tris.append(vtris + base)
            all_double.append(np.full(len(vtris), int(double)))
            all_mat.append(np.full(len(vtris), material))
            all_obj.append(owners)
            all_held.append(np.full(len(vtris), hold))
            base += len(vpos)
            log(f"  {src.names[material]}: {len(vpos)} vertices")
        positions = np.concatenate(all_pos) if all_pos else np.empty((0, 3))
        rgb = np.concatenate(all_rgb) if all_rgb else np.empty((0, 3), dtype=int)
        tris = np.concatenate(all_tris) if all_tris else np.empty((0, 3), dtype=int)
        tri_double = np.concatenate(all_double) if all_double else np.empty(0, dtype=int)
        tri_mat = np.concatenate(all_mat) if all_mat else np.empty(0, dtype=int)
        step["triangles_out"] = len(tris)
    ceiling, room, floor = ceiling_split(renderer)
    card_triangles = sum(report['triangles'] for _, _, report in accepted_cards)
    check_ceiling(card_triangles, ceiling)
    # Cards are paid first: the rest and the carved copies share what they leave.
    available = None if ceiling is None else ceiling - card_triangles
    split = (available, None if room is None else room - card_triangles, floor)
    with recorder.step("simplify", len(tris)) as step:
        if settings.simplify and len(tris):
            steps = settings.simplify
            budget = renderer.variant.triangles if available is None else min(renderer.variant.triangles, available)
            mesh_report['rest_budget'] = budget
            positions, rgb, tris, tri_mat = simplify(positions, rgb.astype(np.float64), tris, tri_mat, budget,
                                                   allocation=Allocation(tri_o=np.concatenate(all_obj), object_names=src.object_names,
                                                                         material_names=src.names, meshes=meshes, selection=selected,
                                                                         detail=derived, split=split if carve else None,
                                                                         double_sided=tuple(i for i, name in enumerate(src.names) if name in double_names),
                                                                         ceiling=available,
                                                                         shade=(lambda p, c, t, m, **kw: shade_placed(src, job, scene, p, c, t, m,
                                                                                                             occluder, bounce, **kw)) if job.bake else None,
                                                                         shade_shared=(lambda p, c, t, m, **kw: shade_placed(src, job, scene, p, c, t, m,
                                                                                                             occluder, bounce, preserve_corners=True, **kw))
                                                                                      if job.bake and carve else None,
                                                                         report=mesh_report, shapes=shapes,
                                                                         held=np.concatenate(all_held)),
                                                   reserved=[(frozenset(i for i, name in enumerate(src.names) if name in steps.props),
                                                              steps.props_share)],
                                                   colour_deviation=steps.colour_deviation, seal_seams=steps.seal_seams, **scale)
            rgb = np.clip(np.round(rgb), 0, 255).astype(np.int64)
            tri_double = np.isin(tri_mat, [index for index, name in enumerate(src.names) if name in double_names]).astype(np.int64)
        step["triangles_out"] = len(tris)
    from r3d.objects.instances import Instance

    for _, card_shape, _ in accepted_cards:
        p, albedo = card_shape['positions'], card_shape['albedo']
        if job.bake:
            radiance = light(p, card_shape['normals'], np.ones(len(p), bool), intersector, scene.lights,
                             job.bake.ray_offset, bounce, ao=job.bake.ao,
                             bounce_intensity=scene.indirect.intensity)
            colours = to_srgb8(albedo * radiance, scene.tonemap_white)
        else:
            colours = encode_srgb8(albedo)
        instance = Instance(card_shape['owner'], np.eye(3), np.zeros(3), False, 0.0)
        card_shape['copies'] = [(instance, (p, colours, card_shape['tris'], card_shape['labels']))]
        shapes = [] if shapes is None else shapes
        shapes.append(card_shape)
    if card_report is not None:
        mesh_report['cards'] = card_report
        if 'ceiling' in mesh_report:
            mesh_report['ceiling'] = ceiling
        mesh_report['scene_total'] = mesh_report['output_triangles'] = (
            mesh_report.get('scene_total', len(tris)) + card_triangles)
    # The ceiling caps what is stored: a prototype once however many copies draw it, and each card.
    stored = mesh_report.get('stored_total', len(tris)) + card_triangles
    if card_report is not None:
        mesh_report['stored_total'] = stored
    check_ceiling(stored, ceiling)
    if derived is not None and "passes" in mesh_report:
        mesh_report["detail"], mesh_report["authoring"] = detail_report(derived.shapes, mesh_report["passes"],
                                                                        src.object_names, derived.focal)
    return SimpleNamespace(src=src, intersector=occluder, positions=positions, rgb=rgb, tris=tris, tri_double=tri_double,
                           tri_mat=tri_mat, scale=scale, bounce=bounce, mesh_report=mesh_report, shapes=shapes)


def shade_placed(src, job, scene, positions, rgb, tris, labels, intersector, bounce, *, preserve_corners=False,
                 groups=None):
    """Light the simplified placed geometry against the full source occluder, one group of triangles at a time. A
    group holds equal `groups` values (`labels` by default) and is lit as its first triangle's material."""
    out_p, out_c, out_t, out_m = [], [], [], []
    base = 0
    groups = labels if groups is None else groups
    for group in np.unique(groups):
        selected = groups == group
        material = labels[selected][0]
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
    """Write a mesh bundle from prepared geometry or its source, returning the rest mesh.
    A fitted renderer's rest is its plain CPU start; the later CPU levels stage builds its hierarchy.
    Copy prototypes retain their levels and colour sets."""
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
        mesh = build_lit_mesh(geometry.positions, None if renderer.face_samples else geometry.rgb,
                              geometry.tris, geometry.tri_double, face_rgb=face_rgb, recorder=recorder, mesh_builder=bake_lit_mesh,
                              meshlet_triangles=job.settings.meshlet_triangles,
                              lod=job.settings.lod and getattr(renderer, "fit", None) is None,
                              **geometry.scale)
    entries = bundle_entries(name, mesh, morph_blob if job.settings.morph else None) if len(mesh.tris) else []
    copies = []
    for shape in getattr(geometry, "shapes", None) or ():
        products, placed = write_shape(job, scene, out_dir, name, geometry, shape, recorder)
        entries.extend(products)
        copies.extend(placed)
    with recorder.step("write", len(mesh.tris)):
        (out_dir / f"{name}{mesh_bundle.SUFFIX}").write_bytes(mesh_bundle.encode(entries, copies))
    return mesh


def write_shape(job, scene, out_dir, name, geometry, shape, recorder):
    """One local mesh per handedness and double-sided mask (copies joined by surface keep their own materials,
    which may differ in sidedness), with colour sets following its baked array order."""
    from r3d.objects.instances import place

    masked = "vertex_uvs" in shape
    flat = job.renderer.face_samples and not masked
    double_sided = [i for i, material in enumerate(geometry.src.names) if material in job.settings.double_sided]
    groups = prototype_groups(shape, double_sided)
    entries, rows = [], []
    for (mirrored, _), copies in sorted(groups.items(), key=lambda group: group[0][0]):
        first, lit = copies[0]
        first_name = geometry.src.object_names[int(first.object_name)]
        mesh_id = mesh_bundle.shape_id(shape.get('name', job.asset_name), shape['rule'], first_name)
        reflect = np.diag([-1 if mirrored else 1, 1, 1])
        p, _, t, labels = lit
        local = place(p[t].reshape(-1, 3) - first.position, first.linear.T, 0.0) * np.diag(reflect)
        tris = np.arange(len(local)).reshape(-1, 3)
        double = np.isin(labels, double_sided).astype(int)
        if masked:
            double[:] = 1
        options = dict(vertex_uvs=shape['vertex_uvs'][t].reshape(-1, 2),
                       vertex_pages=np.repeat(shape['page_of_card'], len(QUAD_CORNERS))[t].ravel()) if masked else {}
        colours = []
        for instance, (p, rgb, t, labels) in copies:
            if flat:
                placed = SimpleNamespace(**vars(geometry))
                placed.positions, placed.tris, placed.tri_mat = p, t, labels
                colours.append(flat_colours(job, scene, placed, job.renderer.face_samples))
            else:
                colours.append(rgb[t].reshape(-1, 3))
        keys = np.concatenate(colours, axis=1)
        mesh = build_lit_mesh(local, None if flat else keys,
                              tris, double, face_rgb=keys if flat else None,
                              colourless=True, recorder=recorder, mesh_builder=bake_lit_mesh,
                              meshlet_triangles=job.settings.meshlet_triangles, lod=job.settings.lod and not masked,
                              **geometry.scale, **options)
        if masked:
            luvs = uv_transfer.card_luvs(mesh, mesh.vertex_uvs, mesh.page_of_cluster)
            entries.append((uv_transfer.name_for_mesh(mesh_id), uv_transfer.TYPE, uv_transfer.pack_luvs(luvs)))
            coloured = SimpleNamespace(**vars(mesh))
            coloured.rgb = mesh.colour_keys[:, :lcol_asset.STRIDES[lcol_asset.RGB888]]
            for page, texture in enumerate(shape['pages']):
                tint = uv_transfer.drawn_tint(coloured, luvs, page)
                texture = replace(texture, tint=tuple(int(round(c)) for c in tint))
                entries.append((uv_transfer.texture_name(mesh_id, page), texture_asset.TYPE,
                                texture_asset.pack_ltex(texture)))
        encoding = lcol_asset.RGB565 if flat else lcol_asset.RGB888
        keys = mesh.face_keys if flat else mesh.colour_keys
        channels = lcol_asset.STRIDES[lcol_asset.RGB888]
        sets = [panel_colour(keys[:, k:k + channels]).tobytes() if flat
                else keys[:, k:k + channels].astype('u1').tobytes() for k in range(0, keys.shape[1], channels)]
        entries.extend(bundle_entries(mesh_id, mesh, morph_blob if job.settings.morph else None))
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
    """Bakes one mesh into `out_dir`. `scene` is None for a bare import. A fitted renderer's fit needs a CUDA
    GPU (bake/bake.py bake --kind fit runs fitted_variant.py), so here it gets the CPU method: its start, the
    import simplified to the fit's budget, with levels."""
    mesh = write_baked(job, scene, out_dir, job.asset_name)
    if job.renderer.fit:
        from r3d.lod.levels_stage import levels

        path = pathlib.Path(out_dir) / f"{job.asset_name}{mesh_bundle.SUFFIX}"
        path.write_bytes(levels(path.read_bytes(), job.asset_name, job.settings.lod, job.settings.meshlet_triangles))
        log(f"{job.asset_name}: its CPU start with levels; the fit needs a CUDA GPU (bake/bake.py bake --kind fit)")
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
