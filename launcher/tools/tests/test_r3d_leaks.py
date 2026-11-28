"""Checks the probes that look for sky through a simplified mesh's walls (r3d/leaks.py) and the simplify passes
that lock a vertex per hole: an arch covered by a separate sheet behind it stays covered at the same budget."""

import pathlib
import sys
import unittest
from contextlib import contextmanager
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import leaks
    from r3d import simplify as simplify_module
    from r3d.import_settings import seal_camera
    from r3d.meshopt import PERMISSIVE, REGULARIZE_LIGHT
    from r3d.repair import repair, seal_tolerance
    from r3d.simplify import Allocation, simplify
except ImportError:
    np = None

from tests.r3d_env import needs_llvm  # noqa: E402

# A wall with a half-round opening and, a little behind it, a half disc slightly wider than the opening: two
# pieces that cover the wall only by overlapping, as an arch ring in front of a vault's end does.
OPENING_RADIUS = 100.0
SHEET_BEHIND = 7.6
OVERLAP = 1.0
ARC_SEGMENTS = 64
RINGS = 6
WALL_HALF_WIDTH, WALL_HEIGHT = 200.0, 300.0
# Too few triangles to keep the half disc round: its chords pull inside the opening.
BUDGET = 24
# Too few triangles for the half disc alone to stay round: its locks stop at the budget with holes left.
SHEET_ALONE_BUDGET = 24
# A budget for the wall and the half disc simplified as two passes, and the half disc's share of it.
TWO_PASS_BUDGET, SHEET_SHARE = 96, 0.5
# One cube face per batch.
SMALL_BATCH = leaks.probes.PROBE_FACE ** 2 if np is not None else None
# Quantisation steps per source unit, which sets the join step's tolerance.
POSITION_SCALE = 8
# The half disc carved out as a mesh of its own, at too few triangles to stay round.
CARVED_SHEET_BUDGET = 6
# Where the camera can be, in front of the wall, and its near plane.
CAMERA = ((np.array([60.0, 10.0, -90.0]), np.array([260.0, 110.0, 90.0])), 2.0) if np is not None else None


def strip(inner, outer):
    """Triangles between two rows of points, RINGS quads deep."""
    rows = [inner + (outer - inner) * k / RINGS for k in range(RINGS + 1)]
    n = len(inner)
    tris = [tri for k in range(RINGS) for i in range(n - 1)
            for tri in ((k * n + i, k * n + i + 1, (k + 1) * n + i + 1), (k * n + i, (k + 1) * n + i + 1, (k + 1) * n + i))]
    return np.concatenate(rows), np.array(tris)


def arch():
    """(positions, triangles): the wall at x = 0, then the half disc, facing +x."""
    angle = np.linspace(0.0, np.pi, ARC_SEGMENTS + 1)
    unit = np.stack([np.zeros_like(angle), np.sin(angle), np.cos(angle)], axis=1)
    reach = np.minimum(WALL_HALF_WIDTH / np.maximum(np.abs(unit[:, 2]), 1e-9), WALL_HEIGHT / np.maximum(unit[:, 1], 1e-9))
    wall, wall_tris = strip(unit * OPENING_RADIUS, unit * reach[:, None])
    behind = np.array([-SHEET_BEHIND, 0.0, 0.0])
    disc, disc_tris = strip(unit * 1e-3 + behind, unit * (OPENING_RADIUS + OVERLAP) + behind)
    pos, tris = np.concatenate([wall, disc]), np.concatenate([wall_tris, disc_tris + len(wall)])
    backward = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])[:, 0] < 0
    tris[backward] = tris[backward][:, ::-1]
    return pos, tris


