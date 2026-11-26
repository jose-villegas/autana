"""Checks the midsurface collapse (r3d/source/midsurface.py): a closed thin slab becomes one sheet halfway between
its skins, its rim and far skin gone, and the sheet reads what it shows from behind off its own far skin. Traces
rays: needs the pinned r3d environment with LLVM."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.procedural.wave_sheet import BACK, sheet
    from r3d.ray_query import RayQuery
    from r3d.source import midsurface
except ImportError:
    np = None

from tests.r3d_env import needs_llvm  # noqa: E402

SLAB_CELLS = 6
SLAB_THICKNESS = 0.1
# A second slab beside the first, farther than its reach, so no ray may pair across.
BESIDE = (2.0, 0.0, 0.0)


def slab(offset=(0.0, 0.0, 0.0)):
    """A closed unit slab centred on (0.5, 0.5, 0) + offset: the wave sheet's two layers, the top facing +z and the
    bottom SLAB_THICKNESS below, joined by the hem. (points, tris, far): far marks the bottom skin."""
    cloth = sheet(SLAB_CELLS, centre=(0.5 + offset[0], 0.5 + offset[1]), layers=2, thickness=SLAB_THICKNESS,
                  z=SLAB_THICKNESS / 2 + offset[2])
    return cloth.positions, cloth.tris, cloth.layer == BACK


def two_slabs():
    """slab() and another BESIDE it, as one mesh: (points, tris, far, piece)."""
    p1, t1, f1 = slab()
    p2, t2, f2 = slab(BESIDE)
    piece = np.repeat([0, 1], [len(t1), len(t2)])
    return np.concatenate([p1, p2]), np.concatenate([t1, t2 + len(p1)]), np.concatenate([f1, f2]), piece


def far_skin(collapsed, tris, piece):
    """The FarSkin of a collapse, as the import builds it."""
    far_rows, sheet_rows = np.flatnonzero(collapsed.far), np.flatnonzero(collapsed.sheet)
    points = collapsed.points
    return midsurface.FarSkin(RayQuery(points, tris[far_rows]), points[tris[far_rows]], piece[far_rows],
                              points[collapsed.tri_v[sheet_rows]], piece[sheet_rows], collapsed.thickness[sheet_rows])


@unittest.skipIf(np is None, "the r3d environment is not installed")
@needs_llvm
class MidsurfaceTests(unittest.TestCase):
    def test_a_closed_slab_collapses_to_one_sheet_halfway_between_its_skins(self):
        points, tris, far = slab()
        collapsed = midsurface.collapse(points, tris, far, np.zeros(len(tris), dtype=int), RayQuery)
        drawn = collapsed.tri_v[collapsed.drawn]
        # The whole top skin stays, moved to the middle, its border too; the hem joining the skins is the rim.
        self.assertEqual(len(drawn), 2 * SLAB_CELLS ** 2)
        self.assertEqual(int(collapsed.sheet.sum()), 2 * SLAB_CELLS ** 2)
        np.testing.assert_allclose(collapsed.points[drawn][..., 2], 0.0, atol=1e-6)
        self.assertTrue(np.array_equal(collapsed.far, far), "a triangle of the top skin was taken for the far skin")
        np.testing.assert_allclose(collapsed.thickness[collapsed.sheet], SLAB_THICKNESS, rtol=1e-6)
        # Every drawn triangle still faces the top skin's way, and nothing else of the slab is drawn.
        normals = np.cross(collapsed.points[drawn[:, 1]] - collapsed.points[drawn[:, 0]],
                           collapsed.points[drawn[:, 2]] - collapsed.points[drawn[:, 0]])
        self.assertTrue((normals[:, 2] > 0).all())
        np.testing.assert_array_equal(collapsed.points[:len(points)], points)
        # Each moved point names the source point it came from, at the same x and y.
        np.testing.assert_array_equal(collapsed.source[:len(points)], np.arange(len(points)))
        np.testing.assert_allclose(collapsed.points[len(points):, :2], points[collapsed.source[len(points):], :2])

    def test_a_piece_that_is_not_a_shell_stays_as_it_was(self):
        points, tris, far = slab()
        collapsed = midsurface.collapse(points, tris, np.zeros(len(tris), dtype=bool), np.full(len(tris), -1), RayQuery)
        self.assertTrue(collapsed.drawn.all())
        np.testing.assert_array_equal(collapsed.tri_v, tris)

    def test_an_unpaired_patch_inside_the_near_skin_stays_on_the_sheet_moved_by_the_shell_s_thickness(self):
        points, tris, far = slab()
        # Push the bottom skin's middle point far down: the faces above it meet the far skin past the reach, but
        # they lie inside the top skin, joined to the far skin through no other unpaired face; they stay on the
        # sheet, and their points move by half the slab's thickness, as their neighbours' do.
        bottom = np.unique(tris[far])
        centre = bottom[np.argmin(np.linalg.norm(points[bottom, :2] - 0.5, axis=1))]
        moved = points.copy()
        moved[np.all(np.isclose(points, points[centre]), axis=1), 2] -= 10 * SLAB_THICKNESS
        collapsed = midsurface.collapse(moved, tris, far, np.zeros(len(tris), dtype=int), RayQuery)
        self.assertEqual(int(collapsed.sheet.sum()), 2 * SLAB_CELLS ** 2)
        self.assertEqual(int(collapsed.drawn.sum()), 2 * SLAB_CELLS ** 2)
        np.testing.assert_allclose(collapsed.points[collapsed.tri_v[collapsed.sheet]][..., 2], 0.0, atol=1e-6)

    def test_moves_that_turn_a_triangle_over_are_smoothed_with_their_neighbours_until_none_is(self):
        # A unit square of two triangles whose corner (1, 1) moves past the opposite corner: the triangle there turns
        # over, and the corners take the mean move around them until neither is.
        p = np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [1.0, 1.0, 0.0], [0.0, 1.0, 0.0]])
        tri_v = np.array([[0, 1, 2], [0, 2, 3]])
        shift = np.zeros((4, 3))
        shift[2] = [-3.0, -3.0, 0.0]
        rows = np.arange(len(tri_v))
        moved_tris = tri_v + len(p)
        self.assertTrue(midsurface.turned(p[tri_v], (p + shift)[tri_v], rows).any())
        points, flipped = midsurface.untangled(p, tri_v, p, shift, moved_tris, rows)
        self.assertEqual(len(flipped), 0)
        self.assertFalse(midsurface.turned(p[tri_v], points[moved_tris], rows).any())
        np.testing.assert_array_equal(points[:len(p)], p)

    def test_each_shell_pairs_with_its_own_far_skin_alone(self):
        points, tris, far, piece = two_slabs()
        collapsed = midsurface.collapse(points, tris, far, piece, RayQuery)
        self.assertEqual(int(collapsed.sheet.sum()), 4 * SLAB_CELLS ** 2)
        np.testing.assert_allclose(collapsed.points[collapsed.tri_v[collapsed.sheet]][..., 2], 0.0, atol=1e-6)
        # A ray onto another shell's far skin pairs nothing, however well it faces back and however near.
        far_rows = np.flatnonzero(far)
        query = RayQuery(points, tris[far_rows])
        origin, normal = np.array([[0.5, 0.5, SLAB_THICKNESS / 2]]), np.array([[0.0, 0.0, 1.0]])
        far_normals = np.tile([0.0, 0.0, -1.0], (len(far_rows), 1))
        own, _ = midsurface.inward_hits(query, far_normals, piece[far_rows], origin, normal, np.array([0]))
        other, _ = midsurface.inward_hits(query, far_normals, piece[far_rows], origin, normal, np.array([1]))
        self.assertTrue(own[0])
        self.assertFalse(other[0], "a ray paired with another shell's far skin")

    def test_the_sheet_reads_its_back_off_its_own_far_skin_within_reach_else_its_front(self):
        """Along its ray where that meets its shell's far skin, else at that skin's nearest point, within reach."""
        points, tris, far, piece = two_slabs()
        collapsed = midsurface.collapse(points, tris, far, piece, RayQuery)
        skin = far_skin(collapsed, tris, piece)
        at = np.array([[0.5, 0.5, 0.0], [0.5, 0.5, 0.0], [5.0, 5.0, 0.0], [2.5, 0.5, 0.0], [0.5, 0.5, 0.08]])
        normals = np.array([[0, 0, 1.0], [0, 0, -1.0], [0, 0, 1.0], [0, 0, 1.0], [0, 0, 1.0]])
        front = np.full((len(at), 3), 0.25)
        seen = []

        def at_hit(hits, rows, hit):
            seen.append((hits, skin.piece[rows]))
            return np.full((len(hits), 3), 0.75)

        back = midsurface.back_albedo(at, normals, front, skin, at_hit)
        # The first looks at its far skin, the second looks away but stands within reach of it, the fourth is on
        # the second slab; the third is off both slabs and the fifth stands above the first slab, farther from its
        # sheet than half its thickness: an open sheet there is not the slab's and reads its front.
        np.testing.assert_allclose(back, [[0.75] * 3, [0.75] * 3, [0.25] * 3, [0.75] * 3, [0.25] * 3])
        hits = np.concatenate([h for h, _ in seen])
        np.testing.assert_allclose(hits[:, 2], -SLAB_THICKNESS / 2, atol=1e-6)
        shells = np.concatenate([s for _, s in seen])
        np.testing.assert_array_equal(np.sort(shells), [0, 0, 1])
        np.testing.assert_allclose(midsurface.back_albedo(at, normals, front, None, at_hit), front)
        # Without normals, the nearest point alone.
        np.testing.assert_allclose(midsurface.back_albedo(at, None, front, skin, at_hit),
                                   [[0.75] * 3, [0.75] * 3, [0.25] * 3, [0.75] * 3, [0.25] * 3])


if __name__ == "__main__":
    unittest.main()
