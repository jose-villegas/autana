"""Cluster levels (r3d/lod/levels.py) and the per-frame pick (r3d/lod/pick.py)."""

import pathlib
import sys
import unittest
from collections import Counter

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.lod import pick
from r3d.lod.levels import build_levels
from r3d.poses import camera_basis

GRID = 48
CLUSTER = 32
FOCAL_PX = 100.0
NEAR = 0.1


def wavy_grid(n=GRID):
    """An n x n quad grid in z = 0, bent a little so simplifying it costs error."""
    x, y = np.meshgrid(np.arange(n + 1, dtype=float), np.arange(n + 1, dtype=float))
    points = np.stack([x.ravel(), y.ravel(), 0.4 * np.sin(x.ravel() / 4) * np.cos(y.ravel() / 5)], axis=1)
    corner = (np.arange(n)[None, :] + np.arange(n)[:, None] * (n + 1)).ravel()
    triangles = np.concatenate([np.stack([corner, corner + 1, corner + n + 2], 1),
                                np.stack([corner, corner + n + 2, corner + n + 1], 1)])
    return points, triangles


def clip_looking(eye, target, lens=1.0):
    """A world-to-clip matrix whose frustum is wide enough to hold the whole grid."""
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


class LevelsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.points, cls.triangles = wavy_grid()
        cls.levels = build_levels(cls.points, cls.triangles, CLUSTER)
        cls.placed = [pick.Placed(cls.levels, np.eye(3), np.zeros(3))]

    def view_from(self, eye):
        eye = np.asarray(eye, float)
        return pick.view(self.placed, eye, clip_looking(eye, [GRID / 2, GRID / 2, 0]), FOCAL_PX, NEAR)

    def test_the_finest_level_is_the_input(self):
        finest = self.levels.cluster_refined < 0
        self.assertEqual(self.levels.cluster_triangles[finest].sum(), len(self.triangles))
        self.assertGreater(self.levels.group_depth.max(), 1)

    def test_errors_grow_towards_the_coarsest_level(self):
        own = self.levels.group_error[self.levels.cluster_refined]
        refined = self.levels.cluster_refined >= 0
        self.assertTrue(np.all(own[refined] <= self.levels.group_error[self.levels.cluster_group[refined]]))
        self.assertTrue(np.isinf(self.levels.group_error).any())

    def test_a_cut_at_any_tau_leaves_no_crack(self):
        # Eye low over one corner: near clusters stay fine while far ones coarsen, so the cut mixes levels.
        eye = [-2.0, -2.0, 3.0]
        groups, scale = pick.placed_spheres(self.levels.group_sphere, self.placed[0])
        group_px = pick.projected_px(groups, self.levels.group_error * scale, np.asarray(eye), FOCAL_PX, NEAR)
        refined = self.levels.cluster_refined
        v = pick.View(self.levels.cluster_triangles, np.where(refined >= 0, group_px[np.maximum(refined, 0)], 0.0),
                      group_px[self.levels.cluster_group], refined < 0)
        finest_border = boundary_edges(self.levels, v.finest)
        mixed = 0
        for tau in (0.5, 1.0, 2.0, 4.0, 8.0, 16.0):
            drawn = pick.drawn(v, tau)
            mixed += len(set(self.levels.cluster_refined[drawn])) > 1
            # Only the grid's own outline is open: corners of the outline the coarser levels keep.
            self.assertTrue(boundary_edges(self.levels, drawn) <= finest_border | outline_chords(self.points),
                            f"open edges inside the grid at tau {tau}")
        self.assertGreater(mixed, 0, "no tau mixed levels, so the test proved nothing")

    def test_histogram_tau_stays_within_budget_and_one_bin_of_exact(self):
        v = self.view_from([GRID / 2, -GRID, GRID / 2])
        finest = int(v.triangles[v.finest].sum())
        ratio = pick.bin_edges()[1] / pick.bin_edges()[0]
        for share in (0.9, 0.7, 0.5):
            budget = int(share * finest)
            exact, histogram = pick.tau_exact(v, budget), pick.tau_from_histogram(v, budget)
            self.assertLessEqual(pick.triangles_at(v, exact), budget)
            self.assertLessEqual(pick.triangles_at(v, histogram), budget)
            self.assertGreaterEqual(histogram, exact)
            self.assertLessEqual(histogram, exact * ratio * (1 + 1e-9))

    def test_tau_never_drops_below_the_floor(self):
        v = self.view_from([GRID / 2, -GRID, GRID / 2])
        self.assertEqual(pick.tau_exact(v, 10 ** 9), pick.TAU_FLOOR_PX)
        self.assertEqual(pick.tau_from_histogram(v, 10 ** 9), pick.TAU_FLOOR_PX)

    def test_terminal_groups_draw_at_any_tau(self):
        v = self.view_from([GRID / 2, -GRID, GRID / 2])
        self.assertGreater(pick.triangles_at(v, np.inf), 0)

    def test_a_cluster_behind_the_eye_is_culled(self):
        v = self.view_from([GRID / 2, -GRID, GRID / 2])
        behind = pick.view(self.placed, np.array([GRID / 2, -GRID, GRID / 2]),
                           clip_looking(np.array([GRID / 2, -GRID, GRID / 2]), [GRID / 2, -2 * GRID, GRID / 2]),
                           FOCAL_PX, NEAR)
        self.assertGreater(len(v.triangles), 0)
        self.assertEqual(len(behind.triangles), 0)


def outline_chords(points, n=GRID):
    """Edges joining two outline vertices along the same side of the grid: a coarser level replaces a run of
    outline edges with one, which is still the outline, not a crack."""
    on = {i for i, (x, y, _z) in enumerate(points) if x in (0, n) or y in (0, n)}
    edges = set()
    for a in on:
        for b in on:
            if a < b and (points[a][0] == points[b][0] in (0, n) or points[a][1] == points[b][1] in (0, n)):
                edges.add((a, b))
    return edges


if __name__ == "__main__":
    unittest.main()
