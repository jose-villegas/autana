"""Checks that sealing closes new sky and splits T-junctions."""

import pathlib
import sys
import unittest
from collections import Counter
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.appearance_simplify import FitMesh
    from r3d.fit import seal
except ImportError:
    np = None

from tests.r3d_env import needs_llvm  # noqa: E402

# A point this far off an edge, in model units, is past the split distance; half of it is within.
FAR = 2 * seal.T_JUNCTION_DISTANCE if np is not None else None
NEAR = seal.T_JUNCTION_DISTANCE / 2 if np is not None else None


def mesh(points, tris, flat=False):
    """A FitMesh with one vertex per point, each vertex or triangle its own colour."""
    points = np.asarray(points, dtype=float)
    tris = np.asarray(tris)
    rows = len(tris) if flat else len(points)
    rgb = np.linspace(0.0, 1.0, rows * 3).reshape(rows, 3)
    return FitMesh(points, rgb, tris, np.zeros(len(tris), dtype=np.int64), 8, np.arange(len(points)), flat)


def t_junctions(fitted, distance):
    """(point, edge) pairs where a point lies within `distance` of the inside of an edge it does not end."""
    corners = fitted.vertex_point[fitted.tris]
    edges = {tuple(sorted((int(row[i]), int(row[(i + 1) % 3])))) for row in corners for i in range(3)}
    found = []
    for a, b in edges:
        e = fitted.points[b] - fitted.points[a]
        for point in range(len(fitted.points)):
            if point in (a, b):
                continue
            s = (fitted.points[point] - fitted.points[a]) @ e / (e @ e)
            if 0 < s < 1 and np.linalg.norm(fitted.points[a] + s * e - fitted.points[point]) < distance:
                found.append((point, (a, b)))
    return found


