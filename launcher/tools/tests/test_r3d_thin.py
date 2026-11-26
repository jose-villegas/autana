"""Thinning keeps whole pieces, enlarges them to keep the coverage, and the collapse leaves the kept ones alone."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.geometry import triangle_areas
from r3d.simplify import Allocation, simplify
from r3d.thin import AGGREGATE_PIECES_MIN, pieces, thin, thin_keep
from soup import sphere
from tests.r3d_env import needs_llvm  # noqa: E402

QUAD = 1.0  # a test piece's side
SPACING = 3.0  # piece origins apart, so no two pieces touch


def quads(count, z=0.0):
    """`count` separate unit squares in a row, two triangles each."""
    corners = np.array([[0, 0, 0], [QUAD, 0, 0], [QUAD, QUAD, 0], [0, QUAD, 0]], dtype=float)
    p = np.concatenate([corners + [i * SPACING, 0, z] for i in range(count)])
    tri_v = np.concatenate([np.array([[0, 1, 2], [0, 2, 3]]) + 4 * i for i in range(count)])
    return p, tri_v


def piece_areas(p, tri_v, piece):
    area = triangle_areas(p, tri_v)
    return np.array([area[piece == i].sum() for i in np.unique(piece)])


class PieceTests(unittest.TestCase):
    def test_pieces_join_triangles_through_shared_corners_of_one_material(self):
        p, tri_v = quads(3)
        tri_m = np.array([0, 0, 0, 0, 1, 1])
        piece = pieces(p, tri_v, tri_m)
        self.assertEqual(piece[0], piece[1])
        self.assertEqual(piece[2], piece[3])
        self.assertEqual(len(np.unique(piece)), 3)
        piece = pieces(p, tri_v, np.array([0, 1, 0, 0, 0, 0]))
        self.assertNotEqual(piece[0], piece[1])


class ThinTests(unittest.TestCase):
    def test_a_kept_piece_grows_by_the_root_of_one_over_keep_about_its_centre(self):
        p, tri_v = quads(200)
        piece = pieces(p, tri_v, np.zeros(len(tri_v), dtype=int))
        keep = np.full(len(tri_v), 0.25)
        kept, q, out = thin(p, tri_v, piece, keep, np.random.default_rng(1))
        self.assertTrue(np.all(kept[0::2] == kept[1::2]), "a piece is kept or dropped whole")
        np.testing.assert_allclose(piece_areas(q, out[kept], piece[kept]), QUAD * QUAD / 0.25)
        before = p[tri_v[kept]].reshape(-1, 6, 3).mean(axis=1)
        after = q[out[kept]].reshape(-1, 6, 3).mean(axis=1)
        np.testing.assert_allclose(after, before)

    def test_the_kept_share_is_seeded_and_near_keep(self):
        p, tri_v = quads(400)
        piece = pieces(p, tri_v, np.zeros(len(tri_v), dtype=int))
        keep = np.full(len(tri_v), 0.25)
        first = thin(p, tri_v, piece, keep, np.random.default_rng(7))[0]
        again = thin(p, tri_v, piece, keep, np.random.default_rng(7))[0]
        np.testing.assert_array_equal(first, again)
        spread = 4 * np.sqrt(400 * 0.25 * 0.75)  # four standard deviations of the kept count
        self.assertLess(abs(first[0::2].sum() - 100), spread)

    def test_triangles_kept_whole_are_untouched(self):
        p, tri_v = quads(4)
        piece = pieces(p, tri_v, np.zeros(len(tri_v), dtype=int))
        kept, q, out = thin(p, tri_v, piece, np.ones(len(tri_v)), np.random.default_rng(1))
        self.assertTrue(kept.all())
        np.testing.assert_array_equal(q[out], p[tri_v])


class SelectionTests(unittest.TestCase):
    def setUp(self):
        many, few = AGGREGATE_PIECES_MIN, AGGREGATE_PIECES_MIN - 1
        # Object 0: an aggregate of cut-out leaves. Object 1: too few leaves. Object 2: many opaque pieces.
        self.tri_o = np.repeat([0, 1, 2], [2 * many, 2 * few, 2 * many])
        self.tri_m = np.repeat([0, 0, 1], [2 * many, 2 * few, 2 * many])
        self.piece = np.arange(len(self.tri_o)) // 2
        self.names = ['leaf', 'wall']
        self.cut = np.array([True, False])

    def test_automatic_thin_takes_objects_made_of_many_cut_out_pieces(self):
        keep = thin_keep(self.tri_m, self.tri_o, self.piece, None, self.cut, 0.2, self.names)
        np.testing.assert_array_equal(np.unique(keep[self.tri_o == 0]), [0.2])
        self.assertTrue(np.all(keep[self.tri_o != 0] == 1))

    def test_a_ratio_of_one_or_more_thins_nothing(self):
        keep = thin_keep(self.tri_m, self.tri_o, self.piece, None, self.cut, 1.5, self.names)
        self.assertTrue(np.all(keep == 1))

    def test_the_rule_names_its_material_and_keep_whatever_its_pieces(self):
        rule = SimpleNamespace(material='wall', keep=0.5)
        keep = thin_keep(self.tri_m, self.tri_o, self.piece, rule, self.cut, 0.2, self.names)
        self.assertTrue(np.all(keep[self.tri_m == 1] == 0.5))
        self.assertTrue(np.all(keep[self.tri_o == 0] == 0.2))

    def test_skipped_objects_are_not_thinned(self):
        keep = thin_keep(self.tri_m, self.tri_o, self.piece, None, self.cut, 0.2, self.names, skip=(0,))
        self.assertTrue(np.all(keep == 1))


class HeldTests(unittest.TestCase):
    def test_held_pieces_pass_the_collapse_whole_and_the_rest_pays_the_budget(self):
        leaf_p, leaf_v = quads(100, z=5.0)
        ball_p, ball_v = sphere(9)
        pos = np.concatenate([leaf_p, ball_p * 4])
        tris = np.concatenate([leaf_v, ball_v + len(leaf_p)])
        labels = np.repeat([0, 1], [len(leaf_v), len(ball_v)])
        rgb = np.full((len(pos), 3), 128.0)
        budget = len(leaf_v) + 40
        held = labels == 0
        out = simplify(pos, rgb, tris, labels, budget, colour_deviation=1.0, allocation=Allocation(held=held))
        p, _, t, m = out
        self.assertEqual(int((m == 0).sum()), len(leaf_v))
        np.testing.assert_allclose(np.sort(triangle_areas(p, t[m == 0])), np.full(len(leaf_v), QUAD * QUAD / 2))
        self.assertLess(int((m == 1).sum()), len(ball_v))
        self.assertLessEqual(len(t), budget + 3)


class BakeTests(unittest.TestCase):
    @needs_llvm
    def test_a_cut_out_aggregate_keeps_a_thinned_enlarged_share_through_simplify(self):
        from r3d import mesh_import
        from test_r3d_detail import sphere as wound_sphere
        leaf_p, leaf_v = quads(2 * AGGREGATE_PIECES_MIN, z=5.0)
        sphere_rings = 8
        ball_p, ball_v = wound_sphere(rings=sphere_rings)
        p = np.concatenate([leaf_p, ball_p * 4])
        tri_v = np.concatenate([leaf_v, ball_v + len(leaf_p)])
        tri_m = np.repeat([0, 1], [len(leaf_v), len(ball_v)])
        src = SimpleNamespace(p=p, tri_v=tri_v, tri_t=None, uv=None, tri_m=tri_m, tri_o=tri_m.copy(),
                              names=['leaf', 'wall'], object_names=['tree', 'ball'], textures=[None, None],
                              materials={}, colors=None)
        budget = len(tri_v) // 2
        steps = SimpleNamespace(dense_edge=100.0, seal_seams=False, colour_deviation=1.0)
        settings = SimpleNamespace(seed=3, position_scale=None, alpha_keep=None, thin=None, cards=None, double_sided=set(),
                                   meshes=[], simplify=steps)
        job = SimpleNamespace(settings=settings, bake=None,
                              renderer=SimpleNamespace(visibility=None, variant=SimpleNamespace(triangles=budget)))
        with patch.object(mesh_import, 'load_source', return_value=src), \
                patch.object(mesh_import, 'alpha_tested', return_value=np.array([True, False])):
            result = mesh_import.bake_geometry(job, None)
        leaves = result.tri_mat == 0
        ratio = budget / len(tri_v)
        self.assertGreater(leaves.sum(), 0)
        self.assertEqual(leaves.sum() % 2, 0)
        area = triangle_areas(result.positions, result.tris[leaves])
        # The simplifier returns float32 positions before areas are measured.
        rtol = np.finfo(np.float32).eps * np.abs(result.positions).max() / QUAD
        np.testing.assert_allclose(area, QUAD * QUAD / 2 / ratio, rtol=rtol)
        self.assertLessEqual(len(result.tris), budget)


if __name__ == '__main__':
    unittest.main()
