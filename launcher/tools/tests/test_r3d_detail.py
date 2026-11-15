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

    def test_with_sight_an_eye_counts_only_for_the_copies_it_looks_at(self):
        lows, highs = [[0, 0, 0]], [[1, 1, 1]]
        # The near eye looks away from the box, the far one at it.
        eyes, forwards = [[3, 0.5, 0.5], [20, 0.5, 0.5]], np.array([[1, 0, 0], [-1, 0, 0]])
        self.assertAlmostEqual(detail.nearest_distance(lows, highs, eyes, NEAR, (forwards, 0.5)), 19.0)
        self.assertAlmostEqual(detail.nearest_distance(lows, highs, eyes, NEAR), 2.0)

    def test_a_copy_no_eye_looks_at_counts_every_eye(self):
        lows, highs = [[0, 0, 0]], [[1, 1, 1]]
        eyes, forwards = [[3, 0.5, 0.5], [20, 0.5, 0.5]], np.array([[1, 0, 0], [1, 0, 0]])
        self.assertAlmostEqual(detail.nearest_distance(lows, highs, eyes, NEAR, (forwards, 0.5)), 2.0)

    def test_a_copy_is_in_view_while_its_bounding_sphere_reaches_into_the_cone(self):
        # Half-angle atan(0.5) = 26.6 deg. Centre 31.0 deg off axis, sphere 8.5 deg wide: in; 38.7 and 7.8: out.
        lows, highs = np.array([[5., -1, 9], [7, -1, 9]]), np.array([[7., 1, 11], [9, 1, 11]])
        seen = detail.in_view(lows, highs, np.zeros((1, 3)), np.array([[0., 0, 1]]), 0.5)
        self.assertEqual(seen.tolist(), [[True, False]])


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

    def balance(self, room):
        from r3d.objects.carve import measured_balance
        jobs = []
        for rungs in self.LADDERS:
            def build(pick, rungs=rungs):
                return [], set(), [dict(output_triangles=rungs[pick].triangles, copies=1)], []
            jobs.append(dict(build=build, info=dict(ladder=rungs), pick=0))
        measured_balance(jobs, (room, room, 0))
        return [job['pick'] for job in jobs]

    def test_with_room_every_shape_keeps_its_near_level(self):
        self.assertEqual(self.balance(200), [0, 0])
        self.assertEqual(self.balance(None), [0, 0])

    def test_the_step_that_removes_most_triangles_per_pixel_goes_first(self):
        self.assertEqual(self.balance(150), [1, 0])

    def test_it_fails_after_the_last_rungs_when_the_ceiling_cannot_be_met(self):
        from r3d.import_settings import SettingsError
        with self.assertRaisesRegex(SettingsError, 'total 120 exceeds ceiling 10'):
            self.balance(10)


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
