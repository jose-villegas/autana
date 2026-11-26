"""Checks the probes that look for sky through a simplified mesh's walls (r3d/leaks.py) and the simplify passes
that lock a vertex per hole: an arch covered by a separate sheet behind it stays covered at the same budget."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import leaks, mesh_import
    from r3d import simplify as simplify_module
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
# A path view straight at the wall, its size unlike a probe face's, and a batch too small for two probe faces.
PATH_VIEW = (192, 128, 0.25)
PATH_EYE = (150.0, 150.0, 150.0)
SMALL_BATCH = 50000
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


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ViewHoleTests(unittest.TestCase):
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
        p, t = self.simplified(Allocation())
        self.assertGreater(len(self.probes.holes(p, t)), 0, "the fixture does not leak without the probes")
        p, t = self.simplified(Allocation(camera=CAMERA))
        self.assertEqual(self.probes.holes(p, t), [])
        self.assertLessEqual(len(t), BUDGET)

    def one_pass(self, budget, rows=None):
        """(result triangles, the pass's leak record) of the fixture's triangles `rows`, all of them without any,
        as one probed pass."""
        record = {}
        pos, rgb, tris, labels = self.joined
        rows = np.arange(len(tris)) if rows is None else rows
        probes = leaks.LeakProbes(pos, tris, *CAMERA)
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
        t, record = self.one_pass(BUDGET)
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

    def test_a_path_view_finds_a_hole_of_its_own_across_small_batches(self):
        inside = 2 * (ARC_SEGMENTS + ARC_SEGMENTS // 2)
        holed = self.pos, np.delete(self.tris, inside, axis=0)
        target = self.pos[self.tris[inside]].mean(axis=0)
        eye = np.array(PATH_EYE)
        pose = np.concatenate([eye, (target - eye) / np.linalg.norm(target - eye)])
        whole_batches = len(self.probes.holes(*holed))
        with mock.patch.object(leaks, "LAUNCH", SMALL_BATCH):
            without = leaks.LeakProbes(self.pos, self.tris, *CAMERA)
            with_path = leaks.LeakProbes(self.pos, self.tris, *CAMERA, path=(*PATH_VIEW, [pose]))
            self.assertEqual(len(with_path.views), len(without.views) + 1)
            self.assertEqual(with_path.holes(self.pos, self.tris), [])
            self.assertEqual(len(without.holes(*holed)), whole_batches, "a batch edge lost a view")
            self.assertGreater(len(with_path.holes(*holed)), len(without.holes(*holed)))

    def test_a_region_left_out_falls_back_to_the_sources_bounds(self):
        probes = leaks.LeakProbes(self.pos, self.tris, None, CAMERA[1])
        self.assertGreater(len(probes.views), 0)
        eyes = np.array([pose[:3] for _, _, _, pose in probes.views])
        self.assertTrue(np.all(eyes >= self.pos.min(axis=0) - 1e-6) and np.all(eyes <= self.pos.max(axis=0) + 1e-6))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SealCameraTests(unittest.TestCase):
    def scene(self, camera=True):
        # The scene camera stands anywhere, on no path; the flight is a second camera.
        component = SimpleNamespace(near_z=2.0, path=None)
        flight = SimpleNamespace(kind="camera", component=SimpleNamespace(near_z=2.0, path="the flight"))
        return SimpleNamespace(region="the region", camera=SimpleNamespace(component=component) if camera else None,
                               objects=[SimpleNamespace(kind="camera", component=component), flight])

    def test_no_scene_or_no_camera_gives_none(self):
        renderer = SimpleNamespace(seal_views=None)
        self.assertIsNone(leaks.seal_camera(None, renderer))
        self.assertIsNone(leaks.seal_camera(self.scene(camera=False), renderer))

    def test_the_region_and_near_plane_come_alone_without_seal_views(self):
        self.assertEqual(leaks.seal_camera(self.scene(), SimpleNamespace(seal_views=None)), ("the region", 2.0))
        self.assertEqual(leaks.seal_camera(self.scene(), SimpleNamespace()), ("the region", 2.0))

    def test_seal_views_add_the_paths_views_at_their_size_and_lens(self):
        views = SimpleNamespace(every_ms=33, size=(8, 6))
        with mock.patch("r3d.poses.path_poses", return_value=(8, 8, 0.8, 2.0, ["a pose"])) as sampled:
            camera = leaks.seal_camera(self.scene(), SimpleNamespace(seal_views=views))
        self.assertEqual(camera, ("the region", 2.0, (8, 8, 0.8, ["a pose"])))
        self.assertEqual(sampled.call_args.args[0].path, "the flight")
        self.assertEqual(sampled.call_args.args[1:], ((8, 6), 33))


if __name__ == "__main__":
    unittest.main()
