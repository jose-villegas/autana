"""Repeated units: every source object split into connected parts, parts grouped by shape, and the parts that
keep one placement around a repeated anchor grown into one unit (a window: frame, panes, sill), so each unit is
carved as a copy of one shared shape without a per-scene list. A unit becomes an object of its own; carve.py
decides whether and how its copies are carved, exactly as for a repeated object.

A part is a connected piece of one object (object_error.part_labels). Parts are matched exactly
(instances.match_instances): a source stamps a repeated part with the same triangles each time. A group of parts
with at least MIN_COPIES copies is an anchor; around each anchor copy, the repeated parts within UNIT_REACH of its
diagonal are expressed in the anchor's frame, and those at one local spot (within the match gap) in at least
UNIT_SUPPORT of the copies join the unit. A part votes only around its nearest anchor copy, and never as a second
copy of the anchor. A copy joins only with every part of the unit. Anchors go in order of the triangles their
copies cover; a claimed part leaves the pool.
"""

from collections import defaultdict
from dataclasses import dataclass

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

from r3d.objects.instances import SourceObject, match_instances
from r3d.objects.object_error import part_labels, part_names
from r3d.objects.surface_match import SURFACE_TOLERANCE

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


def spot_template(anchors, group, pool, claimed, tree, centre, diagonal, frame, gap):
    """The parts that sit at one spot in the anchor's frame around at least UNIT_SUPPORT of the anchor copies
    `anchors`: a list of {copy: part}, one per spot, each copy's first part there in neighbour order. Spots are
    the unclaimed parts of `pool` (the repeated parts, which `tree` holds) within UNIT_REACH diagonals, clustered
    within `gap` per part shape."""
    reach = tree.query_ball_point(centre[anchors], UNIT_REACH * diagonal[anchors], return_sorted=True)
    copy = np.repeat(np.arange(len(anchors)), [len(near) for near in reach])
    part = pool[np.concatenate(reach).astype(np.int64)] if len(copy) else np.zeros(0, dtype=np.int64)
    # A unit holds one copy of its anchor, and a part votes only around the anchor copy nearest it, so evenly
    # spaced copies (a row of windows) do not reach into each other's parts.
    nearest = cKDTree(centre[anchors]).query(centre[part])[1] if len(part) else part
    keep = (group[part] != group[anchors[0]]) & (nearest == copy) & ~claimed[part]
    copy, part = copy[keep], part[keep]
    # A spot holds at most one row per copy: a part shape with too few rows cannot reach the support.
    shape_of = group[part]
    _, inverse, counts = np.unique(shape_of, return_inverse=True, return_counts=True)
    keep = counts[inverse] >= UNIT_SUPPORT * len(anchors)
    copy, part, shape_of = copy[keep], part[keep], shape_of[keep]
    if not len(part):
        return []
    linear = np.stack([frame[anchor][0] for anchor in anchors])
    position = np.stack([frame[anchor][1] for anchor in anchors])
    local = np.einsum('nj,njk->nk', centre[part] - position[copy], linear[copy])
    pairs = cKDTree(local).query_pairs(gap, output_type="ndarray")
    pairs = pairs[shape_of[pairs[:, 0]] == shape_of[pairs[:, 1]]]
    graph = coo_matrix((np.ones(len(pairs)), (pairs[:, 0], pairs[:, 1])), shape=(len(part), len(part)))
    _, spot = connected_components(graph, directed=False)
    # Each copy's first row at each spot, spots in order of their first row.
    order = np.lexsort((np.arange(len(spot)), copy, spot))
    first = np.ones(len(order), dtype=bool)
    first[1:] = (spot[order][1:] != spot[order][:-1]) | (copy[order][1:] != copy[order][:-1])
    rows = order[first]
    held = np.bincount(spot[rows], minlength=spot.max() + 1)
    template = {}
    for row in rows[held[spot[rows]] >= UNIT_SUPPORT * len(anchors)]:
        template.setdefault(spot[row], {})[copy[row]] = part[row]
    _, start = np.unique(spot, return_index=True)
    return [template[at] for at in sorted(template, key=lambda at: start[at])]


def vote(shapes, index, centre, diagonal):
    """Units grown around repeated anchors: a list of (anchor shape, [the parts of each full copy])."""
    group = np.full(len(centre), -1)
    frame = {}
    for g, shape in enumerate(shapes):
        for instance in shape.instances:
            part = index[instance.object_name]
            group[part] = g
            frame[part] = (np.asarray(instance.linear), np.asarray(instance.position))
    copies = np.bincount(group[group >= 0], minlength=len(shapes))
    pool = np.flatnonzero((group >= 0) & (copies[np.maximum(group, 0)] > 1))
    tree = cKDTree(centre[pool])
    claimed = np.zeros(len(centre), dtype=bool)
    tris = np.array([len(shape.triangles) for shape in shapes])
    anchors = sorted((g for g in range(len(shapes)) if copies[g] >= MIN_COPIES),
                     key=lambda g: (-copies[g] * tris[g], g))
    units = []
    for g in anchors:
        members = np.array([index[i.object_name] for i in shapes[g].instances if not claimed[index[i.object_name]]])
        if len(members) < MIN_COPIES:
            continue
        gap = SURFACE_TOLERANCE * diagonal[members[0]]
        template = spot_template(members, group, pool, claimed, tree, centre, diagonal, frame, gap)
        full = []
        for copy, anchor in enumerate(members):
            parts = [int(anchor)] + [int(at[copy]) for at in template if copy in at]
            if len(parts) == 1 + len(template) and len(set(parts)) == len(parts) and not claimed[parts].any():
                full.append(parts)
                claimed[parts] = True
        if len(full) >= MIN_COPIES:
            units.append((g, full))
        else:
            for parts in full:
                claimed[parts] = False
    return units


def repeated_units(p, tri_v, tri_m, tri_o, object_names, taken=(), masked=()):
    """Split the objects not in `taken` (object ids carved whole: a scene's rules, rigid object copies) into parts
    and grow repeated parts into units, each unit with at least MIN_COPIES copies numbered as an object after the
    source's. A part with a triangle of a `masked` (alpha-tested) material joins no unit: it stays in the rest, as
    carve.repeats keeps such objects."""
    p, tri_v, tri_m, tri_o = map(np.asarray, (p, tri_v, tri_m, tri_o))
    labels, _ = part_labels(p, tri_v, tri_o)
    eligible = ~np.isin(tri_o, list(taken)) & ~np.isin(labels, labels[np.isin(tri_m, masked)])
    report = dict(parts=0, shapes=0, units=[])
    if not eligible.any():
        return Units(tri_o, list(object_names), report)
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
    shapes = match_instances(objects)
    source_tris = np.bincount(labels, minlength=len(names))
    unit_of = np.full(len(names), -1)
    unit_names = list(object_names)
    report.update(parts=len(names), shapes=len(shapes))
    for _, full in vote(shapes, index, centre, diagonal):
        for parts in full:
            unit_of[parts] = len(unit_names)
            unit_names.append(names[parts[0]])
        report["units"].append(dict(anchor=names[full[0][0]], copies=len(full), parts=len(full[0]),
                                    source_triangles=int(source_tris[full[0]].sum())))
    unit = unit_of[labels]
    return Units(np.where(unit >= 0, unit, tri_o), unit_names, report)
