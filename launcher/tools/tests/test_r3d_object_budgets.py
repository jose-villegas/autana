"""Synthetic ownership, welded-pass and projected-area allocation contracts."""

import pathlib
import sys
import tempfile
import unittest

import numpy as np
from scipy.spatial import ConvexHull

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import simplify as owner
from r3d.import_settings import SettingsError, load_import_settings, pass_overrides


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
        kwargs.setdefault("props_share", 1.0)
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

    def test_near_receives_more_and_budget_holds(self):
        *_, t, labels = self.allocate(eyes=np.array([[0., 0., 0.], [1., 0., 0.]]), focal_px=100)
        self.assertGreater(np.count_nonzero(labels == 0), np.count_nonzero(labels == 1))
        self.assertLessEqual(len(t), 400)

    def test_deterministic(self):
        a = self.allocate(eyes=np.zeros((1, 3)), focal_px=100)
        b = self.allocate(eyes=np.zeros((1, 3)), focal_px=100)
        for x, y in zip(a, b):
            np.testing.assert_array_equal(x, y)

    def test_reordered_objects(self):
        p, t, o = props()
        a = self.allocate(eyes=np.zeros((1, 3)), focal_px=100)
        order = np.concatenate([np.flatnonzero(o == 1), np.flatnonzero(o == 0)])
        b = owner.simplify(p, np.full_like(p, 100), t[order], o[order], 400, colour_deviation=1,
                          allocation=owner.Allocation(tri_o=1 - o[order], object_names=['far', 'near'], material_names=['plain', 'leaf'],
                                                      eyes=np.zeros((1, 3)), focal_px=100, props_share=1))
        for label in (0, 1):
            self.assertEqual(np.count_nonzero(a[3] == label), np.count_nonzero(b[3] == label))
        np.testing.assert_array_equal(np.sort(a[0], axis=0), np.sort(b[0], axis=0))

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
                      simplify=NS(dense_edge=0.5, seal_seams=True, colour_deviation=1),
                      overrides=[{'object': 'far', 'triangles': 100}], props_share=1)
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

    def test_fixed_count(self):
        *_, labels = self.allocate(overrides=[{'object': 'near', 'triangles': 100}])
        self.assertEqual(np.count_nonzero(labels == 0), 100)

    def test_importance_shifts_triangles(self):
        *_, plain = self.allocate()
        *_, shifted = self.allocate(overrides=[{'material': 'leaf', 'importance': 3}])
        self.assertGreater(np.count_nonzero(shifted == 1), np.count_nonzero(plain == 1))

    def test_old_props_pointer(self):
        for key, value in [('props', '[]'), ('props_share', '0.3')]:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                path = pathlib.Path(directory) / 'm.import.toml'
                path.write_text('[source]\npath="m.obj"\ncredit="test"\n[output]\n'
                                f'[geometry.simplify]\n{key}={value}\n')
                with self.assertRaisesRegex(SettingsError, r'\[objects\]'):
                    load_import_settings(path)

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

    def test_override_schema_and_combination(self):
        from r3d.import_settings import load_object_overrides
        for rule in ({'object': '*', 'material': '*', 'triangles': 1},
                     {'object': '*', 'importance': 1, 'triangles': 1},
                     {'object': '*', 'importance': 0}, {'object': '*', 'triangles': 0},
                     {'object': '*', 'triangles': 1, 'typo': True}):
            with self.subTest(rule=rule), self.assertRaises(SettingsError):
                load_object_overrides({'overrides': [rule]})
        rules = [{'object': '*', 'importance': 2}, {'material': 'leaf*', 'importance': 0.5},
                 {'object': '*', 'triangles': 10}]
        factor, fixed = pass_overrides(['a', 'b'], {'a': ['leaf'], 'b': ['leaf']}, rules)
        self.assertEqual((factor, fixed), (1, 10))

    def test_view_eyes(self):
        from types import SimpleNamespace as NS
        from unittest.mock import patch
        from r3d.mesh_import import view_eyes, REGION_EYE_SAMPLES
        from r3d.poses import focal_px
        from panel_size import PANEL_WIDTH, PANEL_HEIGHT
        camera = NS(component=NS(half_fov_short_tan=0.5))
        scene = NS(camera=camera, region=[[0, 0, 0], [1, 2, 3]])
        region = NS(source='region')
        eyes, focal = view_eyes(region, scene, 5)
        self.assertEqual(eyes.shape, (REGION_EYE_SAMPLES, 3))
        self.assertEqual(focal, focal_px(PANEL_WIDTH, PANEL_HEIGHT, 0.5))
        np.testing.assert_array_equal(eyes, view_eyes(region, scene, 5)[0])
        poses = np.array([[1, 2, 3, 0, 0, 1]])
        with patch('r3d.mesh_import.camera_path_poses', return_value=(400, 200, 0.25, 1, poses)):
            eyes, focal = view_eyes(NS(source='camera_path'), scene, 5)
        np.testing.assert_array_equal(eyes, poses[:, :3])
        self.assertEqual(focal, focal_px(400, 200, 0.25))
        self.assertEqual(view_eyes(region, NS(camera=None), 5), (None, 1))
        for visibility in (None, region):
            self.assertEqual(view_eyes(visibility, NS(camera=camera, region=None), 5), (None, 1))

    def test_projected_scale_edges(self):
        p = np.array([[0., 0., 0.]])
        self.assertEqual(owner.projected_scale(p, None, 100), 1)
        self.assertEqual(owner.projected_scale(p, np.empty((0, 3)), 100), 1)
        self.assertEqual(owner.projected_scale(p, p, 1), 1 / np.finfo(np.float32).eps)
        scale = owner.projected_scale(p, np.array([[1., 0, 0], [2., 0, 0], [4., 0, 0]]), 1)
        self.assertGreater(scale, 0.25)
        self.assertLess(scale, 1)

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

    def test_inside_room_scale_is_comparable(self):
        corners = np.array([[x, y, z] for x in (-5., 5.) for y in (-5., 5.) for z in (-5., 5.)])
        room_t = ConvexHull(corners).simplices
        room, _, _ = owner.densify(corners, room_t, np.zeros(len(room_t)), 0.5)
        eye = np.zeros((1, 3))
        room_scale = owner.projected_scale(room, eye, 100)
        prop_scale = owner.projected_scale(np.array([[0., 0., 2.], [0., 0., 3.]]), eye, 100)
        self.assertLess(room_scale, prop_scale * 2)
        self.assertGreater(room_scale, prop_scale / 10)

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

    def test_importance_once_per_pass(self):
        self.assertEqual(pass_overrides(['a', 'b'], {'a': [], 'b': []}, [{'object': '*', 'importance': 2}]), (2, None))



if __name__ == '__main__':
    unittest.main()
