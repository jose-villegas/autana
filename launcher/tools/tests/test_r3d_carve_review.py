"""Regression checks for derived carving, stored prototypes and fitted ceilings."""

import json
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace as NS
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from r3d import mesh_bundle, mesh_import, simplify
from r3d.fit import stage
from r3d.import_settings import SettingsError
from r3d.objects import carve, detail, objects_report
from test_mesh_bundle import bundle_fixture
from test_r3d_carve import odd_scene
from test_r3d_detail import FOCAL, NEAR, sphere


class DerivedReviewTests(unittest.TestCase):
    def test_missing_camera_is_an_actionable_error(self):
        with self.assertRaisesRegex(SettingsError, 'deriving carve budgets needs a scene camera'):
            carve.views(None, NS(visibility=None))

    def test_zero_distance_is_rejected_before_simplifying(self):
        p, t = sphere()
        with patch.object(detail, 'level', side_effect=RuntimeError('entered level')):
            with self.assertRaises(AssertionError):
                detail.ladder(p, t, 0, FOCAL)

    def test_level_halves_the_requested_error_even_when_measurement_barely_misses(self):
        p, t = sphere()
        asked = []
        def keep(p, t, error):
            asked.append(error)
            if len(asked) > 40:
                self.fail('shrink did not reach float32 resolution')
            return t[:1]
        with patch.object(detail, 'simplify_to_error', side_effect=keep), \
                patch.object(detail, 'surface_error', return_value=1.00001):
            detail.level(p, t, 1.0)
        self.assertTrue(all(b <= a * detail.MAX_ERROR_SHRINK for a, b in zip(asked, asked[1:])))

    def test_stand_in_budget_grows_geometrically_when_measurement_barely_misses(self):
        p, t = sphere()
        asks = []
        def stand(p, rgb, t, mat, budget, *args):
            asks.append(budget)
            return NS(positions=p, tris=t[:1]), []
        with patch('r3d.objects.stand_ins.best_stand_in', side_effect=stand), \
                patch.object(detail, 'surface_error', side_effect=lambda *args: 1.00001 if len(asks) < 2 else 0):
            simplify.stand_in_within(p, p, t, np.zeros(len(t)), 100, 1, 0, None, 1)
        self.assertGreaterEqual(asks[1], asks[0] * simplify.MIN_BUDGET_GROWTH)

    def test_carve_selection_preserves_the_rules(self):
        p, t, m, o, names = odd_scene()
        rule = {'match': {'object': '*'}, 'shape': 'surface'}
        renderer = NS(variant=NS(triangles=1, ceiling=None), fit=None, visibility=None)
        camera = NS(component=NS(path=None, half_fov_short_tan=1, near_z=NEAR))
        selected, derived = mesh_import.carve_selection(NS(p=p, tri_o=o, object_names=names), t, m,
            {i: rule for i in range(3)}, NS(position_scale=4), NS(renderer=renderer), NS(camera=camera))
        self.assertTrue(all(value is rule for value in selected.values()))

    def test_uncarved_import_avoids_views_and_carved_import_needs_no_rules(self):
        from r3d.import_settings import load_import_settings, albedo_jobs
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'copies.obj').write_text(
                'mtllib copies.mtl\nusemtl stone\no a\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'
                'o b\nv 10 0 0\nv 11 0 0\nv 10 1 0\nf 4 5 6\n')
            (root / 'copies.mtl').write_text('newmtl stone\nKd 1 1 1\n')
            path = root / 'copies.import.toml'
            path.write_text('[source]\npath="copies.obj"\ncredit="test"\n[output]\nposition_scale=4\n'
                            '[geometry]\nsimplify={dense_edge=100.0, colour_deviation=1.0, props=[], props_share=0.0, seal_seams=false}\n'
                            '[[variants]]\nname="copies"\ntriangles=10\nceiling=20\n')
            job = albedo_jobs(load_import_settings(path))[0]
            with patch.object(mesh_import, 'views', side_effect=AssertionError('uncarved needs no view')):
                flat = mesh_import.bake_geometry(job, None, carve=False)
            self.assertIsNone(flat.shapes)
            self.assertEqual(len(flat.tris), 2)
            camera = NS(component=NS(path=None, half_fov_short_tan=1, near_z=NEAR))
            carved = mesh_import.bake_geometry(job, NS(camera=camera), carve=True)
            self.assertIsNotNone(carved.shapes)
            self.assertEqual(len(carved.shapes), 1)
            self.assertEqual(len(carved.shapes[0]['copies']), 2)

    def test_prototype_accounting_matches_the_writer_for_handedness_and_sidedness(self):
        from r3d.objects.budget import bundle_count, stored_count
        with tempfile.TemporaryDirectory() as directory:
            blob, _, geometry, _, _ = bundle_fixture(directory, materials=(0, 1, 0))
        entries, copies = mesh_bundle.decode(blob)
        count = stored_count(geometry.shapes[0], [1])
        self.assertEqual(count, 3)
        self.assertEqual(count, bundle_count(entries, [row['mesh'] for row in copies]))

    def test_simplify_ceiling_counts_both_mirrored_prototypes(self):
        from test_r3d_mesh_policies import MeshPolicyTests, REST_ASK
        from r3d.objects.budget import prototype_groups, stored_count
        p, t, o, allocation = MeshPolicyTests._three_spheres()
        allocation.shapes, allocation.report = [], {}
        result = simplify.simplify(p, np.full_like(p, 80), t, np.zeros(len(t), dtype=int), REST_ASK,
                                   colour_deviation=1, allocation=allocation)
        self.assertEqual(len(prototype_groups(allocation.shapes[0], ())), 2)
        paid = sum(stored_count(shape, ()) for shape in allocation.shapes)
        self.assertEqual(allocation.report['stored_total'], len(result[2]) + paid)
        allocation.shapes.clear()
        allocation.ceiling = len(result[2]) + paid // 2
        allocation.split = (allocation.ceiling, paid // 2, len(result[2]))
        out = simplify.simplify(p, np.full_like(p, 80), t, np.zeros(len(t), dtype=int), REST_ASK,
                                colour_deviation=1, allocation=allocation)
        self.assertLessEqual(len(out[2]) + paid, allocation.ceiling)

    def test_surface_rule_merges_different_triangulations_with_derived_detail(self):
        p = np.array([[0., 0, 0], [2, 0, 0], [2, 2, 0], [0, 2, 0],
                      [10, 0, 0], [12, 0, 0], [12, 2, 0], [10, 2, 0], [11, 1, 0]])
        t = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 8], [5, 6, 8], [6, 7, 8], [7, 4, 8]])
        o, m = np.array([0, 0, 1, 1, 1, 1]), np.zeros(len(t), dtype=int)
        rule = {'match': {'object': '*'}, 'shape': 'surface', 'seam': 'free'}
        renderer = NS(variant=NS(triangles=1, ceiling=None), fit=None, visibility=None)
        camera = NS(component=NS(path=None, half_fov_short_tan=1, near_z=NEAR))
        selected, derived = mesh_import.carve_selection(NS(p=p, tri_o=o, object_names=['a', 'b']), t, m,
            {0: rule, 1: rule}, NS(position_scale=4), NS(renderer=renderer), NS(camera=camera))
        allocation = simplify.Allocation(tri_o=o, object_names=['a', 'b'], meshes=(rule,), selection=selected,
                                        shapes=[], report={})
        allocation.detail = derived
        simplify.simplify(p, np.full_like(p, 80), t, m, 1, colour_deviation=1, allocation=allocation)
        self.assertEqual(len(allocation.shapes), 1)
        self.assertEqual(len(allocation.shapes[0]['copies']), 2)

    def test_authoring_split_counts_emitted_prototypes(self):
        renderer = NS(variant=NS(triangles=10, ceiling=20), fit=NS(budget=10))
        report = {'passes': [dict(stand_in='original', output_triangles=2, stored_triangles=4)]}
        split = objects_report.budget_split(renderer, report, [])
        self.assertEqual((split['copies'], split['rest']), (4, 16))
        job = NS(renderer=renderer, object=NS(name='room'))
        with tempfile.TemporaryDirectory() as directory, patch.object(mesh_import, 'bake_geometry', return_value=NS(mesh_report=report)):
            out = pathlib.Path(directory)
            objects_report.write_authoring(None, job, out)
            self.assertIn('| 20 | 10 | 4 | 16 | 0 |', (out / 'authoring.md').read_text())
            self.assertEqual(json.loads((out / 'detail.json').read_text()), [])


