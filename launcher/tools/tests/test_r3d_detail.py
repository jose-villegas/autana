"""Checks the carved shapes' derived detail: near distance, ladder, balance and the authoring report."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.objects import detail
except ImportError:
    np = None

FOCAL = 200.0  # pixels per unit depth
NEAR = 0.1
RINGS = 24  # latitude and longitude steps of the test sphere


def sphere(radius=1.0, rings=RINGS):
    """A UV sphere: curved everywhere, so every coarser error keeps fewer triangles."""
    theta = np.linspace(0, np.pi, rings + 1)[1:-1]
    phi = np.linspace(0, 2 * np.pi, rings, endpoint=False)
    t, f = np.meshgrid(theta, phi, indexing="ij")
    body = np.stack([np.sin(t) * np.cos(f), np.cos(t), np.sin(t) * np.sin(f)], axis=-1).reshape(-1, 3)
    positions = np.concatenate([[[0, 1, 0]], body, [[0, -1, 0]]]) * radius
    rows, last = len(theta), len(positions) - 1
    tris = []
    for j in range(rings):
        k = (j + 1) % rings
        tris.append([0, 1 + k, 1 + j])
        tris.append([last, 1 + (rows - 1) * rings + j, 1 + (rows - 1) * rings + k])
        for i in range(rows - 1):
            a, b = 1 + i * rings + j, 1 + i * rings + k
            tris += [[a, b, b + rings], [a, b + rings, a + rings]]
    return positions, np.array(tris)


@unittest.skipIf(np is None, "needs the r3d environment")
class NearestDistanceTest(unittest.TestCase):
    def test_the_closest_eye_to_the_closest_copy_sets_the_distance(self):
        lows, highs = [[0, 0, 0], [10, 0, 0]], [[1, 1, 1], [11, 1, 1]]
        self.assertAlmostEqual(detail.nearest_distance(lows, highs, [[14, 0.5, 0.5], [-5, 0.5, 0.5]], NEAR), 3.0)

    def test_an_eye_inside_a_copy_or_no_eyes_at_all_means_the_near_plane(self):
        lows, highs = [[0, 0, 0]], [[1, 1, 1]]
        self.assertEqual(detail.nearest_distance(lows, highs, [[0.5, 0.5, 0.5]], NEAR), NEAR)
        self.assertEqual(detail.nearest_distance(lows, highs, [], NEAR), NEAR)


@unittest.skipIf(np is None, "needs the r3d environment")
class LadderTest(unittest.TestCase):
    def test_a_nearer_view_keeps_more_triangles(self):
        positions, tris = sphere()
        near = detail.ladder(positions, tris, 2.0, FOCAL)[0].triangles
        far = detail.ladder(positions, tris, 20.0, FOCAL)[0].triangles
        self.assertLess(far, near)
        self.assertLessEqual(near, len(tris))

    def test_each_rung_keeps_fewer_triangles_at_a_larger_error(self):
        positions, tris = sphere()
        rungs = detail.ladder(positions, tris, 2.0, FOCAL)
        self.assertGreater(len(rungs), 2)
        self.assertEqual(rungs[0].error_px, detail.TOLERANCE_PX)
        for coarse, fine in zip(rungs[1:], rungs):
            self.assertLess(coarse.triangles, fine.triangles)
            self.assertGreater(coarse.error_px, fine.error_px)

    def test_the_near_level_moves_no_surface_more_than_the_threshold_on_screen(self):
        positions, tris = sphere()
        distance = 2.0
        error = detail.object_error(distance, FOCAL)
        kept = detail.level(positions, tris, error)
        moved = detail.surface_error(positions, tris, positions, kept)
        self.assertLessEqual(moved * FOCAL / distance, detail.TOLERANCE_PX)
        self.assertLess(len(kept), len(tris))

    def test_a_part_dropped_at_any_error_keeps_the_shape_whole(self):
        # A flat grid, and apart from it a zero-area sliver meshoptimizer drops even at error 0.
        grid = [[x, y, 0.0] for y in range(3) for x in range(3)]
        cells = [(0, 1, 4, 3), (1, 2, 5, 4), (3, 4, 7, 6), (4, 5, 8, 7)]
        positions = np.array(grid + [[5.0, 0, 0], [6.0, 0, 0], [7.0, 0, 0]])
        tris = np.array([t for a, b, c, d in cells for t in ((a, b, c), (a, c, d))] + [(9, 10, 11)])
        kept = detail.level(positions, tris, 1e-3)
        self.assertEqual(detail.surface_error(positions, tris, positions, kept), 0.0)
        self.assertEqual(len(kept), len(tris))


@unittest.skipIf(np is None, "needs the r3d environment")
class BalanceTest(unittest.TestCase):
    LADDERS = [[detail.Rung(100, 2.0), detail.Rung(40, 4.0), detail.Rung(30, 8.0)],
               [detail.Rung(100, 2.0), detail.Rung(90, 4.0)]]

    def test_with_room_every_shape_keeps_its_near_level(self):
        self.assertEqual(detail.balance(self.LADDERS, 200), [0, 0])
        self.assertEqual(detail.balance(self.LADDERS, None), [0, 0])

    def test_the_step_that_removes_most_triangles_per_pixel_goes_first(self):
        self.assertEqual(detail.balance(self.LADDERS, 150), [1, 0])

    def test_it_stops_at_the_last_rungs_when_the_room_cannot_be_met(self):
        self.assertEqual(detail.balance(self.LADDERS, 10), [2, 1])


@unittest.skipIf(np is None, "needs the r3d environment")
class AuthoringTest(unittest.TestCase):
    def test_only_shapes_above_the_threshold_are_named_with_their_reason(self):
        found = detail.needs_authoring([("chair", 1.0, False), ("lamp", 5.0, False), ("sign", 9.0, True)])
        self.assertEqual([(a.shape, a.reason) for a in found], [("lamp", "silhouette"), ("sign", "ceiling")])

    def test_a_surface_moved_by_a_distance_errs_by_that_distance(self):
        positions, tris = sphere()
        self.assertAlmostEqual(detail.surface_error(positions, tris, positions * 1.1, tris), 0.1, places=6)


if __name__ == "__main__":
    unittest.main()
