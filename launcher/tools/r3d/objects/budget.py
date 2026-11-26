"""Count the stored prototypes in geometry and mesh bundles."""

import numpy as np

from r3d.lit_mesh import finest_count, read_bundle_mesh


def prototype_groups(shape, double_sided):
    """Copies sharing one stored prototype, by handedness and per-face sidedness."""
    groups = {}
    for instance, lit in shape['copies']:
        key = instance.mirrored, np.isin(lit[3], double_sided).tobytes()
        groups.setdefault(key, []).append((instance, lit))
    return groups


def stored_count(groups):
    """Triangles emitted by a carved shape's stored prototype groups."""
    return sum(len(copies[0][1][2]) for copies in groups.values())


def bundle_count(entries, names):
    """Finest triangles of each named stored mesh, independent of placement count."""
    return sum(finest_count(read_bundle_mesh(entries, name)) for name in sorted(set(names)))