@contextmanager
def opening_camera():
    # Isolate the arch from the outer silhouette with a probe face aimed head-on.
    half_height = OPENING_RADIUS / 2
    eye_x = OPENING_RADIUS
    pose = (eye_x, half_height, 0, -1, 0, 0)
    lens = half_height / (eye_x + SHEET_BEHIND)
    with mock.patch.object(leaks.probes, "region_poses", return_value=([pose], None)), \
         mock.patch.object(leaks.probes, "FACE_LENS", lens):
        yield CAMERA


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PinTests(unittest.TestCase):
    def setUp(self):
        self.pos = np.array([(0, 0, 0), (4, 0, 0), (0, 4, 0), (4, 4, 3), (0, 0, 9)], dtype=float)
        self.tris = np.array([(0, 1, 2), (1, 3, 2), (0, 2, 4)])
        self.result = np.array([(0, 0, 0), (4, 0, 0), (0, 4, 0)], dtype=float), np.array([(0, 1, 2)])

    def test_a_hole_is_answered_by_its_vertex_farthest_from_the_result(self):
        free = np.ones(len(self.pos), dtype=bool)
        np.testing.assert_array_equal(leaks.pins(self.pos, self.tris, [np.array([1])], *self.result, free), [3])
        # The farthest first: vertex 4 stands 9 off the result, vertex 3 only 3.
        np.testing.assert_array_equal(leaks.pins(self.pos, self.tris, [np.array([1]), np.array([2])], *self.result, free),
                                      [4, 3])

    def test_holes_that_choose_one_vertex_lock_it_once(self):
        free = np.ones(len(self.pos), dtype=bool)
        picked = leaks.pins(self.pos, self.tris, [np.array([1]), np.array([1]), np.array([2])], *self.result, free)
        np.testing.assert_array_equal(picked, [4, 3])

    def test_a_locked_vertex_is_passed_over_and_a_hole_with_none_free_gets_none(self):
        free = np.ones(len(self.pos), dtype=bool)
        free[3] = False
        picked = leaks.pins(self.pos, self.tris, [np.array([1])], *self.result, free)
        self.assertEqual(len(picked), 1)
        self.assertIn(picked[0], (1, 2))
        free[:] = False
        self.assertEqual(len(leaks.pins(self.pos, self.tris, [np.array([1])], *self.result, free)), 0)

    def test_a_pass_simplified_to_nothing_still_pins_each_hole_a_free_vertex(self):
        free = np.ones(len(self.pos), dtype=bool)
        nothing = np.zeros((0, 3)), np.zeros((0, 3), dtype=np.int64)
        # No result surface to measure from: every free vertex is as far as any, so the lowest stands.
        picked = leaks.pins(self.pos, self.tris, [np.array([1]), np.array([2])], *nothing, free)
        np.testing.assert_array_equal(picked, [0, 1])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ViewHoleTests(unittest.TestCase):
    def test_joined_sky_is_a_hole_only_past_the_reach(self):
        first_hole = leaks.REACH + 1
        source_sky = np.zeros((first_hole + 2, first_hole + 2), dtype=bool)
        source_sky[0, 0] = True
        sky = source_sky.copy()
        np.fill_diagonal(sky, True)
        expected = np.zeros_like(sky)
        np.fill_diagonal(expected[first_hole:, first_hole:], True)

        holes = leaks.view_holes(sky, source_sky)
        self.assertEqual(len(holes), 1)
        np.testing.assert_array_equal(holes[0], expected)

    def test_no_reach_counts_only_sky_separate_from_the_source_sky(self):
        first_hole = leaks.REACH + 1
        source_sky = np.zeros((first_hole + 2, first_hole + 2), dtype=bool)
        source_sky[0, 0] = True
        sky = source_sky.copy()
        np.fill_diagonal(sky, True)
        enclosed = np.zeros_like(sky)
        enclosed[-1, 0] = True
        sky |= enclosed

        holes = leaks.view_holes(sky, source_sky, reach=None)
        self.assertEqual(len(holes), 1)
        np.testing.assert_array_equal(holes[0], enclosed)

    def test_outline_movement_at_or_below_the_reach_is_allowed(self):
        source_sky = np.zeros((leaks.REACH + 1, 2), dtype=bool)
        source_sky[0] = True
        sky = source_sky.copy()
        sky[1:, 0] = True

        for reach in (leaks.REACH, leaks.REACH + 1):
            with self.subTest(reach=reach):
                self.assertEqual(leaks.view_holes(sky, source_sky, reach=reach), [])

    def test_outline_shrinking_inward_past_the_reach_leaves_a_hole(self):
        first_hole = leaks.REACH + 1
        source_sky = np.zeros((first_hole + 2, 2), dtype=bool)
        source_sky[0] = True
        sky = source_sky.copy()
        sky[:first_hole + 1] = True
        expected = np.zeros_like(sky)
        expected[first_hole] = True

        holes = leaks.view_holes(sky, source_sky)
        self.assertEqual(len(holes), 1)
        np.testing.assert_array_equal(holes[0], expected)

    def test_enclosed_pixel_is_a_hole_at_any_reach(self):
        source_sky = np.zeros((3, 3), dtype=bool)
        source_sky[0, 0] = True
        expected = np.zeros_like(source_sky)
        expected[-1, -1] = True
        sky = source_sky | expected

        for reach in (0, leaks.REACH):
            with self.subTest(reach=reach):
                holes = leaks.view_holes(sky, source_sky, reach=reach)
                self.assertEqual(len(holes), 1)
                np.testing.assert_array_equal(holes[0], expected)

    def test_sky_enclosed_by_wall_is_a_hole_and_sky_that_reaches_the_sources_sky_is_not(self):
        source_sky = np.zeros((6, 8), dtype=bool)
        source_sky[0] = True  # the source's own sky along the top
        sky = source_sky.copy()
        sky[1:3, 5] = True  # the outline moved: joined to the source's sky
        sky[4, 1:3] = True  # a hole in the wall
        holes = leaks.view_holes(sky, source_sky)
        self.assertEqual(len(holes), 1)
        self.assertEqual(sorted(zip(*np.nonzero(holes[0]))), [(4, 1), (4, 2)])

    def test_pixels_touching_at_a_corner_are_one_hole_and_apart_are_two(self):
        none = np.zeros((5, 5), dtype=bool)
        sky = none.copy()
        sky[1, 1] = sky[2, 2] = True
        self.assertEqual(len(leaks.view_holes(sky, none)), 1)
        sky[2, 2], sky[3, 3] = False, True
        self.assertEqual(len(leaks.view_holes(sky, none)), 2)

    def test_probe_holes_and_restore_views_use_the_requested_reach(self):
        narrow_reach = leaks.REACH - 1
        width, height = 3, leaks.REACH + 1
        source_sky = np.zeros((height, width), dtype=bool)
        source_sky[0] = True
        sky = source_sky.copy()
        sky[:, 1] = True
        source_row, rim_row = 0, 1
        source_seen = np.full(sky.size, source_row)
        fitted_seen = np.full(sky.size, rim_row)
        rays = np.zeros((sky.size, 3))
        probes = leaks.LeakProbes.__new__(leaks.LeakProbes)
        probes.source_sky = [source_sky.ravel()]
        reference_depth = np.arange(sky.size)
        probes.reference_depth = [reference_depth]
        probes.query = mock.Mock()
        probes.query.first_hits.return_value = (~source_sky.ravel(), None, source_seen)
        views = [(width, height, None, None)]
        with mock.patch.object(probes, "_batches", return_value=[(views, rays, rays)]), \
                mock.patch.object(leaks, "RayQuery") as ray_query:
            result = ray_query.return_value
            result.blocked.return_value = ~sky.ravel()
            result.first_hits.return_value = (~sky.ravel(), None, fitted_seen)
            self.assertEqual(probes.holes(None, None), [])
            found = probes.holes(None, None, reach=narrow_reach)
            self.assertEqual([rows.tolist() for rows in found], [[source_row]])
            self.assertEqual(list(probes.restore_views(result, 1, leaks.REACH)), [])
            (_, _, depth, [(rim, seen)]), = probes.restore_views(result, 1, narrow_reach)
            np.testing.assert_array_equal(depth, reference_depth.reshape(height, width))
            np.testing.assert_array_equal(rim, [rim_row])
            np.testing.assert_array_equal(seen, [source_row])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SeeThroughCountTests(unittest.TestCase):
    def test_counts_farther_and_missing_hits_in_a_full_region(self):
        side = leaks.OPENING
        source = np.ones((side, side))
        fit = np.full_like(source, np.nextafter(leaks.BEHIND, np.inf))
        fit[0, 0] = np.inf
        self.assertEqual(leaks.see_through_count(source, fit), source.size)
        fit[:] = np.nextafter(leaks.BEHIND, 0)
        self.assertEqual(leaks.see_through_count(source, fit), 0)
        source[:] = np.inf
        fit[:] = np.inf
        self.assertEqual(leaks.see_through_count(source, fit), 0)

    def test_discards_a_one_pixel_sliver(self):
        side = leaks.OPENING
        source = np.ones((side, side))
        fit = source.copy()
        fit[:, 0] = np.inf
        self.assertEqual(leaks.see_through_count(source, fit), 0)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class LeakProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pos, cls.tris = arch()
        cls.probes = leaks.LeakProbes(cls.pos, cls.tris, *CAMERA)
        # What simplify() hands its passes: the fixture after the join step, and the probes over that.
        rgb = np.full((len(cls.pos), 3), 128.0)
        cls.joined = repair(cls.pos, rgb, cls.tris, np.zeros(len(cls.tris), dtype=int), seal_tolerance(POSITION_SCALE))

    def simplified(self, allocation, budget=BUDGET):
        rgb = np.full((len(self.pos), 3), 128.0)
        p, _, t, _ = simplify(self.pos, rgb, self.tris, np.zeros(len(self.tris), dtype=int), budget, colour_deviation=1.0,
                              seal_seams=True, position_scale=POSITION_SCALE, allocation=allocation)
        return p, t

    def test_the_source_shows_no_hole_and_a_missing_triangle_inside_the_wall_is_one(self):
        self.assertEqual(self.probes.holes(self.pos, self.tris), [])
        # The wall's strips run from the opening outwards; a triangle of the second ring has wall all round it.
        inside = 2 * (ARC_SEGMENTS + ARC_SEGMENTS // 2)
        holes = self.probes.holes(self.pos, np.delete(self.tris, inside, axis=0))
        self.assertGreater(len(holes), 0)
        self.assertTrue(all(set(seen) == {inside} for seen in holes))

    def test_the_sheet_behind_an_opening_keeps_it_covered_at_the_same_budget(self):
        with opening_camera() as camera:
            probes = leaks.LeakProbes(self.pos, self.tris, *camera)
            p, t = self.simplified(Allocation())
            self.assertGreater(len(probes.holes(p, t)), 0, "the fixture does not leak without the probes")
            p, t = self.simplified(Allocation(camera=camera))
            self.assertEqual(probes.holes(p, t), [])
            self.assertLessEqual(len(t), BUDGET)

    def one_pass(self, budget, rows=None, *, camera=CAMERA):
        """(result triangles, the pass's leak record) of the fixture's triangles `rows`, all of them without any,
        as one probed pass."""
        record = {}
        pos, rgb, tris, labels = self.joined
        rows = np.arange(len(tris)) if rows is None else rows
        probes = leaks.LeakProbes(pos, tris, *camera)
        _, _, t, _ = simplify_module._run_pass(pos, rgb, tris[rows], labels[rows], budget, 1.0, REGULARIZE_LIGHT | PERMISSIVE,
                                               leaks=(probes, rows), record=record)
        return t, record

    def test_locks_stop_at_the_budget_and_the_holes_left_are_recorded(self):
        sheet = np.arange(len(self.tris) // 2, len(self.tris))
        t, record = self.one_pass(SHEET_ALONE_BUDGET, sheet)
        self.assertLessEqual(len(t), SHEET_ALONE_BUDGET)
        self.assertGreater(record["leak_holes"], 0, "the budget never stopped the locks: the fixture proves nothing")
        self.assertLess(record["leak_rounds"], leaks.ROUNDS)
        self.assertGreater(record["leak_locks"], 0, "locks that fit the budget were not taken")
        with mock.patch.object(leaks, "ROUNDS", 0):
            self.assertGreater(self.one_pass(SHEET_ALONE_BUDGET, sheet)[1]["leak_holes"], record["leak_holes"])

    def test_a_sealed_pass_records_its_rounds_and_locks(self):
        with opening_camera() as camera:
            _, record = self.one_pass(BUDGET, camera=camera)
        self.assertEqual(record["leak_holes"], 0)
        self.assertGreater(record["leak_rounds"], 0)
        self.assertGreater(record["leak_locks"], 0)

    def test_with_no_round_left_the_first_run_stands_and_its_holes_are_recorded(self):
        plain = self.simplified(Allocation())
        with mock.patch.object(leaks, "ROUNDS", 0):
            t, record = self.one_pass(BUDGET)
        self.assertEqual(len(t), len(plain[1]))
        self.assertEqual((record["leak_rounds"], record["leak_locks"]), (0, 0))
        self.assertEqual(record["leak_holes"], len(leaks.LeakProbes(self.joined[0], self.joined[2], *CAMERA).holes(*plain)))

    def test_the_wall_and_the_sheet_as_two_passes_close_what_each_can_within_their_budgets(self):
        """The sheet's pass comes first and is sealed against the wall's source; the wall's pass then sees the
        sheet as simplified, and cannot lock the sheet's vertices."""
        labels = (np.arange(len(self.tris)) >= len(self.tris) // 2).astype(int)
        rgb = np.full((len(self.pos), 3), 128.0)

        def two_passes(allocation):
            p, _, t, _ = simplify(self.pos, rgb, self.tris, labels, TWO_PASS_BUDGET, [({1}, SHEET_SHARE)],
                                  colour_deviation=1.0, seal_seams=True, position_scale=POSITION_SCALE, allocation=allocation)
            return p, t

        plain = len(self.probes.holes(*two_passes(Allocation())))
        self.assertGreater(plain, 0, "the fixture does not leak in two passes")
        p, t = two_passes(Allocation(camera=CAMERA))
        self.assertLess(len(self.probes.holes(p, t)), plain / 4)
        self.assertLessEqual(len(t), TWO_PASS_BUDGET)

    def test_a_carved_mesh_stands_as_its_stand_in_for_the_rest(self):
        """The sheet is carved to a budget that cannot cover the opening; the wall's pass must see that sheet, not
        the source's, so it is the stand-in's triangles that the probes are given."""
        owners = (np.arange(len(self.tris)) >= len(self.tris) // 2).astype(int)
        rgb = np.full((len(self.pos), 3), 128.0)
        rule = dict(triangles=CARVED_SHEET_BUDGET, seam="free", match=dict(object="sheet"))
        allocation = Allocation(tri_o=owners, object_names=("wall", "sheet"), material_names=("stone",), shapes=[],
                                meshes=(rule,), selection={1: rule}, camera=CAMERA)
        finished = []
        real = leaks.LeakProbes.finish
        with mock.patch.object(leaks.LeakProbes, "finish", autospec=True,
                               side_effect=lambda probes, rows, p, t: (finished.append((rows, len(t))), real(probes, rows, p, t))[1]):
            simplify(self.pos, rgb, self.tris, np.zeros(len(self.tris), dtype=int), BUDGET, colour_deviation=1.0,
                     seal_seams=True, position_scale=POSITION_SCALE, allocation=allocation)
        sheet = [count for rows, count in finished if np.all(owners[rows] == 1)]
        self.assertEqual(len(sheet), 1, "the carved sheet never replaced its source in the probes' scene")
        self.assertLessEqual(sheet[0], CARVED_SHEET_BUDGET)
        self.assertEqual(len(allocation.shapes), 1)

    def test_the_same_input_seals_to_the_same_mesh(self):
        first, second = self.simplified(Allocation(camera=CAMERA)), self.simplified(Allocation(camera=CAMERA))
        np.testing.assert_array_equal(first[0], second[0])
        np.testing.assert_array_equal(first[1], second[1])

    def test_an_unsealed_import_builds_no_probes_even_with_a_camera(self):
        rgb = np.full((len(self.pos), 3), 128.0)
        with mock.patch.object(leaks, "LeakProbes", side_effect=AssertionError("probes built")):
            simplify(self.pos, rgb, self.tris, np.zeros(len(self.tris), dtype=int), BUDGET, colour_deviation=1.0,
                     position_scale=POSITION_SCALE, allocation=Allocation(camera=CAMERA))
            self.simplified(Allocation())

    def test_region_probes_find_the_same_holes_across_small_batches(self):
        inside = 2 * (ARC_SEGMENTS + ARC_SEGMENTS // 2)
        holed = self.pos, np.delete(self.tris, inside, axis=0)
        whole_batches = len(self.probes.holes(*holed))
        with mock.patch.object(leaks, "LAUNCH", SMALL_BATCH):
            probes = leaks.LeakProbes(self.pos, self.tris, *CAMERA)
            self.assertEqual(len(probes.holes(*holed)), whole_batches, "a batch edge lost a view")

    def test_a_region_left_out_falls_back_to_the_sources_bounds(self):
        probes = leaks.LeakProbes(self.pos, self.tris, None, CAMERA[1])
        self.assertGreater(len(probes.views), 0)
        eyes = np.array([pose[:3] for _, _, _, pose in probes.views])
        self.assertTrue(np.all(eyes >= self.pos.min(axis=0) - 1e-6) and np.all(eyes <= self.pos.max(axis=0) + 1e-6))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SealCameraTests(unittest.TestCase):
    def scene(self, camera=True):
        component = SimpleNamespace(near_z=CAMERA[1])
        return SimpleNamespace(region=CAMERA[0], camera=SimpleNamespace(component=component) if camera else None)

    def test_no_scene_or_no_camera_gives_none(self):
        self.assertIsNone(seal_camera(None))
        self.assertIsNone(seal_camera(self.scene(camera=False)))

    def test_the_seal_uses_the_scenes_region_and_near_plane(self):
        self.assertEqual(seal_camera(self.scene()), CAMERA)


if __name__ == "__main__":
    unittest.main()
