"""Synthetic ownership, welded-pass and screen-error allocation contracts."""

import pathlib
import sys
import tempfile
import unittest

import numpy as np
from scipy.spatial import ConvexHull

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import simplify as owner
from r3d.import_settings import MESHLET_TRIANGLES, SettingsError, load_import_settings
from r3d.obj import load_obj
from r3d.gltf_mesh import load_gltf_mesh
from gltf.gltf_write import build_glb


def props():
    n = 300
    k = np.arange(n)
    y = 1 - 2 * (k + 0.5) / n
    angle = k * np.pi * (3 - np.sqrt(5))
    radius = np.sqrt(1 - y * y)
    p = np.column_stack([radius * np.cos(angle), y, radius * np.sin(angle)])
    t = ConvexHull(p).simplices
    return np.concatenate([p + [0, 0, 5], p + [0, 0, 50]]), np.concatenate([t, t + n]), np.repeat([0, 1], len(t))


class ObjectBudgetTests(unittest.TestCase):
    def allocate(self, budget=400, **kwargs):
        p, t, o = props()
        return owner.simplify(p, np.full_like(p, 100), t, o, budget, tri_o=o,
                              object_names=['near', 'far'], material_names=['plain', 'leaf'],
                              colour_deviation=1.0, **kwargs)

    def test_obj_triangle_order_and_default(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'plain.obj'
            path.write_text('v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\ng second\nf 1 3 4\n')
            *_, tri_o, names = load_obj(path)
            self.assertEqual(names, ['plain', 'second'])
            np.testing.assert_array_equal(tri_o, [0, 0, 1])
            path.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')
            self.assertEqual(load_obj(path)[-1], ['plain'])

    def test_gltf_node_ownership_order(self):
        mesh = {'primitives': [{'positions': [[0, 0, 0], [1, 0, 0], [0, 1, 0]], 'indices': [0, 1, 2]}]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'nodes.glb'
            path.write_bytes(build_glb([{'mesh': 0, 'name': 'a'}, {'mesh': 0, 'name': 'b'}], [], meshes=[mesh]))
            src = load_gltf_mesh(path)
            self.assertEqual(src.object_names, ['a', 'b'])
            np.testing.assert_array_equal(src.tri_o, [0, 1])
            self.assertEqual(len(src.tri_o), len(src.tri_v))

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
        self.assertLessEqual(400 - len(t), MESHLET_TRIANGLES)

    def test_deterministic(self):
        a = self.allocate(eyes=np.zeros((1, 3)), focal_px=100)
        b = self.allocate(eyes=np.zeros((1, 3)), focal_px=100)
        for x, y in zip(a, b):
            np.testing.assert_array_equal(x, y)

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
                with self.assertRaisesRegex(SettingsError, r'\[objects\] overrides'):
                    load_import_settings(path)

    def test_no_new_cracks(self):
        p, t, o = props()
        p, t = p[:300], t[o == 0]
        # Two source objects tile one closed surface; independent passes would open its seam.
        o = (p[t].mean(axis=1)[:, 0] > 0).astype(int)
        result = owner.simplify(p, np.full_like(p, 100), t, o, 160, tri_o=o, colour_deviation=1)
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
        factor, fixed = owner.pass_overrides(['a', 'b'], ['leaf'], rules)
        self.assertEqual((factor, fixed), (2, 20))

    def test_region_sampling_preserves_rng_and_uses_actual_eyes(self):
        from r3d.light import visible_from_region
        from unittest.mock import Mock
        p = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.]])
        t = np.array([[0, 1, 2]])
        query = Mock()
        query.first_hit.return_value = (np.zeros((0, 3)), np.array([], dtype=int), np.array([], dtype=int))
        a, b = np.random.default_rng(5), np.random.default_rng(5)
        captured = []
        plain = visible_from_region(p, t, np.array([True]), query, 2, a, [0, 0, 2], [1, 1, 3])
        recorded = visible_from_region(p, t, np.array([True]), query, 2, b, [0, 0, 2], [1, 1, 3], eyes=captured)
        np.testing.assert_array_equal(plain, recorded)
        self.assertEqual(a.bit_generator.state, b.bit_generator.state)
        expected = np.random.default_rng(5)
        expected.random(2)
        expected.random(2)
        np.testing.assert_array_equal(captured[0], [0, 0, 2] + expected.random((2, 3)))

    def test_repeated_error_targets_are_cached(self):
        from unittest.mock import patch
        original = owner.simplify_with_update
        seen = set()

        def record(p, c, t, budget, weight, options, **kwargs):
            key = (tuple(p[0]), kwargs['target_error'])
            self.assertNotIn(key, seen)
            seen.add(key)
            return original(p, c, t, budget, weight, options, **kwargs)

        with patch.object(owner, 'simplify_with_update', side_effect=record):
            self.allocate(budget=401, eyes=np.zeros((1, 3)), focal_px=100)
        self.assertGreater(len(seen), 2)
        self.assertLess(len(seen), 2 * (owner.BISECTION_STEPS + 2))


if __name__ == '__main__':
    unittest.main()