class FitCeilingReviewTests(unittest.TestCase):
    def run_fit(self, ceiling, output_extra=False):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        work = pathlib.Path(directory.name)
        recipe = NS(budget=2, steps=1, batch=1, laplacian=0, normal_weight=0, surface_weight=0, surface_margin=0)
        blob, copies, geometry, job, scene = bundle_fixture(work, fit=recipe)
        job.renderer.variant = NS(name='start', triangles=2, ceiling=ceiling)
        (work / 'start.bundle').write_bytes(blob)
        calls = []
        def fake_fit(command):
            calls.append(command)
            out = work / 'fitted'
            out.mkdir()
            fitted = (work / (job.asset_name + '.bundle')).read_bytes()
            if output_extra:
                from r3d.lit_mesh import build_lit_mesh, bundle_entries
                positions = np.concatenate([geometry.positions + [i * 8, 0, 0] for i in range(3)])
                triangles = np.concatenate([geometry.tris + i * len(geometry.positions) for i in range(3)])
                mesh = build_lit_mesh(positions, np.tile(geometry.rgb, (3, 1)), triangles, np.zeros(3, dtype=int))
                fitted = mesh_bundle.encode(bundle_entries(job.asset_name, mesh), [])
            (out / (job.asset_name + '.bundle')).write_bytes(fitted)
        with patch('r3d.appearance_simplify.main', side_effect=fake_fit):
            stage.fit('scene', scene, job, work)
        return calls

    def test_fit_counts_each_prototype_once_despite_repeated_placements(self):
        calls = self.run_fit(10)
        self.assertEqual(calls[0][calls[0].index('--budget') + 1], '8')

    def test_prune_floor_must_not_exceed_variant_ceiling(self):
        from r3d.import_settings import load_fit
        recipe = dict(prune=dict(budget=3, coverage_every_ms=1),
                      poses=dict(size=[2, 2], train_every_ms=1, held_out_every_ms=2),
                      optimise=dict(steps=1, batch=1, laplacian=1, normal_weight=0,
                                    surface_weight=10.0, surface_margin=0.25))
        with self.assertRaisesRegex(SettingsError, 'prune.budget.*ceiling'):
            load_fit(recipe, NS(triangles=4, ceiling=2), 'fit')

    def test_rest_may_shrink_below_floor_after_copies_exhaust_their_rungs(self):
        calls = self.run_fit(3)
        self.assertEqual(calls[0][calls[0].index('--budget') + 1], '1')

    def test_copies_alone_over_ceiling_fail_before_the_gpu_fit(self):
        with patch('r3d.appearance_simplify.main') as gpu:
            with self.assertRaisesRegex(SettingsError, 'copies total 2 exceeds ceiling 1'):
                self.run_fit(1)
            gpu.assert_not_called()

    def test_fitted_bundle_is_checked_against_the_ceiling(self):
        with self.assertRaisesRegex(SettingsError, 'ceiling'):
            self.run_fit(4, output_extra=True)


