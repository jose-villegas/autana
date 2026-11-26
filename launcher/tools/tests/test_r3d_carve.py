"""Checks which objects an import carves without a rule and the budgets derived for them."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.objects import budget, carve, detail
    from test_r3d_detail import sphere
except ImportError:
    np = None

SCALE = 4  # position ticks per unit: the seal tolerance is a quarter unit


def scene(*offsets):
    """One sphere object per offset, all of material 0: (positions, tri_v, tri_m, tri_o, names)."""
    positions, tris = sphere()
    pos, tri_v, tri_o = [], [], []
    for index, offset in enumerate(offsets):
        tri_v.append(tris + len(positions) * index)
        pos.append(positions + offset)
        tri_o.append(np.full(len(tris), index))
    tri_v = np.concatenate(tri_v)
    return (np.concatenate(pos), tri_v, np.zeros(len(tri_v), dtype=int), np.concatenate(tri_o),
            [f"ball_{i}" for i in range(len(offsets))])


def odd_scene():
    """Two free copies of a sphere, then a lone larger sphere touching neither."""
    pos, tri_v, tri_m, tri_o, names = scene([0, 0, 0], [10, 0, 0])
    big, tris = sphere(radius=2.0)
    return (np.concatenate([pos, big + [0, 10, 0]]), np.concatenate([tri_v, tris + len(pos)]),
            np.concatenate([tri_m, np.zeros(len(tris), dtype=int)]), np.concatenate([tri_o, np.full(len(tris), 2)]),
            names + ["lone"])


@unittest.skipIf(np is None, "needs the r3d environment")
class FreeRepeatTest(unittest.TestCase):
    def test_copies_that_touch_nothing_are_carved_and_a_lone_object_is_not(self):
        pos, tri_v, tri_m, tri_o, names = odd_scene()
        found = carve.free_repeats(pos, tri_v, tri_m, tri_o, names, {}, SCALE)
        self.assertEqual(sorted(found), [0, 1])
        self.assertTrue(all(rule is carve.FREE_REPEAT for rule in found.values()))

    def test_a_shape_with_one_touching_copy_stays_in_the_rest(self):
        pos, tri_v, tri_m, tri_o, names = scene([0, 0, 0], [10, 0, 0], [12, 0, 0])
        self.assertEqual(carve.touching(pos, tri_v, tri_o, {0, 1, 2}, SCALE), {1, 2})
        self.assertEqual(carve.free_repeats(pos, tri_v, tri_m, tri_o, names, {}, SCALE), {})

    def test_objects_a_rule_carves_are_left_to_it(self):
        pos, tri_v, tri_m, tri_o, names = scene([0, 0, 0], [10, 0, 0], [20, 0, 0])
        rule = {"match": {"object": "ball_0"}}
        self.assertEqual(sorted(carve.free_repeats(pos, tri_v, tri_m, tri_o, names, {0: rule}, SCALE)), [1, 2])

    def test_free_copies_with_an_alpha_tested_material_stay_in_the_rest(self):
        pos, tri_v, tri_m, tri_o, names = scene([0, 0, 0], [10, 0, 0])
        per_copy = len(tri_m) // 2
        tri_m = np.where(np.arange(len(tri_m)) % per_copy % 7 == 0, 1, 0)  # each copy: the same few leaf cards
        self.assertEqual(sorted(carve.free_repeats(pos, tri_v, tri_m, tri_o, names, {}, SCALE, masked=[0])), [])
        self.assertEqual(sorted(carve.free_repeats(pos, tri_v, tri_m, tri_o, names, {}, SCALE, masked=[1])), [])
        self.assertEqual(sorted(carve.free_repeats(pos, tri_v, tri_m, tri_o, names, {}, SCALE, masked=[2])), [0, 1])

    def test_a_material_is_alpha_tested_when_its_texture_cuts_a_texel(self):
        from r3d.obj import ALPHA_OPAQUE, alpha_tested

        def texture(alpha):
            return SimpleNamespace(levels=[np.dstack([np.ones((2, 2, 3)), np.full((2, 2), alpha)])])
        opaque, cut = texture(1.0), texture(1.0)
        cut.levels[0][0, 0, 3] = ALPHA_OPAQUE
        self.assertEqual(alpha_tested([None, opaque, cut]).tolist(), [False, False, True])


@unittest.skipIf(np is None, "needs the r3d environment")
class CarveWithinTest(unittest.TestCase):
    def test_the_carve_adds_triangles_until_its_measured_move_fits(self):
        from r3d.geometry import bounds_diagonal
        from r3d.meshopt import PERMISSIVE, REGULARIZE
        from r3d.objects.allocate import FLOOR_SHARE
        from r3d.simplify import stand_in_within

        positions, tris = sphere()
        rgb = np.where(positions[:, :1] > 0, 255.0, 0.0).repeat(3, axis=1)
        materials = np.zeros(len(tris), dtype=int)
        options = REGULARIZE | PERMISSIVE
        error = FLOOR_SHARE * bounds_diagonal(positions)
        floor = len(detail.level(positions, tris, error))
        _, _, kept, unbounded = stand_in_within(positions, rgb, tris, materials, floor, 2.0, options, None, None)
        self.assertEqual(kept, floor)
        _, _, budget, moved = stand_in_within(positions, rgb, tris, materials, floor, 2.0, options, None, error)
        self.assertLessEqual(moved, error)
        self.assertGreaterEqual(budget, floor)
        if unbounded > error:
            self.assertGreater(budget, floor)


if __name__ == "__main__":
    unittest.main()
