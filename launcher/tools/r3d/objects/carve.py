"""Select free rigid repeats and group carved objects by rule."""

import numpy as np

from r3d.objects.contact import contact_pairs
from r3d.objects.instances import match_instances, source_objects

# The rule a free repeat is carved by: its seams free, since it touches nothing.
FREE_REPEAT = {"match": {"repeat": "touches nothing"}, "seam": "free"}
# A rigid copy needs a second object to be one.
MIN_COPIES = 2


def touching(pos, tri_v, tri_o, candidates, position_scale):
    """The candidate objects that touch any other object: a vertex of either within the seal tolerance of the
    other's surface (contact.py's test)."""
    return {obj for edge in contact_pairs(pos, tri_v, tri_o, position_scale, among=candidates)
            for obj in (edge.a, edge.b) if obj in candidates}


def free_repeats(pos, tri_v, tri_m, tri_o, object_names, selected, position_scale, masked=()):
    """{object: FREE_REPEAT} for every object outside `selected` that is a rigid copy of another, has no triangle
    of a `masked` (alpha-tested) material, and whose shape's copies all touch nothing."""
    names = [str(i) for i in range(len(object_names))]
    cut = tri_o[np.isin(tri_m, masked)]
    rest = ~np.isin(tri_o, [*selected, *cut])
    shapes = [shape for shape in match_instances(source_objects(pos, tri_v[rest], tri_m[rest], tri_o[rest], names))
              if len(shape.instances) >= MIN_COPIES]
    copies = {int(instance.object_name) for shape in shapes for instance in shape.instances}
    touched = touching(pos, tri_v, tri_o, copies, position_scale)
    return {int(instance.object_name): FREE_REPEAT for shape in shapes
            if not any(int(instance.object_name) in touched for instance in shape.instances)
            for instance in shape.instances}


def rule_groups(selected):
    """Object ids by rule index, preserving equal rules across derived shapes."""
    rules, groups = [], {}
    for obj, rule in selected.items():
        if rule not in rules:
            rules.append(rule)
        groups.setdefault(rules.index(rule), []).append(obj)
    return groups
