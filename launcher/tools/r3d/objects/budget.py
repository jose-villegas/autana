"""The variant ceiling split and the stored prototypes paid from it."""

import numpy as np

from r3d.import_settings import SettingsError
from r3d.lit_mesh import finest_count, read_bundle_mesh


def ceiling_split(renderer):
    """(start ceiling, copies' room, rest floor); a fitted start has no ceiling."""
    ceiling = getattr(renderer.variant, 'ceiling', None)
    fit = getattr(renderer, 'fit', None)
    floor = renderer.variant.triangles if fit is None else fit.budget
    return (ceiling if fit is None else None, None if ceiling is None else ceiling - floor, floor)


def rest_target(split, copies):
    """The rest's target after paying copies; the ceiling is hard and the floor is soft."""
    start, room, floor = split
    if room is None:
        return floor
    ceiling = room + floor
    if copies > ceiling:
        raise SettingsError(f"copies total {copies} exceeds ceiling {ceiling} (room {room})")
    available = ceiling - copies
    return min(floor, available) if start is not None else available


def prototype_groups(shape, double_sided):
    """Copies sharing one stored prototype, by handedness and per-face sidedness."""
    groups = {}
    for instance, lit in shape['copies']:
        key = instance.mirrored, np.isin(lit[3], double_sided).tobytes()
        groups.setdefault(key, []).append((instance, lit))
    return groups


def stored_count(shape, double_sided):
    """Triangles emitted by every prototype of a carved shape."""
    return sum(len(copies[0][1][2]) for copies in prototype_groups(shape, double_sided).values())


def bundle_count(entries, names):
    """Finest triangles of each named stored mesh, independent of placement count."""
    return sum(finest_count(read_bundle_mesh(entries, name)) for name in sorted(set(names)))
