"""Source ownership and fixed simplifier reservations."""

import pathlib
import sys
import unittest

import numpy as np
from scipy.spatial import ConvexHull

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import simplify as owner
from r3d.import_settings import SettingsError, pass_overrides


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
    def allocate(self, budget=400, **kwargs):
        p, t, o = props()
        return owner.simplify(p, np.full_like(p, 100), t, o, budget,
                              allocation=owner.Allocation(tri_o=o, object_names=['near', 'far'], material_names=['plain', 'leaf'], **kwargs),
                              colour_deviation=1.0)

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
                      overrides=[{'object': 'far', 'triangles': 100}])
        job = NS(settings=settings, renderer=NS(visibility=NS(source='camera_region'),
                                                variant=NS(triangles=500)), bake=None)
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

    def test_override_schema(self):
        from r3d.import_settings import load_object_overrides
        for rule in ({'object': '*', 'material': '*', 'triangles': 1},
                     {'object': '*', 'importance': 1}, {'object': '*', 'triangles': 0},
                     {'object': '*', 'triangles': 1, 'typo': True}):
            with self.subTest(rule=rule), self.assertRaises(SettingsError):
                load_object_overrides({'overrides': [rule]})
        with self.assertRaises(SettingsError):
            load_object_overrides({'props_share': 0.3})
        self.assertEqual(load_object_overrides({'overrides': [{'object': '*', 'triangles': 10}]}),
                         [{'object': '*', 'triangles': 10}])

    def test_fixed_reservation_can_fill_budget(self):
        from unittest.mock import patch
        with patch.object(owner, 'simplify_with_update', wraps=owner.simplify_with_update) as run:
            self.allocate(budget=100, overrides=[{'object': 'near', 'triangles': 100}])
        self.assertEqual([call.args[3] for call in run.call_args_list], [100])

    def test_fixed_count(self):
        *_, labels = self.allocate(overrides=[{'object': 'near', 'triangles': 100}])
        self.assertEqual(np.count_nonzero(labels == 0), 100)


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
        edges = np.sort(np.concatenate([st[:, [0, 1]], st[:, [1, 2]], st[:, [2, 0]]]), axis=1)
        _, count = np.unique(edges, axis=0, return_counts=True)
        np.testing.assert_array_equal(count, np.full(len(count), 2))
        self.assertLess(len(st), len(t))
        self.assertEqual(len(np.unique(sp, axis=0)), len(sp))

    def test_fixed_overrides_cannot_exceed_budget(self):
        with self.assertRaisesRegex(ValueError, 'exceed triangle budget'):
            self.allocate(overrides=[{'object': '*', 'triangles': 300}])


    def test_material_fixed_and_clamp(self):
        from unittest.mock import patch
        *_, labels = self.allocate(overrides=[{'material': 'leaf', 'triangles': 100}])
        self.assertEqual(np.count_nonzero(labels == 1), 100)
        original = owner.simplify_with_update
        def bounded(p, c, t, target, *args, **kwargs):
            self.assertLessEqual(target, len(t))
            return original(p, c, t, target, *args, **kwargs)
        with patch.object(owner, 'log') as logged, patch.object(owner, 'simplify_with_update', side_effect=bounded):
            *_, labels = self.allocate(budget=1000, overrides=[{'object': 'near', 'triangles': 999}])
        self.assertEqual(np.count_nonzero(labels == 0), np.count_nonzero(props()[2] == 0))
        self.assertTrue(any('clamped' in str(call) for call in logged.call_args_list))


    def test_override_no_match(self):
        with self.assertRaisesRegex(SettingsError, 'missing'):
            self.allocate(overrides=[{'object': 'missing', 'triangles': 10}])

    def test_material_fixed_ambiguous(self):
        with self.assertRaisesRegex(SettingsError, 'several passes'):
            self.allocate(overrides=[{'material': '*', 'triangles': 10}])

    def test_welded_fixed_rejects_other_objects(self):
        with self.assertRaisesRegex(SettingsError, 'other'):
            pass_overrides(['chosen', 'other'], {'chosen': ['plain'], 'other': ['plain']}, [{'object': 'chosen', 'triangles': 10}])

    def test_material_fixed_rejects_welded_other_objects(self):
        p = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0], [1., 1, 0]])
        t = np.array([[0, 1, 2], [1, 3, 2]])
        o = np.arange(2)
        with self.assertRaisesRegex(SettingsError, 'other'):
            owner.simplify(p, np.full_like(p, 100), t, o, 2, colour_deviation=1,
                           allocation=owner.Allocation(tri_o=o, object_names=['chosen', 'other'], material_names=['plain', 'leaf'],
                                                       overrides=[{'material': 'plain', 'triangles': 1}]))


if __name__ == '__main__':
    unittest.main()
