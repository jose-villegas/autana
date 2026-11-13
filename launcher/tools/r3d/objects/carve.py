"""Which objects an import carves, and how many triangles each carved shape keeps.

A carved bake selects its rules' objects and every other object repeated as a rigid
copy whose copies touch nothing, so a free-standing prop needs no rule. A rule that pins `triangles` keeps that
budget; every other shape's comes from detail.py: its near level for the camera's closest view, stepped down by
the measured balance while the stored copy prototypes would exceed their room.
"""

import sys
from pathlib import Path
from dataclasses import asdict
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device"))

from board import PANEL_HEIGHT, PANEL_WIDTH  # noqa: E402
from r3d.mesh_source import camera_path_poses  # noqa: E402
from r3d.objects import detail  # noqa: E402
from r3d.objects.contact import contact_pairs  # noqa: E402
from r3d.objects.instances import match_instances, source_objects  # noqa: E402
from r3d.poses import focal_px  # noqa: E402

# The rule a free repeat is carved by: its seams free, since it touches nothing.
FREE_REPEAT = {"match": {"repeat": "touches nothing"}, "seam": "free"}
# A rigid copy needs a second object to be one.
MIN_COPIES = 2


def touching(pos, tri_v, tri_o, candidates, position_scale):
    """The candidate objects that touch any other object: a vertex of either within the seal tolerance of the
    other's surface (contact.py's test)."""
    return {obj for edge in contact_pairs(pos, tri_v, tri_o, position_scale, among=candidates)
            for obj in (edge.a, edge.b) if obj in candidates}


def free_repeats(pos, tri_v, tri_m, tri_o, object_names, selected, position_scale):
    """{object: FREE_REPEAT} for every object outside `selected` that is a rigid copy of another and whose shape's
    copies all touch nothing."""
    names = [str(i) for i in range(len(object_names))]
    rest = ~np.isin(tri_o, list(selected))
    shapes = [shape for shape in match_instances(source_objects(pos, tri_v[rest], tri_m[rest], tri_o[rest], names))
              if len(shape.instances) >= MIN_COPIES]
    copies = {int(instance.object_name) for shape in shapes for instance in shape.instances}
    touched = touching(pos, tri_v, tri_o, copies, position_scale)
    return {int(instance.object_name): FREE_REPEAT for shape in shapes
            if not any(int(instance.object_name) in touched for instance in shape.instances)
            for instance in shape.instances}


def views(scene, renderer):
    """(eyes, focal, near): the camera path's eye positions, sampled at the fit's coverage spacing or else the
    renderer's camera_path visibility spacing, and the panel's focal length. A scene without a camera path, or a
    renderer that samples none (its camera may stand anywhere in a region), has no eyes."""
    camera = getattr(getattr(scene, "camera", None), "component", None)
    if camera is None:
        from r3d.import_settings import SettingsError
        raise SettingsError("deriving carve budgets needs a scene camera")
    fit = getattr(renderer, "fit", None)
    every_ms = fit.coverage_every_ms if fit is not None else getattr(renderer.visibility, "every_ms", None)
    if camera.path is None or every_ms is None:
        lens = camera.half_fov_short_tan
        return np.zeros((0, 3)), focal_px(PANEL_WIDTH, PANEL_HEIGHT, lens), camera.near_z
    width, height, lens, near, poses = camera_path_poses(
        scene, SimpleNamespace(size=(PANEL_WIDTH, PANEL_HEIGHT), every_ms=every_ms), either_way_up=False)
    return np.array([pose[:3] for pose in poses]), focal_px(width, height, lens), near


