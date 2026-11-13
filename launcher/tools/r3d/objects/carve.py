"""Which objects an import carves, and how many triangles each carved shape keeps.

An import that carves ([[objects.meshes]]) carves its rules' objects and every other object repeated as a rigid
copy whose copies touch nothing, so a free-standing prop needs no rule. A rule that pins `triangles` keeps that
budget; every other shape's comes from detail.py: its near level for the camera's closest view, stepped down by
the balance while the shapes, each counted once, would push the variant past its ceiling.
"""

import sys
from pathlib import Path
from dataclasses import asdict
from types import SimpleNamespace

import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device"))

from board import PANEL_HEIGHT, PANEL_WIDTH  # noqa: E402
from r3d.mesh_source import camera_path_poses  # noqa: E402
from r3d.objects import detail  # noqa: E402
from r3d.objects.contact import _surface_border  # noqa: E402
from r3d.objects.instances import match_instances, source_objects  # noqa: E402
from r3d.poses import focal_px  # noqa: E402
from r3d.repair import seal_tolerance  # noqa: E402

# The rule a free repeat is carved by: its seams free, since it touches nothing.
FREE_REPEAT = {"match": {"repeat": "touches nothing"}, "seam": "free"}
# A rigid copy needs a second object to be one.
MIN_COPIES = 2


def touching(pos, tri_v, tri_o, candidates, position_scale):
    """The candidate objects that touch any other object: a vertex of either within the seal tolerance of the
    other's surface (contact.py's test)."""
    tolerance = seal_tolerance(position_scale)
    owners = np.unique(tri_o)
    faces = {int(obj): tri_v[tri_o == obj] for obj in owners}
    vertices = {obj: np.unique(t) for obj, t in faces.items()}
    lows = np.array([pos[vertices[int(obj)]].min(axis=0) for obj in owners]) - tolerance
    highs = np.array([pos[vertices[int(obj)]].max(axis=0) for obj in owners]) + tolerance
    centres, reach = (lows + highs) / 2, np.linalg.norm(highs - lows, axis=1) / 2
    tree = cKDTree(centres)
    found = set()
    for obj in sorted(candidates):
        at = int(np.searchsorted(owners, obj))
        for other in tree.query_ball_point(centres[at], reach[at] + reach.max()):
            if other == at or np.any(lows[at] > highs[other]) or np.any(lows[other] > highs[at]):
                continue
            b = int(owners[other])
            if (_surface_border(pos, vertices[obj], faces[b], tolerance)[0] or
                    _surface_border(pos, vertices[b], faces[obj], tolerance)[0]):
                found.add(obj)
                break
    return found


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
    fit = getattr(renderer, "fit", None)
    every_ms = fit.coverage_every_ms if fit is not None else getattr(renderer.visibility, "every_ms", None)
    if camera is None or camera.path is None or every_ms is None:
        lens = 1.0 if camera is None else camera.half_fov_short_tan
        return np.zeros((0, 3)), focal_px(PANEL_WIDTH, PANEL_HEIGHT, lens), 0.0 if camera is None else camera.near_z
    width, height, lens, near, poses = camera_path_poses(
        scene, SimpleNamespace(size=(PANEL_WIDTH, PANEL_HEIGHT), every_ms=every_ms), either_way_up=False)
    return np.array([pose[:3] for pose in poses]), focal_px(width, height, lens), near


def derived_budgets(pos, tri_v, tri_m, tri_o, selected, room, eyes, focal, near):
    """(budgets, shapes): each carved object whose rule pins no `triangles` gets its shape's balanced rung, never
    below the rule's min_triangles, within `room` less the pinned shapes' triangles, each shape counted once;
    `shapes` describes each derived shape for the report: its objects, nearest view distance, ladder and pick.
    Shapes are matched within each rule, as the carve matches them."""
    names = [str(i) for i in range(int(tri_o.max()) + 1)]
    groups = {}
    for obj, rule in selected.items():
        groups.setdefault(id(rule), []).append(obj)
    shapes = []
    for objects in groups.values():
        mine = np.isin(tri_o, objects)
        rule = selected[objects[0]]
        for shape in match_instances(source_objects(pos, tri_v[mine], tri_m[mine], tri_o[mine], names)):
            if "triangles" in rule:
                room = None if room is None else room - rule["triangles"]
                continue
            members = [int(instance.object_name) for instance in shape.instances]
            used = [np.unique(tri_v[tri_o == obj]) for obj in members]
            distance = detail.nearest_distance([pos[u].min(axis=0) for u in used], [pos[u].max(axis=0) for u in used],
                                               eyes, near)
            shapes.append(dict(objects=members, distance=distance, positions=shape.positions,
                               triangles=shape.triangles,
                               ladder=detail.ladder(shape.positions, shape.triangles, distance, focal)))
    picks = detail.balance([shape["ladder"] for shape in shapes], room)
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
                         carved_triangles=entry.get("output_triangles"),
                         error_px=entry["surface_error"] * focal / shape["distance"] if entry else None))
    found = detail.needs_authoring([(row["shape"], row["error_px"], row["rung"] > 0) for row in rows
                                    if row["error_px"] is not None])
    return rows, [asdict(item) for item in found]
