"""Synthetic prop classification and bounded projected-area allocation."""

import pathlib
import sys
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import simplify as owner
from r3d.import_settings import SettingsError, load_object_overrides


def scene():
    p = np.array([[0., 0, 5], [1, 0, 5], [0, 1, 5],
                  [0., 0, 50], [1, 0, 50], [0, 1, 50],
                  [-100., -100, -10], [100, -100, -10], [0, 100, -10],
                  [-100., -100, -20], [100, -100, -20], [0, 100, -20]])
    t = np.arange(len(p)).reshape(-1, 3)
    return p, t, np.arange(len(t))


class PropTests(unittest.TestCase):
    def test_capped_shares_redistribute_and_round_within_budget(self):
        np.testing.assert_array_equal(owner.prop_budgets([1, 3, 6], [100, 4, 100], 20), [2, 4, 14])
        shares = owner.prop_budgets([1, 1, 1], [10, 10, 10], 8)
        np.testing.assert_array_equal(shares, [3, 3, 2])
        np.testing.assert_array_equal(owner.prop_budgets([1, 1], [2, 3], 20), [2, 3])
        np.testing.assert_array_equal(owner.prop_budgets([0, 0], [2, 3], 4), [2, 2])
        np.testing.assert_array_equal(owner.prop_budgets([1, 1, 1], [5, 5, 5], 1), [1, 0, 0])

    def test_props_and_one_structure_call_keep_count_target_semantics(self):
        p, t, o = scene()
        calls = []
        def record(pos, rgb, tris, budget, weight, options):
            calls.append((len(tris), budget))
            return pos, rgb, tris[:budget], np.arange(len(pos))
        with patch.object(owner, 'simplify_with_update', side_effect=record):
            owner.simplify(p, np.full_like(p, 100), t, o, 4, colour_deviation=1,
                           allocation=owner.Allocation(o, ['a', 'b', 'wall', 'floor'],
                                                       props_share=0.5))
        self.assertEqual(calls, [(1, 1), (1, 1), (2, 2)])

    def test_rule_excludes_welded_objects_and_large_free_objects(self):
        p, t, o = scene()
        p[3] = p[0]
        groups = owner.object_groups(p, t, o)
        self.assertEqual(groups[0], groups[1])
        self.assertEqual(owner.prop_groups(p, t, o, groups), set())
        p, t, o = scene()
        self.assertEqual(owner.prop_groups(p, t, o, owner.object_groups(p, t, o)), {0, 1})

    def test_importance_is_an_error_for_structure(self):
        p, t, o = scene()
        with self.assertRaisesRegex(SettingsError, 'structure.*nothing to split'):
            owner.simplify(p, np.full_like(p, 100), t, o, 4, colour_deviation=1,
                           allocation=owner.Allocation(o, ['a', 'b', 'wall', 'floor'],
                                                       overrides=[{'object': 'wall', 'importance': 2}]))

    def test_fixed_overrides_cannot_borrow_from_the_other_share(self):
        p, t, o = scene()
        for name, share in [('a', 0), ('wall', 1)]:
            with self.subTest(name=name), self.assertRaisesRegex(SettingsError, 'share'):
                owner.simplify(p, np.full_like(p, 100), t, o, 4, colour_deviation=1,
                               allocation=owner.Allocation(o, ['a', 'b', 'wall', 'floor'], props_share=share,
                                                           overrides=[{'object': name, 'triangles': 1}]))

    def test_projected_area_and_importance_multiply_the_prop_weight(self):
        p, t, o = scene()
        t = np.concatenate([np.repeat(t[:1], 100, axis=0), np.repeat(t[1:2], 100, axis=0), t[2:]])
        o = np.repeat(np.arange(4), [100, 100, 1, 1])
        for eyes, rules, expected in [(None, [], [50, 50]),
                                      (None, [{'object': 'b', 'importance': 4}], [20, 80]),
                                      (np.zeros((1, 3)), [], [99, 1])]:
            calls = []
            def record(pos, rgb, tris, budget, weight, options):
                calls.append(budget)
                return pos, rgb, tris[:budget], np.arange(len(pos))
            with self.subTest(eyes=eyes, rules=rules), patch.object(owner, 'simplify_with_update', side_effect=record):
                owner.simplify(p, np.full_like(p, 100), t, o, 200, colour_deviation=1,
                               allocation=owner.Allocation(o, ['a', 'b', 'wall', 'floor'], props_share=0.5,
                                                           eyes=eyes, overrides=rules))
            self.assertEqual(calls[:2], expected)
            self.assertEqual(calls[2:], [100])

    def test_classification_uses_source_bounds_after_visibility(self):
        p, t, o = scene()
        p, t, o = p[:3], t[:1], o[:1]
        groups = owner.object_groups(p, t, o)
        self.assertEqual(owner.prop_groups(p, t, o, groups), set())
        self.assertEqual(owner.prop_groups(p, t, o, groups, source_diagonal=100), {0})
        diagonal = owner.bounds_diagonal(p)
        self.assertEqual(owner.prop_groups(p, t, o, groups,
                                          source_diagonal=diagonal / owner.PROP_DIAGONAL_FRACTION), set())

    def test_zero_prop_share_can_drop_all_props(self):
        p, t, o = scene()
        with patch.object(owner, 'simplify_with_update') as simplify_pass:
            result = owner.simplify(p[:3], p[:3], t[:1], o[:1], 4, colour_deviation=1,
                                    allocation=owner.Allocation(o[:1], props_share=0, source_diagonal=100))
        self.assertTrue(all(len(array) == 0 for array in result))
        simplify_pass.assert_not_called()

    def test_share_schema(self):
        for share in (-0.1, 1.1, True, '0.3'):
            with self.subTest(share=share), self.assertRaises(SettingsError):
                load_object_overrides({'props_share': share})
        self.assertEqual(load_object_overrides({'props_share': 0.3}), [])


if __name__ == '__main__':
    unittest.main()
