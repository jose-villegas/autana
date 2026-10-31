"""Source ownership and welded simplifier groups."""

import pathlib
import sys
import unittest

import numpy as np
from scipy.spatial import ConvexHull

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import simplify as owner
from soup import edge_counts


def props():
    n = 300
    k = np.arange(n)
    y = 1 - 2 * (k + 0.5) / n
    angle = k * np.pi * (3 - np.sqrt(5))
    radius = np.sqrt(1 - y * y)
    p = np.column_stack([radius * np.cos(angle), y, radius * np.sin(angle)])
    t = ConvexHull(p).simplices
    return np.concatenate([p + [0, 0, 5], p + [0, 0, 100]]), np.concatenate([t, t + n]), np.repeat([0, 1], len(t))


class ObjectBudgetTests(unittest.TestCase):
    def test_shared_edge_and_free_object(self):
        p = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0],
                      [5, 0, 0], [6, 0, 0], [5, 1, 0]], dtype=float)
        t = np.arange(9).reshape(-1, 3)
        groups = owner.object_groups(p, t, np.arange(3))
        self.assertEqual(groups[0], groups[1])
        self.assertNotEqual(groups[0], groups[2])


    def test_ownership_through_bake_geometry(self):
        from types import SimpleNamespace as NS
        from unittest.mock import patch
        from r3d import mesh_import as importer
        p, t, o = props()
        uv = np.array([[0., 0.], [1., 0.]])
        tt = np.ones_like(t)
        tt[:10] = 0
        texture = NS(levels=[np.zeros((1, 1, 4))],
                     sample=lambda coords, lod: np.column_stack([np.ones((len(coords), 3)), coords[:, 0]]))
        src = NS(p=p, uv=uv, tri_v=t, tri_t=tt, tri_m=o.copy(), tri_o=o.copy(),
                 object_names=['near', 'far'], names=['plain', 'leaf'], textures=[texture, texture])
        settings = NS(seed=5, position_scale=None, alpha_keep=0.5, double_sided=[],
                      thin=NS(material='leaf', keep=0.7),
                      simplify=NS(dense_edge=0.5, seal_seams=True, colour_deviation=1, props=set(), props_share=0),
                      meshes=[{'match': {'object': 'far'}, 'triangles': 100}])
        job = NS(settings=settings, renderer=NS(visibility=NS(source='camera_region'),
                                                variant=NS(name='full', triangles=500, objects_scale=1.0)), bake=None)
        stages = []
        def mask(*args, **kwargs):
            result = original_mask(*args, **kwargs)
            self.assertEqual(len(result[0]), np.count_nonzero(result[3]))
            stages.append('alpha')
            return result
        def visible(v, scene, p, triangles, *args):
            self.assertEqual(len(src.tri_o), len(triangles))
            keep = np.ones(len(triangles), dtype=bool)
            keep[::11] = False
            stages.append('visibility')
            return keep
        def dense(p, triangles, ownership, *args):
            self.assertEqual(len(ownership), len(triangles))
            result = owner.densify(p, triangles, ownership, *args)
            self.assertEqual(len(result[2]), len(result[1]))
            stages.append('thin/dense')
            return result
        original_repair = owner.repair
        def repair(p, c, triangles, ownership, *args):
            self.assertEqual(len(ownership), len(triangles))
            result = original_repair(p, c, triangles, ownership, *args)
            self.assertEqual(len(result[2]), len(result[3]))
            stages.append('seal')
            return result
        original_mask = importer.drop_masked
        with patch.object(importer, 'load_source', return_value=src), \
                patch.object(importer, 'RayQuery'), \
                patch.object(importer, 'path_light_for', return_value=None), \
                patch.object(importer, 'drop_masked', side_effect=mask), \
                patch.object(importer, 'visible_triangles', side_effect=visible), \
                patch.object(importer, 'densify', side_effect=dense), \
                patch.object(owner, 'repair', side_effect=repair), \
                patch.object(importer, 'shade_unlit', side_effect=lambda src, mat, p, t: (p, np.full_like(p, 100), t)):
            result = importer.bake_geometry(job, None)
        self.assertEqual(stages, ['alpha', 'visibility', 'thin/dense', 'seal'])
        self.assertEqual(np.count_nonzero(result.tri_mat == 1), 100)
        self.assertTrue(np.all(result.positions[result.tris[result.tri_mat == 1]][:, :, 2] > 40))

    def test_reserved_share_matches_main_formula(self):
        from unittest.mock import patch
        p, t, o = props()
        for budget, share in ((401, 0.3), (400, 0), (400, 1)):
            calls = []
            def record(p, c, t, target, *args):
                calls.append(target)
                return p, c, t[:target], np.arange(len(p))
            with self.subTest(budget=budget, share=share), patch.object(owner, 'simplify_with_update', side_effect=record):
                owner.simplify(p, np.full_like(p, 100), t, o, budget, reserved=[({0}, share)],
                               colour_deviation=1, allocation=owner.Allocation(tri_o=o))
            prop = max(1, int(budget * share))
            self.assertEqual(calls, [prop, max(1, budget - prop)])


    def test_no_new_cracks(self):
        p, t, o = props()
        p, t = p[:300], t[o == 0]
        # Two source objects tile one closed surface; independent passes would open its seam.
        o = (p[t].mean(axis=1)[:, 0] > 0).astype(int)
        pieces = []
        for label in (0, 1):
            sp, _, st, _ = owner.simplify(p, np.full_like(p, 100), t[o == label],
                                         o[o == label], 80, colour_deviation=1)
            pieces.append(sp[st])
        independent = np.concatenate(pieces)
        independent_edges = np.sort(np.concatenate([independent[:, [0, 1]], independent[:, [1, 2]],
                                                    independent[:, [2, 0]]]), axis=1).reshape(-1, 6)
        _, independent_count = np.unique(independent_edges, axis=0, return_counts=True)
        self.assertTrue(np.any(independent_count == 1))
        result = owner.simplify(p, np.full_like(p, 100), t, o, 160, allocation=owner.Allocation(tri_o=o), colour_deviation=1)
        sp, _, st, _ = result
        count = edge_counts(st)
        np.testing.assert_array_equal(count, np.full(len(count), 2))
        self.assertLess(len(st), len(t))
        self.assertEqual(len(np.unique(sp, axis=0)), len(sp))


if __name__ == '__main__':
    unittest.main()