def area(fitted):
    p = fitted.points[fitted.vertex_point[fitted.tris]]
    return 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1).sum()


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SplitTJunctions(unittest.TestCase):
    def setUp(self):
        # Triangle 0 above the x axis; triangle 1 below it, its top corner (point 4) just off triangle 0's base.
        self.points = [(0, 0, 0), (4, 0, 0), (2, 3, 0), (1, -3, 0), (2, NEAR, 0), (3, -3, 0)]
        self.tris = [(0, 1, 2), (3, 5, 4)]

    def test_a_point_on_an_edge_splits_it_through_that_point(self):
        before = mesh(self.points, self.tris)
        after, splits = seal.split_t_junctions(before)
        self.assertEqual(splits, 1)
        self.assertEqual(len(after.tris), 3)
        self.assertIs(after.points, before.points)
        self.assertEqual(t_junctions(after, seal.T_JUNCTION_DISTANCE), [])
        split = after.vertex_point[after.tris]
        self.assertEqual(sum(4 in row for row in split.tolist()), 3, "both halves and triangle 1 end at point 4")
        self.assertAlmostEqual(area(after), area(before), delta=4 * NEAR)

    def test_the_split_vertex_takes_the_colour_along_the_edge(self):
        before = mesh(self.points, self.tris)
        after, _ = seal.split_t_junctions(before)
        added = len(after.vertex_point) - 1
        self.assertEqual(after.vertex_point[added], 4)
        np.testing.assert_allclose(after.rgb[added], (before.rgb[0] + before.rgb[1]) / 2, atol=0.02)
        np.testing.assert_array_equal(after.rgb[:len(before.rgb)], before.rgb)

    def test_a_flat_mesh_keeps_each_half_the_split_triangle_colour(self):
        before = mesh(self.points, self.tris, flat=True)
        after, _ = seal.split_t_junctions(before)
        self.assertEqual(len(after.rgb), 3)
        self.assertEqual(len(after.vertex_point), len(before.vertex_point))
        halves = [row for row, corners in zip(after.rgb, after.vertex_point[after.tris].tolist()) if 4 in corners
                  and 2 in corners]
        for colour in halves:
            np.testing.assert_array_equal(colour, before.rgb[0])

    def test_a_point_past_the_distance_or_a_corner_splits_nothing(self):
        points = list(self.points)
        points[4] = (2, FAR, 0)
        after, splits = seal.split_t_junctions(mesh(points, self.tris))
        self.assertEqual(splits, 0)
        self.assertEqual(len(after.tris), 2)
        after, splits = seal.split_t_junctions(mesh(self.points[:3], self.tris[:1]))
        self.assertEqual(splits, 0)

    def test_two_points_on_one_edge_both_split_it(self):
        points = self.points + [(1, -NEAR, 0), (0.5, -3, 0)]
        after, splits = seal.split_t_junctions(mesh(points, self.tris + [(7, 3, 6)]))
        self.assertEqual(splits, 2)
        self.assertEqual(len(after.tris), 5)
        self.assertEqual(t_junctions(after, seal.T_JUNCTION_DISTANCE), [])

    def test_splits_can_equal_the_start_triangle_count(self):
        points = self.points[:3] + [(1, 0, 0), (2, 0, 0), (1, -3, 0), (3, 0, 0), (3, -3, 0), (4, -3, 0)]
        after, splits = seal.split_t_junctions(mesh(points, [self.tris[0], (3, 5, 4), (6, 7, 8)]))
        self.assertEqual(splits, 3)
        self.assertEqual(len(after.tris), 6)

    def test_one_more_split_than_starting_triangles_raises_with_both_counts(self):
        points = self.points[:3] + [(1, 0, 0), (2, 0, 0), (1, -3, 0), (3, 0, 0), (3, -3, 0), (3.5, 0, 0)]
        with self.assertRaisesRegex(ValueError, r"4.*3 starting triangles"):
            seal.split_t_junctions(mesh(points, [self.tris[0], (3, 5, 4), (6, 7, 8)]))

    def test_a_split_that_would_leave_a_flat_half_is_not_made(self):
        # Points 0, 1 and 3 nearly on one line: splitting edge 1-2 at point 3 leaves the half 1, 3, 0 flat, whose
        # edge 0-3 would hold point 1, and the two splits would undo each other for ever.
        points = [(10, 0, 0), (-6, 0, 0), (-36, -0.4, 0), (-6.2, NEAR / 5, 0), (-6.2, 5, 0), (-9, 5, 0)]
        after, splits = seal.split_t_junctions(mesh(points, [(0, 1, 2), (3, 4, 5)]))
        self.assertEqual(splits, 0)
        self.assertEqual(len(after.tris), 2)

    def test_an_edge_two_triangles_share_splits_on_both_sides(self):
        # Triangle 2 shares triangle 0's base from below; point 4 sits on that base, ending triangle 1 inside it.
        points = self.points + [(2, -1, 0)]
        tris = [(0, 1, 2), (1, 0, 6), (3, 5, 4)]
        after, splits = seal.split_t_junctions(mesh(points, tris))
        self.assertEqual(splits, 2)
        self.assertEqual(len(after.tris), 5)
        self.assertEqual(t_junctions(after, seal.T_JUNCTION_DISTANCE), [])

    def test_both_faces_choose_the_same_point_each_pass(self):
        points = self.points + [(2, -1, 0), (3, -NEAR, 0), (4, -3, 0)]
        tris = [(0, 1, 2), (1, 0, 6), (3, 5, 4), (5, 8, 7)]
        with mock.patch.object(seal, "_split", wraps=seal._split) as split:
            after, splits = seal.split_t_junctions(mesh(points, tris))
        first = split.call_args_list[0].args[1]
        self.assertEqual({row: point for row, (_, point, _) in first.items()}, {0: 4, 1: 4})
        self.assertEqual(splits, 4)
        corners = after.vertex_point[after.tris].tolist()
        uses = Counter(tuple(sorted(edge)) for a, b, c in corners for edge in ((a, b), (b, c), (c, a)))
        self.assertEqual([uses[edge] for edge in ((0, 4), (4, 7), (1, 7))], [2, 2, 2])

    def test_an_orphan_point_cannot_split_an_edge(self):
        before = mesh(self.points, self.tris[:1])
        after, splits = seal.split_t_junctions(before)
        self.assertEqual(splits, 0)
        self.assertIs(after, before)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SnapBackCopyRows(unittest.TestCase):
    def test_only_real_rim_and_seen_rows_restore_points(self):
        points = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (2, 0, 0), (3, 0, 0), (2, 1, 0)]
        start = mesh(points, [(0, 1, 2), (3, 4, 5)])
        fitted = start._replace(points=start.points + [0, 0, 1], tris=start.tris[:1], double=start.double[:1])
        copies = mesh(points[:3], [(0, 1, 2)])
        # Copies begin at different rows in the fitted and start meshes because pruning removed a real row.
        fitted_copy, start_copy = len(fitted.tris), len(start.tris)
        for rim, seen in (([fitted_copy], [start_copy]), ([0, fitted_copy], [start_copy]),
                          ([fitted_copy], [0, start_copy])):
            with self.subTest(rim=rim, seen=seen):
                probes = mock.Mock()
                probes.rims.side_effect = [[np.array(rim)], []]
                probes.holes.return_value = [np.array(seen)]
                sealed, moved, left, _ = seal.snap_back(start, fitted, copies, probes)
                expected = fitted.points.copy()
                if 0 in rim or 0 in seen:
                    expected[:3] = start.points[:3]
                np.testing.assert_array_equal(sealed.points, expected)
                self.assertEqual(left, 0)
                self.assertEqual(moved, np.count_nonzero(np.any(expected != fitted.points, axis=1)))


