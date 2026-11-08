"""Repeated units: every source object split into connected parts, parts grouped by shape, and the parts that
keep one placement around a repeated anchor grown into one unit (a window: frame, panes, sill), so each unit is
carved as a copy of one shared shape without a per-scene list.

A part is a connected piece of one object (parts.part_labels). Parts are matched exactly, then joined by
surface (surface_match.merge) within the source's reduction error. A group of parts with at least MIN_COPIES
copies is an anchor; around each anchor copy, the repeated parts within UNIT_REACH of its diagonal are expressed
in the anchor's frame, and those at one local spot (within the match gap) in at least UNIT_SUPPORT of the copies
join the unit. A copy joins only with every part of the unit. Anchors go in order of the triangles their copies
cover; a claimed part leaves the pool.
"""

from collections import defaultdict
from dataclasses import dataclass

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

from r3d.objects.instances import SourceObject, match_instances
from r3d.objects.parts import part_labels, part_names
from r3d.objects.surface_match import SURFACE_TOLERANCE, merge

# A shape is carved when it has at least this many copies.
MIN_COPIES = 3
# A unit gathers repeated parts whose centres lie within this many anchor diagonals of the anchor's centre.
UNIT_REACH = 2.0
# A part joins a unit when it sits at its spot in at least this share of the anchor's copies.
UNIT_SUPPORT = 0.6


@dataclass(frozen=True)
class Units:
    tri_o: np.ndarray       # per source triangle: its object, or its unit (numbered after the objects)
    object_names: list      # the objects' names, then the units' (each named after its anchor part)
    selected: dict          # unit id -> its carve rule, one rule shared by every copy of a unit
    report: dict


def part_objects(p, tri_v, tri_m, labels, names):
    """Each part as a SourceObject, vertices in first-use order as instances.source_objects keeps them."""
    order = np.argsort(labels, kind="stable")
    bounds = np.flatnonzero(np.diff(labels[order])) + 1
    objects = []
    for rows in np.split(order, bounds):
        triangles = tri_v[rows]
        vertices, first, inverse = np.unique(triangles, return_index=True, return_inverse=True)
        keep = np.argsort(first)
        remap = np.argsort(keep)
        objects.append(SourceObject(names[labels[rows[0]]], p[vertices[keep]], remap[inverse].reshape(-1, 3),
                                    tri_m[rows]))
    return objects


def vote(shapes, index, centre, diagonal, floor):
    """Units grown around repeated anchors: a list of (anchor shape, [the parts of each full copy])."""
    group = np.full(len(centre), -1)
    frame = {}
    for g, shape in enumerate(shapes):
        for instance in shape.instances:
            part = index[instance.object_name]
            group[part] = g
            frame[part] = (np.asarray(instance.linear), np.asarray(instance.position))
    copies = np.bincount(group[group >= 0], minlength=len(shapes))
    repeated = (group >= 0) & (copies[np.maximum(group, 0)] > 1)
    tree = cKDTree(centre)
    claimed = np.zeros(len(centre), dtype=bool)
    tris = np.array([len(shape.triangles) for shape in shapes])
    anchors = sorted((g for g in range(len(shapes)) if copies[g] >= MIN_COPIES),
                     key=lambda g: (-copies[g] * tris[g], g))
    units = []
    for g in anchors:
        members = [index[i.object_name] for i in shapes[g].instances if not claimed[index[i.object_name]]]
        if len(members) < MIN_COPIES:
            continue
        rows = defaultdict(list)
        for copy, anchor in enumerate(members):
            linear, position = frame[anchor]
            for q in tree.query_ball_point(centre[anchor], UNIT_REACH * diagonal[anchor]):
                if q != anchor and repeated[q] and not claimed[q]:
                    rows[group[q]].append((copy, (centre[q] - position) @ linear, q))
        gap = max(SURFACE_TOLERANCE * diagonal[members[0]], floor)
        template = []
        for found in rows.values():
            local = np.array([spot for _, spot, _ in found])
            pairs = cKDTree(local).query_pairs(gap, output_type="ndarray")
            graph = coo_matrix((np.ones(len(pairs)), (pairs[:, 0], pairs[:, 1])), shape=(len(found), len(found)))
            _, spot = connected_components(graph, directed=False)
            for s in np.unique(spot):
                at = {}
                for row in np.flatnonzero(spot == s):
                    at.setdefault(found[row][0], found[row][2])
                if len(at) >= UNIT_SUPPORT * len(members):
                    template.append(at)
        full = []
        for copy, anchor in enumerate(members):
            parts = [anchor] + [at[copy] for at in template if copy in at]
            if len(parts) == 1 + len(template) and len(set(parts)) == len(parts) and not claimed[parts].any():
                full.append(parts)
                claimed[parts] = True
        if len(full) >= MIN_COPIES:
            units.append((g, full))
        else:
            for parts in full:
                claimed[parts] = False
    return units


def repeated_units(p, tri_v, tri_m, tri_o, object_names, floor=0.0, taken=()):
    """Split the objects not in `taken` (object ids a scene's own rules carve) into parts, grow repeated parts
    into units and give every unit with MIN_COPIES copies a carve rule, one rule shared by its copies. A unit
    keeps its source detail: its budget is a ceiling on detail, and its levels (each copy shape has its own,
    shared by its copies) bring a far copy down at run time, as the frame's one tolerance picks. `floor` is the source's reduction error, in source units: copies of one
    reduced shape differ by up to that much."""
    p, tri_v, tri_m, tri_o = map(np.asarray, (p, tri_v, tri_m, tri_o))
    eligible = ~np.isin(tri_o, list(taken))
    report = dict(parts=0, shapes=0, units=[])
    if not eligible.any():
        return Units(tri_o, list(object_names), {}, report)
    labels, _ = part_labels(p, tri_v, tri_o)
    names, _, _ = part_names(labels, tri_o, object_names)
    rows = np.flatnonzero(eligible)
    objects = part_objects(p, tri_v[rows], tri_m[rows], labels[rows], names)
    index = {name: k for k, name in enumerate(names)}
    low = np.full((len(names), 3), np.inf)
    high = np.full((len(names), 3), -np.inf)
    corners = p[tri_v].reshape(-1, 3)
    np.minimum.at(low, np.repeat(labels, 3), corners)
    np.maximum.at(high, np.repeat(labels, 3), corners)
    centre, diagonal = (low + high) / 2, np.linalg.norm(high - low, axis=1)
    shapes = merge(match_instances(objects), SURFACE_TOLERANCE, floor)
    source_tris = np.bincount(labels, minlength=len(names))
    out = tri_o.copy()
    unit_names, selected = list(object_names), {}
    report.update(parts=len(names), shapes=len(shapes))
    for _, full in vote(shapes, index, centre, diagonal, floor):
        anchor = names[full[0][0]]
        budget = int(source_tris[full[0]].sum())
        rule = dict(match=dict(object=anchor), triangles=budget, seam="free", shape="surface", surface_floor=floor)
        for parts in full:
            selected[len(unit_names)] = rule
            out[np.isin(labels, parts)] = len(unit_names)
            unit_names.append(names[parts[0]])
        report["units"].append(dict(anchor=anchor, copies=len(full), parts=len(full[0]),
                                    source_triangles=budget))
    return Units(out, unit_names, selected, report)
