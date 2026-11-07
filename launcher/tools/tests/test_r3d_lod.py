"""Cluster levels (r3d/lod/levels.py), the per-frame pick (r3d/lod/pick.py) and the replay's summary
(r3d/lod/lod_replay.py)."""

import pathlib
import sys
import tempfile
import unittest
from collections import Counter

import numpy as np
from scipy.spatial.transform import Rotation

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.lod import lod_replay, pick
from r3d.lod.levels import build_levels
from r3d.poses import camera_basis

GRID = 48
CLUSTER = 32
FOCAL_PX = 100.0
NEAR = 0.1
# A lens this wide keeps the whole grid in view from every eye the tests use.
WIDE_LENS = 100.0
PLACEMENT = (2.0 * Rotation.from_euler("xyz", [20, -35, 50], degrees=True).as_matrix(), np.array([5.0, -3.0, 7.0]))


def wavy_grid(n=GRID, height=0.4, waves=(4.0, 5.0)):
    """An n x n quad grid in z = 0, bent `height` over `waves` so simplifying it costs error."""
    x, y = np.meshgrid(np.arange(n + 1, dtype=float), np.arange(n + 1, dtype=float))
    points = np.stack([x.ravel(), y.ravel(), height * np.sin(x.ravel() / waves[0]) * np.cos(y.ravel() / waves[1])], axis=1)
    corner = (np.arange(n)[None, :] + np.arange(n)[:, None] * (n + 1)).ravel()
    triangles = np.concatenate([np.stack([corner, corner + 1, corner + n + 2], 1),
                                np.stack([corner, corner + n + 2, corner + n + 1], 1)])
    return points, triangles


def clip_looking(eye, target, lens=1.0):
    """A world-to-clip matrix looking from `eye` at `target`, `lens` its half-view tangent."""
    right, up, forward = camera_basis(np.asarray(target, float) - eye)
    rows = np.array([right / lens, up / lens, forward, forward])
    return np.concatenate([rows, (-rows @ eye)[:, None]], axis=1)


def boundary_edges(levels, drawn):
    """Edges used by exactly one drawn triangle."""
    count = Counter()
    for c in np.flatnonzero(drawn):
        first = levels.cluster_first[c]
        for t in levels.indices[first:first + 3 * levels.cluster_triangles[c]].reshape(-1, 3):
            for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                count[(min(a, b), max(a, b))] += 1
    return {edge for edge, n in count.items() if n == 1}


def outline_chords(points, n=GRID):
    """Edges joining two outline vertices along the same side of the grid: a coarser level replaces a run of
    outline edges with one, which is still the outline, not a crack."""
    on = [i for i, (x, y, _z) in enumerate(points) if x in (0, n) or y in (0, n)]
    return {(a, b) for a in on for b in on if a < b and (points[a][0] == points[b][0] in (0, n)
                                                         or points[a][1] == points[b][1] in (0, n))}


def switch_points(v):
    """Every tolerance where some cluster starts or stops being drawn, and just either side of it."""
    points = np.unique(np.concatenate([v.own_px, v.group_px]))
    points = points[np.isfinite(points) & (points > 0)]
    return np.concatenate([points, np.nextafter(points, 0), np.nextafter(points, np.inf)])


class LevelsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.points, cls.triangles = wavy_grid()
        cls.levels = build_levels(cls.points, cls.triangles, CLUSTER)
        cls.placed = [pick.Placed(cls.levels, np.eye(3), np.zeros(3))]
        cls.outline = boundary_edges(cls.levels, cls.levels.cluster_refined < 0) | outline_chords(cls.points)

    def view_from(self, eye, placed=None, lens=1.0):
        """The view from `eye` towards the grid's centre, wherever `placed` puts it."""
        placed = placed or self.placed
        eye = np.asarray(eye, float)
        centre = placed[0].matrix @ [GRID / 2, GRID / 2, 0] + placed[0].position
        return pick.view(placed, eye, clip_looking(eye, centre, lens), FOCAL_PX, NEAR)

    def test_the_finest_level_is_the_input(self):
        finest = self.levels.cluster_refined < 0
        self.assertEqual(self.levels.cluster_triangles[finest].sum(), len(self.triangles))
        self.assertGreater(len(np.unique(self.levels.cluster_refined)), 2, "fewer than three levels")

    def test_errors_grow_towards_the_coarsest_level(self):
        self.assertTrue(np.all(self.levels.own_error <= self.levels.parent_error))
        self.assertTrue(np.all(self.levels.own_error[self.levels.cluster_refined < 0] == 0))
        self.assertTrue(np.isinf(self.levels.parent_error).any())

    def test_a_clusters_own_record_is_its_source_groups_parent_record(self):
        # Both sides of a switch read the same numbers, so they agree on the pixel the switch happens at.
        levels = self.levels
        for c in np.flatnonzero(levels.cluster_refined >= 0):
            source = np.flatnonzero(levels.cluster_group == levels.cluster_refined[c])
            self.assertTrue(len(source) > 0)
            np.testing.assert_array_equal(levels.parent_sphere[source], np.broadcast_to(levels.own_sphere[c], (len(source), 4)))
            np.testing.assert_array_equal(levels.parent_error[source], levels.own_error[c])

    def test_a_placed_box_holds_its_placed_corners(self):
        matrix, position = PLACEMENT
        boxes = pick.placed_boxes(self.levels.cluster_box, pick.Placed(self.levels, matrix, position))
        for c in range(0, len(boxes), 7):
            first = self.levels.cluster_first[c]
            points = self.levels.positions[self.levels.indices[first:first + 3 * self.levels.cluster_triangles[c]]]
            placed = points @ matrix.T + position
            self.assertTrue(np.all(placed >= boxes[c, 0] - 1e-9) and np.all(placed <= boxes[c, 1] + 1e-9))

    def test_every_switch_point_cuts_without_crack_or_overlap(self):
        # Placed, through pick.view, from eyes near and far, one inside the grid's spheres: the cut mixes levels.
        matrix, position = PLACEMENT
        placed = [pick.Placed(self.levels, matrix, position)]
        mixed = 0
        for local_eye in ([-2.0, -2.0, 3.0], [GRID / 2, GRID / 2, 3.0], [GRID * 3, -GRID, GRID]):
            v = self.view_from(matrix @ local_eye + position, placed, WIDE_LENS)
            self.assertEqual(len(v.triangles), len(self.levels.cluster_triangles), "a cluster left the view")
            for tau in [*switch_points(v), pick.TAU_FLOOR_PX, np.inf]:
                drawn = pick.drawn(v, tau)
                mixed += len(set(self.levels.cluster_refined[drawn])) > 1
                self.assertTrue(boundary_edges(self.levels, drawn) <= self.outline, f"a crack at tau {tau}")
                # No group is drawn both as itself and as what it was simplified into.
                both = set(self.levels.cluster_group[drawn]) & set(self.levels.cluster_refined[drawn])
                self.assertEqual(both, set(), f"a group drawn twice at tau {tau}")
        self.assertGreater(mixed, 0, "no tau mixed levels, so the test proved nothing")

    def test_a_placement_scales_errors_by_its_largest_scale(self):
        # Scale s, rotation and translation, seen from an eye: the same pixels as the unplaced mesh seen from the
        # eye taken back into its space, while the near plane is not reached.
        matrix, position = PLACEMENT
        scale = 2.0
        local_eye = np.array([GRID * 2, -GRID, GRID])
        placed = self.view_from(matrix @ local_eye + position, [pick.Placed(self.levels, matrix, position)], WIDE_LENS)
        plain = self.view_from(local_eye, lens=WIDE_LENS)
        np.testing.assert_allclose(placed.group_px, plain.group_px, rtol=1e-9)
        np.testing.assert_allclose(placed.own_px, plain.own_px, rtol=1e-9)
        stretched, largest = pick.placed_spheres(self.levels.parent_sphere, pick.Placed(
            self.levels, Rotation.from_euler("z", 30, degrees=True).as_matrix() @ np.diag([1.0, -3.0, 0.5]), position))
        self.assertAlmostEqual(largest, 3.0)
        np.testing.assert_allclose(stretched[:, 3], self.levels.parent_sphere[:, 3] * 3.0)
        self.assertAlmostEqual(np.linalg.norm(matrix, ord=2), scale)

    def test_one_view_keeps_each_placements_own_scale(self):
        eye = np.array([GRID / 2, -GRID, GRID / 2])
        one = self.view_from(eye, lens=WIDE_LENS)
        far = [pick.Placed(self.levels, np.eye(3), np.zeros(3)), pick.Placed(self.levels, 2 * np.eye(3), [0, 0, 0])]
        both = self.view_from(eye, far, WIDE_LENS)
        np.testing.assert_allclose(both.group_px[:len(one.group_px)], one.group_px)
        self.assertFalse(np.allclose(both.group_px[len(one.group_px):], one.group_px))

    def test_histogram_tau_stays_within_budget_and_one_bin_of_exact(self):
        ratio = pick.bin_edges()[1] / pick.bin_edges()[0]
        rng = np.random.default_rng(5)
        for _ in range(12):
            eye = np.array([*rng.uniform(-GRID, 2 * GRID, 2), rng.uniform(1, GRID)])
            v = self.view_from(eye, lens=WIDE_LENS)
            finest = int(v.triangles[v.finest].sum())
            for share in rng.uniform(0.1, 1.0, 4):
                budget = int(share * finest)
                exact, histogram = pick.tau_exact(v, budget), pick.tau_from_histogram(v, budget)
                if exact > pick.TAU_CEILING_PX:
                    continue
                self.assertLessEqual(pick.triangles_at(v, exact), budget)
                self.assertLessEqual(pick.triangles_at(v, histogram), budget)
                self.assertGreaterEqual(histogram, exact)
                self.assertLessEqual(histogram, exact * ratio * (1 + 1e-9))

    def test_a_partly_culled_view_still_gets_the_least_tau_within_budget(self):
        # A narrow lens culls some finer clusters whose coarser cluster's box is in view, so the count can rise
        # with tau: the exact tau is the least switch point at or above the floor within budget, found by trying
        # every one, and the histogram's lands at or after it, within budget.
        rng = np.random.default_rng(9)
        checked = 0
        for _ in range(20):
            eye = np.array([*rng.uniform(0, GRID, 2), rng.uniform(2, GRID / 2)])
            target = np.array([*rng.uniform(0, GRID, 2), 0.0])
            v = pick.view(self.placed, eye, clip_looking(eye, target, 0.2), FOCAL_PX, NEAR)
            if len(v.triangles) == 0:
                continue
            switches = np.sort(switch_points(v))
            below = [pick.triangles_at(v, tau) for tau in switches[switches < pick.TAU_FLOOR_PX]]
            taus = np.r_[pick.TAU_FLOOR_PX, switches[switches >= pick.TAU_FLOOR_PX]]
            counts = np.array([pick.triangles_at(v, tau) for tau in taus])
            # A dip below the floor is no answer: the floor's own count is what is drawn there.
            for budget in (int(counts[0] * 0.8), int(counts[0] * 0.5), min(below, default=counts[0])):
                feasible = taus[counts <= budget]
                exact, histogram = pick.tau_exact(v, budget), pick.tau_from_histogram(v, budget)
                self.assertEqual(exact, feasible.min() if len(feasible) else np.inf)
                if np.isfinite(exact) and exact <= pick.TAU_CEILING_PX:
                    self.assertGreaterEqual(histogram, exact)
                    self.assertLessEqual(pick.triangles_at(v, histogram), budget)
                    checked += 1
        self.assertGreater(checked, 10)

    def test_an_empty_view_draws_nothing_at_the_floor(self):
        eye = np.array([GRID / 2, -GRID, GRID / 2])
        empty = pick.view(self.placed, eye, clip_looking(eye, eye + [0, -1, 0]), FOCAL_PX, NEAR)
        self.assertEqual(pick.tau_exact(empty, 0), pick.TAU_FLOOR_PX)
        row = lod_replay.pose_row(empty, 0, [10])
        self.assertEqual((row["finest"], row["b10_histogram"], row["b10_exact"]), (0, 0, 0))

    def test_a_budget_below_the_coarsest_level_stops_at_the_ceiling(self):
        v = self.view_from([GRID / 2, -GRID, GRID / 2])
        coarsest = pick.triangles_at(v, np.inf)
        self.assertEqual(pick.tau_exact(v, coarsest - 1), np.inf)
        self.assertEqual(pick.tau_from_histogram(v, coarsest - 1), pick.TAU_CEILING_PX)
        self.assertGreater(pick.triangles_at(v, pick.TAU_CEILING_PX), coarsest - 1)

    def test_tau_never_drops_below_the_floor(self):
        v = self.view_from([GRID / 2, -GRID, GRID / 2])
        finest = int(v.triangles[v.finest].sum())
        for budget in (finest, 10 ** 9):
            self.assertEqual(pick.tau_exact(v, budget), pick.TAU_FLOOR_PX)
            self.assertEqual(pick.tau_from_histogram(v, budget), pick.TAU_FLOOR_PX)

    def test_at_any_tau_past_every_error_only_terminal_groups_draw(self):
        v = self.view_from([GRID / 2, -GRID, GRID / 2])
        np.testing.assert_array_equal(pick.drawn(v, np.inf), np.isinf(v.group_px))
        self.assertGreater(pick.triangles_at(v, np.inf), 0)

    def test_a_cluster_behind_the_eye_is_culled(self):
        eye = np.array([GRID / 2, -GRID, GRID / 2])
        behind = pick.view(self.placed, eye, clip_looking(eye, eye + [0, -1, 0]), FOCAL_PX, NEAR)
        self.assertGreater(len(self.view_from(eye).triangles), 0)
        self.assertEqual(len(behind.triangles), 0)


