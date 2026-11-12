"""Checks which objects an import carves without a rule and the budgets derived for them."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.objects import carve, detail
    from test_r3d_detail import FOCAL, NEAR, sphere
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


@unittest.skipIf(np is None, "needs the r3d environment")
class DerivedBudgetTest(unittest.TestCase):
    def selected(self):
        pos, tri_v, tri_m, tri_o, names = odd_scene()
        rule = {"match": {"material": "m"}}
        return pos, tri_v, tri_m, tri_o, {0: rule, 1: rule, 2: rule}

    def test_copies_share_one_budget_from_the_closest_view_of_any(self):
        pos, tri_v, tri_m, tri_o, selected = self.selected()
        budgets, shapes = carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, None, [[13, 0, 0]], FOCAL, NEAR)
        self.assertEqual(budgets[0], budgets[1])
        copies = next(shape for shape in shapes if shape["objects"] == [0, 1])
        self.assertAlmostEqual(copies["distance"], 2.0)
        self.assertEqual(budgets[0], copies["ladder"][0].triangles)

    def test_a_ceiling_that_binds_steps_shapes_down_counting_each_once(self):
        pos, tri_v, tri_m, tri_o, selected = self.selected()
        free, shapes = carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, None, [[13, 0, 0]], FOCAL, NEAR)
        stored = free[0] + free[2]
        tight, _ = carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, stored - 1, [[13, 0, 0]], FOCAL, NEAR)
        self.assertLessEqual(tight[0] + tight[2], stored - 1)
        self.assertEqual(carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, stored, [[13, 0, 0]], FOCAL,
                                               NEAR)[0], free)

    def test_a_pinned_rule_keeps_its_budget_and_takes_its_share_of_the_room(self):
        pos, tri_v, tri_m, tri_o, selected = self.selected()
        pinned = {"match": {"object": "lone"}, "triangles": 50}
        selected = selected | {2: pinned}
        free, _ = carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, None, [[13, 0, 0]], FOCAL, NEAR)
        self.assertNotIn(2, free)
        room = free[0] + 50
        self.assertEqual(carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, room, [[13, 0, 0]], FOCAL,
                                               NEAR)[0], free)
        tight, _ = carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, room - 1, [[13, 0, 0]], FOCAL, NEAR)
        self.assertLess(tight[0], free[0])


@unittest.skipIf(np is None, "needs the r3d environment")
class DetailReportTest(unittest.TestCase):
    def test_each_shape_s_surface_move_is_reported_in_pixels_at_its_nearest_view(self):
        rungs = [detail.Rung(100, 2.0), detail.Rung(40, 4.0)]
        shapes = [dict(objects=[0, 1], distance=2.0, ladder=rungs, pick=1)]
        passes = [dict(objects=["b", "a"], surface_error=0.05)]
        rows, authoring = carve.detail_report(shapes, passes, ["a", "b"], FOCAL)
        self.assertEqual(rows[0]["kept_triangles"], 40)
        self.assertAlmostEqual(rows[0]["error_px"], 0.05 * FOCAL / 2.0)
        self.assertEqual(authoring, [dict(shape="a", error_px=rows[0]["error_px"], reason="ceiling")])


if __name__ == "__main__":
    unittest.main()
