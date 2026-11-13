"""Checks which objects an import carves without a rule and the budgets derived for them."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.objects import budget, carve, detail
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
        budgets, shapes = carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, [[13, 0, 0]], FOCAL, NEAR)
        self.assertEqual(budgets[0], budgets[1])
        copies = next(shape for shape in shapes if shape["objects"] == [0, 1])
        self.assertAlmostEqual(copies["distance"], 2.0)
        self.assertEqual(budgets[0], copies["ladder"][0].triangles)

    def measured(self, selected, room):
        from r3d.simplify import Allocation, simplify
        pos, tri_v, tri_m, tri_o, _ = self.selected()
        _, shapes = carve.derived_budgets(pos, tri_v, tri_m, tri_o, selected, [[13, 0, 0]], FOCAL, NEAR)
        allocation = Allocation(tri_o=tri_o, object_names=['a', 'b', 'c'], selection=selected, shapes=[], report={},
            detail=SimpleNamespace(per_object={obj: shape for shape in shapes for obj in shape['objects']}, focal=FOCAL),
            split=(room, room, 0))
        simplify(pos, np.full_like(pos, 100), tri_v, tri_m, 1, colour_deviation=1, allocation=allocation)
        return allocation.report

    def test_a_ceiling_that_binds_steps_shapes_down_counting_each_once(self):
        *_, selected = self.selected()
        free = self.measured(selected, None)['stored_total']
        tight = self.measured(selected, free - 1)['stored_total']
        self.assertLessEqual(tight, free - 1)
        self.assertEqual(self.measured(selected, free)['stored_total'], free)

    def test_a_pinned_rule_keeps_its_budget_and_takes_its_share_of_the_room(self):
        *_, selected = self.selected()
        selected[2] = {'match': {'object': 'c'}, 'triangles': 50}
        free = self.measured(selected, None)
        pinned = next(row for row in free['passes'] if row['objects'] == ['c'])
        self.assertEqual(pinned['budget'], 50)
        tight = self.measured(selected, free['stored_total'] - 1)
        self.assertLess(tight['stored_total'], free['stored_total'])
        self.assertEqual(next(row for row in tight['passes'] if row['objects'] == ['c'])['budget'], 50)


@unittest.skipIf(np is None, "needs the r3d environment")
class CarveWithinTest(unittest.TestCase):
    def test_the_carve_adds_triangles_until_its_measured_move_fits(self):
        from r3d.meshopt import PERMISSIVE, REGULARIZE
        from r3d.simplify import stand_in_within

        positions, tris = sphere()
        rgb = np.where(positions[:, :1] > 0, 255.0, 0.0).repeat(3, axis=1)
        materials = np.zeros(len(tris), dtype=int)
        options = REGULARIZE | PERMISSIVE
        error = detail.object_error(2.0, FOCAL)
        floor = len(detail.level(positions, tris, error))
        _, _, kept, unbounded = stand_in_within(positions, rgb, tris, materials, floor, 2.0, options, None, None)
        self.assertEqual(kept, floor)
        _, _, budget, moved = stand_in_within(positions, rgb, tris, materials, floor, 2.0, options, None, error)
        self.assertLessEqual(moved, error)
        self.assertGreaterEqual(budget, floor)
        if unbounded > error:
            self.assertGreater(budget, floor)


@unittest.skipIf(np is None, "needs the r3d environment")
class DetailReportTest(unittest.TestCase):
    def test_each_shape_s_surface_move_is_reported_in_pixels_at_its_nearest_view(self):
        rungs = [detail.Rung(100, 2.0), detail.Rung(40, 4.0)]
        shapes = [dict(objects=[0, 1], distance=2.0, ladder=rungs, pick=1, triangles=np.zeros((120, 3)))]
        passes = [dict(objects=["b", "a"], surface_error=0.05, output_triangles=48)]
        rows, authoring = carve.detail_report(shapes, passes, ["a", "b"], FOCAL)
        self.assertEqual((rows[0]["rung_triangles"], rows[0]["carved_triangles"]), (40, 48))
        self.assertAlmostEqual(rows[0]["error_px"], 0.05 * FOCAL / 2.0)
        self.assertEqual(authoring, [dict(shape="a", error_px=rows[0]["error_px"], reason="ceiling")])



@unittest.skipIf(np is None, "needs the r3d environment")
class ViewsTest(unittest.TestCase):
    def test_a_renderer_that_samples_no_camera_path_has_no_eyes_and_sees_from_the_near_plane(self):
        camera = SimpleNamespace(path=object(), half_fov_short_tan=0.5, near_z=NEAR)
        path_scene = SimpleNamespace(camera=SimpleNamespace(component=camera))
        for visibility in (SimpleNamespace(source="camera_region", rounds=4), None):
            eyes, focal, near = carve.views(path_scene, SimpleNamespace(visibility=visibility))
            self.assertEqual((len(eyes), near), (0, NEAR))
            self.assertGreater(focal, 0.0)


@unittest.skipIf(np is None, "needs the r3d environment")
class CeilingSplitTest(unittest.TestCase):
    """The variant ceiling is one budget: without a fit it caps the baked mesh; with one, the fitted bundle, and
    the fit's budget is the least its rest keeps."""
    CEILING, TRIANGLES, FLOOR = 34500, 33178, 18678

    def renderer(self, fit):
        variant = SimpleNamespace(ceiling=self.CEILING, triangles=self.TRIANGLES)
        return SimpleNamespace(variant=variant, fit=SimpleNamespace(budget=self.FLOOR) if fit else None)

    def test_without_a_fit_the_ceiling_caps_the_bake_and_the_copies_get_what_the_variant_leaves(self):
        renderer = self.renderer(fit=False)
        self.assertEqual(budget.ceiling_split(renderer), (self.CEILING, self.CEILING - self.TRIANGLES, self.TRIANGLES))

    def test_with_a_fit_the_start_is_uncapped_and_the_copies_get_what_the_floor_leaves(self):
        renderer = self.renderer(fit=True)
        self.assertEqual(budget.ceiling_split(renderer), (None, self.CEILING - self.FLOOR, self.FLOOR))

    def test_the_fit_prunes_to_the_ceiling_less_the_copies_with_a_soft_floor(self):
        split = budget.ceiling_split(self.renderer(fit=True))
        self.assertEqual(budget.rest_target(split, 14449), self.CEILING - 14449)
        self.assertEqual(budget.rest_target(split, self.CEILING - 1), 1)
        unbounded = self.renderer(fit=True)
        unbounded.variant.ceiling = None
        self.assertEqual(budget.rest_target(budget.ceiling_split(unbounded), 14449), self.FLOOR)

    def test_without_a_ceiling_nothing_caps_the_copies(self):
        renderer = self.renderer(fit=True)
        renderer.variant.ceiling = None
        self.assertIsNone(budget.ceiling_split(renderer)[1])

if __name__ == "__main__":
    unittest.main()