def row(time_ms, finest, budget, tau_exact, exact, histogram, terminal):
    drawn = {f"at_{tau:g}px": finest // 2 for tau in lod_replay.FIXED_TAUS_PX}
    return dict(time_ms=time_ms, finest=finest, coarsest=finest // 10, **drawn, **{
        f"b{budget}_tau_exact": tau_exact, f"b{budget}_tau_histogram": tau_exact, f"b{budget}_exact": exact,
        f"b{budget}_histogram": histogram, f"b{budget}_terminal": terminal})


class ReplayTest(unittest.TestCase):
    def test_summary_counts_only_poses_the_budget_binds(self):
        budget = 100
        rows = [row(0, 0, budget, pick.TAU_FLOOR_PX, 0, 0, 0),             # nothing in view
                row(100, 80, budget, pick.TAU_FLOOR_PX, 80, 80, 8),         # under budget: not bound
                row(200, 200, budget, 2.0, 100, 90, 9),                     # bound and steered: 10% unspent
                row(300, 400, budget, np.inf, 150, 150, 30)]                # bound, past the ceiling
        summary = dict(lod_replay.summary_rows(rows, [budget]))
        self.assertEqual(summary["poses"], "4")
        self.assertEqual(summary[f"budget {budget}: poses over it at the finest level"], "50.0%")
        self.assertEqual(summary[f"budget {budget}: poses past the {pick.TAU_CEILING_PX:g} px ceiling"], "1")
        self.assertEqual(summary[f"budget {budget}: histogram short of exact, mean / max"], "10.0% / 10.0%")
        self.assertEqual(summary[f"budget {budget}: terminal share of drawn, mean / max"], "14.7% / 20.0%")
        self.assertEqual(summary["saved at 2 px, mean / p95 pose"], "50.0% / 50.0%")

    def test_no_bound_pose_reports_a_dash(self):
        summary = dict(lod_replay.summary_rows([row(0, 50, 100, pick.TAU_FLOOR_PX, 50, 50, 0)], [100]))
        self.assertEqual(summary["budget 100: histogram short of exact, mean / max"], "-")
        self.assertEqual(summary["budget 100: exact tau on those, median / p95 px"], "-")

    def test_a_segment_holds_its_ends(self):
        rows = [dict(time_ms=t) for t in (0, 1000, 1500, 2000, 2100)]
        self.assertEqual([r["time_ms"] for r in lod_replay.in_segment(rows, (1.0, 2.0))], [1000, 1500, 2000])

    def test_copies_are_placed_under_the_renderer_after_their_own_transform(self):
        shapes = dict(rest="rest levels", chair="chair levels")
        turn = Rotation.from_euler("z", 90, degrees=True).as_matrix()
        rows = [dict(mesh="chair", matrix=turn.tolist(), position=[1.0, 0.0, 0.0])]
        renderer, offset = 2 * Rotation.from_euler("x", 90, degrees=True).as_matrix(), np.array([0.0, 0.0, 5.0])
        rest, chair = lod_replay.placed_meshes(shapes, rows, "rest", renderer, offset)
        self.assertEqual((rest.levels, chair.levels), ("rest levels", "chair levels"))
        local = np.array([1.0, 2.0, 3.0])
        np.testing.assert_allclose(chair.matrix @ local + chair.position, renderer @ (turn @ local + [1, 0, 0]) + offset)
        np.testing.assert_allclose(rest.matrix @ local + rest.position, renderer @ local + offset)

    def test_out_inside_the_repository_is_refused(self):
        with self.assertRaises(SystemExit):
            lod_replay.main(["missing.scene.toml", "--object", "x", "--out", str(lod_replay.REPO / "out")])

    def test_write_makes_its_three_files(self):
        rows = [row(t, 200, 100, 2.0, 100, 90, 9) for t in (0, 100)]
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory)
            lod_replay.write(out, rows, [100], (0, 0.05), "test")
            self.assertEqual(sorted(p.name for p in out.iterdir()), ["lod.png", "poses.csv", "summary.md"])
            table = (out / "summary.md").read_text().splitlines()
            self.assertIn("| Measure | path | 0-0.05 s |", table)
            self.assertIn("| poses | 2 | 1 |", table)


if __name__ == "__main__":
    unittest.main()
