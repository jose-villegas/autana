"""Checks the probes that look for sky through a simplified mesh's walls (r3d/leaks.py) and the simplify passes
that lock a vertex per hole: an arch covered by a separate sheet behind it stays covered at the same budget."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import leaks
    from r3d.simplify import Allocation, simplify
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402

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
        np.testing.assert_array_equal(leaks.pins(self.pos, self.tris, [np.array([1]), np.array([2])], *self.result, free),
                                      [3, 4])

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


@needs_mitsuba
@unittest.skipIf(np is None, "the r3d environment is not installed")
class LeakProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pos, cls.tris = arch()
        cls.probes = leaks.LeakProbes(cls.pos, cls.tris, *CAMERA)

    def simplified(self, allocation):
        rgb = np.full((len(self.pos), 3), 128.0)
        p, _, t, _ = simplify(self.pos, rgb, self.tris, np.zeros(len(self.tris), dtype=int), BUDGET, colour_deviation=1.0,
                              seal_seams=True, position_scale=8, allocation=allocation)
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


if __name__ == "__main__":
    unittest.main()