class SourcePlacementTests(unittest.TestCase):
    def test_part_order_uses_relative_tolerance_across_absolute_rounding_boundaries(self):
        from r3d.obj import load_obj
        from r3d.source.reduce_source import reduce
        from test_reduce_source import write_source
        anchor = np.array([[0., 0, 0], [0, 10, 0], [0, 0, 10]])
        small = np.array([[0., 0, 0], [0, 0.1, 0], [0, 0, 0.1]])
        faces = np.arange(9).reshape(-1, 3)
        first = np.concatenate([anchor, small + [0.049, 3, 0], small + [0.051, -3, 0]])
        second = np.concatenate([anchor + [10, 0, 0], small + [10.051, 3, 0], small + [10.049, -3, 0]])
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_source(root, [('a', first, faces), ('b', second, faces)])
            reduce(root, 'copies', root / 'out', 0)
            p, _, t, _, _, _, o, _ = load_obj(str(root / 'out' / 'copies.obj'))
        orders = [np.sign(p[t[o == owner]].mean(axis=1)[:, 1]) for owner in (0, 1)]
        np.testing.assert_array_equal(orders[0], orders[1])


class MeasuredBalanceTests(unittest.TestCase):
    def carve(self, room, counts):
        p, t, m, o, names = odd_scene()
        rule = {'match': {'object': 'ball_*'}, 'seam': 'free'}
        ladder = [detail.Rung(count, detail.TOLERANCE_PX * 2 ** i) for i, count in enumerate(counts)]
        renderer = NS(variant=NS(triangles=1, ceiling=room + 1), fit=NS(budget=1))
        with patch.object(mesh_import, 'views', return_value=([], FOCAL, NEAR)), \
                patch.object(detail, 'ladder', return_value=ladder):
            selected, derived = mesh_import.carve_selection(NS(p=p, tri_o=o, object_names=names), t, m,
                {0: rule, 1: rule}, NS(position_scale=4), NS(renderer=renderer), object())
        info = derived.shapes[0]
        allocation = simplify.Allocation(tri_o=o, object_names=names, selection=selected, shapes=[],
                                        meshes=(rule,), report={})
        allocation.detail = derived
        allocation.split = (None, room, 1)
        asks = []
        def stand(p, rgb, t, mat, budget, deviation, options, lock, error):
            asks.append(budget)
            count = budget + 4 if budget == counts[0] else budget
            return NS(positions=p, rgb=rgb, tris=t[:count], materials=mat[:count], kept=None,
                      kind='original'), [], budget, error
        with patch.object(simplify, 'stand_in_within', side_effect=stand):
            simplify.simplify(p, np.full_like(p, 80), t, m, 1, colour_deviation=1, allocation=allocation)
        return allocation, info, asks

    def test_growth_after_near_level_is_paid_by_a_measured_coarser_rung(self):
        allocation, info, asks = self.carve(5, [4, 2])
        self.assertEqual(info['pick'], 1)
        self.assertEqual(asks, [4, 2])
        self.assertEqual(allocation.report['passes'][0]['stored_triangles'], 2)

    def test_exhausted_rungs_allow_rest_below_floor_and_report_the_shortfall(self):
        allocation, info, _ = self.carve(2, [4, 3])
        self.assertEqual(info['pick'], 1)
        renderer = NS(variant=NS(triangles=1, ceiling=3), fit=NS(budget=1))
        rows = [dict(shape='ball_0', rung=1, near_triangles=4, carved_triangles=3, coarsest=True)]
        split = objects_report.budget_split(renderer, allocation.report, rows)
        self.assertEqual((split['rest'], split['shortfall'], split['coarsest']), (0, 1, ['ball_0']))
        allocation.report['detail'] = rows
        with tempfile.TemporaryDirectory() as directory, patch.object(mesh_import, 'bake_geometry', return_value=NS(mesh_report=allocation.report)):
            out = pathlib.Path(directory)
            objects_report.write_authoring(None, NS(renderer=renderer, object=NS(name='room')), out)
            text = (out / 'authoring.md').read_text()
            self.assertIn('| 3 | 1 | 3 | 0 | 1 |', text)
            self.assertIn('ball_0', text)

    def test_ceiling_past_last_rung_fails_with_room_and_total(self):
        with self.assertRaisesRegex(SettingsError, 'total 8 exceeds ceiling 2'):
            self.carve(1, [4])


if __name__ == '__main__':
    unittest.main()