def derived_budgets(pos, tri_v, tri_m, tri_o, selected, eyes, focal, near):
    """Near budgets and ladders per derived shape, grouped by rule as the carve groups them.
    The measured carve chooses rungs within the copies' room after stand-in growth."""
    names = [str(i) for i in range(int(tri_o.max()) + 1)]
    groups = rule_groups(selected)
    shapes = []
    for objects in groups.values():
        mine = np.isin(tri_o, objects)
        rule = selected[objects[0]]
        from r3d.simplify import rule_shapes
        for shape in rule_shapes(source_objects(pos, tri_v[mine], tri_m[mine], tri_o[mine], names), rule, set()):
            if "triangles" in rule:
                continue
            members = [int(instance.object_name) for instance in shape.instances]
            used = [np.unique(tri_v[tri_o == obj]) for obj in members]
            distance = detail.nearest_distance([pos[u].min(axis=0) for u in used], [pos[u].max(axis=0) for u in used],
                                               eyes, near)
            shapes.append(dict(objects=members, distance=distance, positions=shape.positions,
                               triangles=shape.triangles,
                               ladder=detail.ladder(shape.positions, shape.triangles, distance, focal)))
    picks = [0] * len(shapes)
    budgets = {}
    for shape, pick in zip(shapes, picks):
        shape["pick"] = pick
        floor = selected[shape["objects"][0]].get("min_triangles", 1)
        for obj in shape["objects"]:
            budgets[obj] = max(floor, shape["ladder"][pick].triangles)
    return budgets, shapes


def detail_report(shapes, passes, object_names, focal):
    """(rows, authoring): per derived shape its copies, nearest view, source and near-level triangles, its rung
    and that rung's triangles, the triangles the carve kept to fit the rung's error, and the carved level's
    measured surface move in pixels at that view; then the shapes needs_authoring() names."""
    carved = {frozenset(entry["objects"]): entry for entry in passes if "surface_error" in entry}
    rows = []
    for shape in shapes:
        names = [object_names[obj] for obj in shape["objects"]]
        entry = carved.get(frozenset(names), {})
        rows.append(dict(shape=names[0], copies=len(names), nearest=shape["distance"],
                         source_triangles=len(shape["triangles"]), near_triangles=shape["ladder"][0].triangles,
                         rung=shape["pick"], rung_triangles=shape["ladder"][shape["pick"]].triangles,
                         coarsest=shape["pick"] == len(shape["ladder"]) - 1,
                         carved_triangles=entry.get("output_triangles"),
                         error_px=entry["surface_error"] * focal / shape["distance"] if entry else None))
    found = detail.needs_authoring([(row["shape"], row["error_px"], row["rung"] > 0) for row in rows
                                    if row["error_px"] is not None])
    return rows, [asdict(item) for item in found]


def rule_groups(selected):
    """Object ids by rule index, preserving equal rules across derived shapes."""
    rules, groups = [], {}
    for obj, rule in selected.items():
        if rule not in rules:
            rules.append(rule)
        groups.setdefault(rules.index(rule), []).append(obj)
    return groups


def measured_balance(jobs, split):
    """Carve near levels, then pay the room using measured prototype counts at each coarser rung."""
    def count(result):
        return sum(row.get('stored_triangles', row['output_triangles'] * row['copies']) for row in result[2])

    from r3d.objects.budget import rest_target

    room = None if split is None else split[1]
    results = [job['build'](0) for job in jobs]
    total = sum(map(count, results))
    trials = {}
    while room is not None and total > room:
        steps = []
        for index, job in enumerate(jobs):
            info, pick = job['info'], job['pick']
            if info is None or pick + 1 >= len(info['ladder']):
                continue
            if index not in trials:
                trials[index] = job['build'](pick + 1)
            error = info['ladder'][pick + 1].error_px - info['ladder'][pick].error_px
            steps.append(((count(results[index]) - count(trials[index])) / error, -index))
        if not steps:
            rest_target(split, total)
            break
        index = -max(steps)[1]
        total += count(trials[index]) - count(results[index])
        results[index] = trials.pop(index)
        jobs[index]['pick'] += 1
    for job in jobs:
        if job['info'] is not None:
            job['info']['pick'] = job['pick']
    return results
