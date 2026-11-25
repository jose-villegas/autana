"""Checks the midsurface collapse (r3d/source/midsurface.py): a closed thin slab becomes one sheet halfway between
its skins, its rim and far skin gone, and the sheet reads what it shows from behind off the far skin. Traces rays:
needs the pinned r3d environment with LLVM."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.ray_query import RayQuery
    from r3d.source import midsurface
except ImportError:
    np = None

from tests.r3d_env import needs_llvm  # noqa: E402

SLAB_CELLS = 6
SLAB_THICKNESS = 0.1


def grid(cells, z, up):
    """A unit square at height z cut into cells x cells quads, facing +z when `up`: (points, tris)."""
    axis = np.linspace(0.0, 1.0, cells + 1)
    x, y = np.meshgrid(axis, axis, indexing="ij")
    points = np.column_stack([x.ravel(), y.ravel(), np.full(x.size, z)])
    tris = []
    for i in range(cells):
        for j in range(cells):
            a, b, c, d = i * (cells + 1) + j, (i + 1) * (cells + 1) + j, (i + 1) * (cells + 1) + j + 1, i * (cells + 1) + j + 1
            tris += [[a, b, c], [a, c, d]] if up else [[a, c, b], [a, d, c]]
    return points, np.array(tris)


def slab(cells=SLAB_CELLS, thickness=SLAB_THICKNESS):
    """A closed slab: a top skin facing +z, a bottom one facing -z `thickness` below, and the walls joining their
    borders. (points, tris, far): far marks the bottom skin."""
    top, top_tris = grid(cells, thickness / 2, True)
    bottom, bottom_tris = grid(cells, -thickness / 2, False)
    points = np.concatenate([top, bottom])
    bottom_tris = bottom_tris + len(top)
    n = cells + 1
    ring = ([i * n for i in range(cells)] + [cells * n + j for j in range(cells)]
            + [(cells - i) * n + cells for i in range(cells)] + [cells - j for j in range(cells)])
    walls = []
    for k in range(len(ring)):
        a, b = ring[k], ring[(k + 1) % len(ring)]
        walls += [[a, b + len(top), b], [a, a + len(top), b + len(top)]]
    tris = np.concatenate([top_tris, bottom_tris, np.array(walls)])
    far = np.zeros(len(tris), dtype=bool)
    far[len(top_tris):len(top_tris) + len(bottom_tris)] = True
    return points, tris, far


@unittest.skipIf(np is None, "the r3d environment is not installed")
@needs_llvm
class MidsurfaceTests(unittest.TestCase):
    def test_a_closed_slab_collapses_to_one_sheet_halfway_between_its_skins(self):
        points, tris, far = slab()
        sheet = midsurface.collapse(points, tris, far, np.zeros(len(tris), dtype=int), RayQuery)
        drawn = sheet.tri_v[sheet.drawn]
        # The whole top skin stays, moved to the middle, its border too; the walls joining the skins are the rim.
        self.assertEqual(len(drawn), 2 * SLAB_CELLS ** 2)
        np.testing.assert_allclose(sheet.points[drawn][..., 2], 0.0, atol=1e-6)
        self.assertTrue(np.array_equal(sheet.far, far), "a triangle of the top skin was taken for the far skin")
        self.assertAlmostEqual(sheet.reach, midsurface.REACH * SLAB_THICKNESS, places=6)
        # Every drawn triangle still faces the top skin's way, and nothing else of the slab is drawn.
        normals = np.cross(sheet.points[drawn[:, 1]] - sheet.points[drawn[:, 0]],
                           sheet.points[drawn[:, 2]] - sheet.points[drawn[:, 0]])
        self.assertTrue((normals[:, 2] > 0).all())
        np.testing.assert_array_equal(sheet.points[:len(points)], points)

    def test_a_piece_that_is_not_a_shell_stays_as_it_was(self):
        points, tris, far = slab()
        piece = np.full(len(tris), -1)
        sheet = midsurface.collapse(points, tris, np.zeros(len(tris), dtype=bool), piece, RayQuery)
        self.assertTrue(sheet.drawn.all())
        np.testing.assert_array_equal(sheet.tri_v, tris)

    def test_a_pair_farther_than_the_reach_or_skins_not_facing_pairs_nothing(self):
        points, tris, far = slab()
        # Push one inside vertex of the bottom skin far down: the faces above it now meet the bottom skin past
        # REACH times the median thickness, or turned away, and join the rim.
        centre = (SLAB_CELLS + 1) * (SLAB_CELLS // 2) + SLAB_CELLS // 2
        moved = points.copy()
        moved[len(points) // 2 + centre, 2] -= 10 * SLAB_THICKNESS
        sheet = midsurface.collapse(moved, tris, far, np.zeros(len(tris), dtype=int), RayQuery)
        top_of_centre = np.any(tris == centre, axis=1)
        self.assertFalse(sheet.drawn[top_of_centre].any(), "a face reached past the reach")
        self.assertLess(int(sheet.drawn.sum()), 2 * SLAB_CELLS ** 2)

    def test_the_sheet_reads_its_back_off_the_far_skin_within_reach_else_its_front(self):
        """Along its ray where that meets the far skin, else at the far skin's nearest point, within reach."""
        points, tris, far = slab()
        far_query = RayQuery(points, tris[far])
        at = np.array([[0.5, 0.5, 0.0], [0.5, 0.5, 0.0], [5.0, 5.0, 0.0]])
        normals = np.array([[0.0, 0.0, 1.0], [0.0, 0.0, -1.0], [0.0, 0.0, 1.0]])
        front = np.full((3, 3), 0.25)
        seen = []

        def at_hit(hits, rows, hit):
            seen.append(hits)
            return np.full((len(hits), 3), 0.75)

        corners = points[tris[far]]
        back = midsurface.back_albedo(at, normals, front, far_query, corners, SLAB_THICKNESS, at_hit)
        # The first looks at the far skin, the second looks away but stands within reach of it, and the third
        # is off the slab.
        np.testing.assert_allclose(back, [[0.75] * 3, [0.75] * 3, [0.25] * 3])
        np.testing.assert_allclose(seen[0], [[0.5, 0.5, -SLAB_THICKNESS / 2]] * 2, atol=1e-6)
        np.testing.assert_allclose(midsurface.back_albedo(at, normals, front, far_query, corners, SLAB_THICKNESS / 4,
                                                          at_hit), front, err_msg="a far skin beyond the reach was read")
        np.testing.assert_allclose(midsurface.back_albedo(at, normals, front, None, corners, 1.0, at_hit), front)
        # Without normals, the nearest point alone.
        np.testing.assert_allclose(midsurface.back_albedo(at, None, front, far_query, corners, SLAB_THICKNESS, at_hit),
                                   [[0.75] * 3, [0.75] * 3, [0.25] * 3])


if __name__ == "__main__":
    unittest.main()