# A wall at x = 0 with a square hole, and a sheet a little behind it, a grid wider than the hole on every side:
# the sheet covers the hole by overlapping the wall, as a seam's pieces do. The camera stands in front.
WALL_HALF, HOLE_HALF, SHEET_HALF, SHEET_BEHIND, SHEET_ROWS = 100.0, 30.0, 40.0, 1.0, 4
CAMERA = ((np.array([30.0, -50.0, -50.0]), np.array([150.0, 50.0, 50.0])), 2.0) if np is not None else None
# How far the fit pulls the sheet's top edge in: past the hole's edge, so a slit of sky opens closed in by the wall.
PULLED_IN = 15.0
START_GAP = 5.0
JOINED_GAP_GROWTH = 2.0
OUTLINE_REACH = 1


def wall_and_sheet():
    """A FitMesh of the wall (its outer corners, then the hole's) and the sheet's (SHEET_ROWS + 1)^2 grid."""
    w, h = WALL_HALF, HOLE_HALF
    outer = [(0, -w, -w), (0, w, -w), (0, w, w), (0, -w, w)]
    inner = [(0, -h, -h), (0, h, -h), (0, h, h), (0, -h, h)]
    tris = [tri for i in range(4) for tri in ((i, (i + 1) % 4, 4 + (i + 1) % 4), (i, 4 + (i + 1) % 4, 4 + i))]
    side = np.linspace(-SHEET_HALF, SHEET_HALF, SHEET_ROWS + 1)
    grid = [(-SHEET_BEHIND, y, z) for z in side for y in side]
    n = SHEET_ROWS + 1
    tris += [tri for r in range(SHEET_ROWS) for c in range(SHEET_ROWS)
             for tri in ((8 + r * n + c, 8 + r * n + c + 1, 8 + (r + 1) * n + c + 1),
                         (8 + r * n + c, 8 + (r + 1) * n + c + 1, 8 + (r + 1) * n + c))]
    return mesh(outer + inner + grid, tris)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class SnapBack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.start = wall_and_sheet()
        points = cls.start.points.copy()
        cls.top = np.flatnonzero((points[:, 0] < 0) & (points[:, 2] == SHEET_HALF))
        cls.bottom = np.flatnonzero((points[:, 0] < 0) & (points[:, 2] == -SHEET_HALF))
        points[cls.top, 2] -= PULLED_IN
        cls.fitted = cls.start._replace(points=points)
        cls.probes = seal.start_probes(cls.start, None, CAMERA)

    def test_sky_the_fit_opened_closes_with_its_points_back_at_the_start(self):
        self.assertGreater(len(self.probes.holes(*seal.geometry(self.fitted, None))), 0)
        sealed, moved, left, _ = seal.snap_back(self.start, self.fitted, None, self.probes)
        self.assertEqual(left, 0)
        self.assertEqual(self.probes.holes(*seal.geometry(sealed, None)), [])
        self.assertGreater(moved, 0)
        self.assertEqual(int(np.count_nonzero(np.any(sealed.points != self.fitted.points, axis=1))), moved)
        np.testing.assert_array_equal(sealed.points[self.top], self.start.points[self.top])

    def test_a_point_away_from_the_hole_keeps_its_fitted_place(self):
        fitted = self.fitted._replace(points=self.fitted.points.copy())
        fitted.points[self.bottom, 0] -= SHEET_BEHIND
        sealed, _, _, _ = seal.snap_back(self.start, fitted, None, self.probes)
        np.testing.assert_array_equal(sealed.points[self.bottom], fitted.points[self.bottom])

    def test_a_fit_that_opens_nothing_moves_nothing(self):
        sealed, moved, left, _ = seal.snap_back(self.start, self.start, None, self.probes)
        self.assertEqual((moved, left), (0, 0))
        np.testing.assert_array_equal(sealed.points, self.start.points)

    def test_the_sheet_seen_through_an_unchanged_rim_moves_back(self):
        points = self.start.points.copy()
        sheet = points[:, 0] == -SHEET_BEHIND
        points[sheet, 1:] = points[sheet, 1:].mean(axis=0)
        fitted = self.start._replace(points=points)

        sealed, moved, left, _ = seal.snap_back(self.start, fitted, None, self.probes)

        np.testing.assert_array_equal(sealed.points[sheet], self.start.points[sheet])
        self.assertEqual(left, 0)
        self.assertEqual(moved, np.count_nonzero(np.any(points != self.start.points, axis=1)))
        self.assertEqual(self.probes.holes(*seal.geometry(sealed, None), reach=seal.SNAP_BACK_REACH), [])

    def test_outline_movement_next_to_start_sky_moves_nothing(self):
        start = self.start._replace(points=self.start.points.copy())
        start.points[self.bottom, 2] = -HOLE_HALF + START_GAP
        fitted = start._replace(points=start.points.copy())
        fitted.points[self.bottom, 2] += JOINED_GAP_GROWTH
        probes = seal.start_probes(start, None, CAMERA)
        self.assertTrue(probes.holes(*seal.geometry(fitted, None), reach=OUTLINE_REACH))

        sealed, moved, left, _ = seal.snap_back(start, fitted, None, probes)

        self.assertEqual((moved, left), (0, 0))
        np.testing.assert_array_equal(sealed.points, fitted.points)

    def test_sky_moving_points_cannot_close_is_counted_and_logged(self):
        # Restoring positions cannot replace the missing sheet.
        missing = self.start._replace(tris=self.start.tris[:8], double=self.start.double[:8])
        rounds = 1
        with mock.patch.object(seal, "SNAP_BACK_ROUNDS", rounds):
            sealed, moved, left, ran = seal.snap_back(self.start, missing, None, self.probes)

        self.assertEqual(ran, rounds)
        self.assertGreater(left, 0)
        self.assertEqual(left, len(self.probes.holes(*seal.geometry(sealed, None), reach=None)))
        self.assertEqual(moved, 0)
        with mock.patch.object(seal, "log") as log:
            seal.log_lines(moved, left, ran, 0, len(missing.tris))
        self.assertEqual(log.call_args_list[1], mock.call(f"snap back: {left} holes left after {rounds} rounds"))


if __name__ == "__main__":
    unittest.main()
