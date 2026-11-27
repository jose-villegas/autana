"""Measure simplification error on carved shapes."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.objects import detail
except ImportError:
    np = None

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
class LevelTest(unittest.TestCase):



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
class SurfaceErrorTest(unittest.TestCase):

    def test_a_surface_moved_by_a_distance_errs_by_that_distance(self):
        positions, tris = sphere()
        self.assertAlmostEqual(detail.surface_error(positions, tris, positions * 1.1, tris), 0.1, places=6)


if __name__ == "__main__":
    unittest.main()
