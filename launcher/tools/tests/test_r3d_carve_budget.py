"""Checks the derived carve budgets, the stored prototypes they pay for and the fitted ceiling split."""

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
from test_r3d_detail import sphere
from tests.r3d_env import needs_llvm  # noqa: E402


class DerivedDetailTests(unittest.TestCase):


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


    @staticmethod
    def two_copies_job(directory, triangles, ceiling):
        """The albedo job of one stone triangle placed as objects a and b, at a variant's budget."""
        from r3d.import_settings import load_import_settings, albedo_jobs
        root = pathlib.Path(directory)
        (root / 'copies.obj').write_text(
            'mtllib copies.mtl\nusemtl stone\no a\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'
            'o b\nv 10 0 0\nv 11 0 0\nv 10 1 0\nf 4 5 6\n')
        (root / 'copies.mtl').write_text('newmtl stone\nKd 1 1 1\n')
        path = root / 'copies.import.toml'
        path.write_text('[source]\npath="copies.obj"\ncredit="test"\n[output]\nposition_scale=4\n'
                        '[geometry]\nsimplify={dense_edge=100.0, colour_deviation=1.0, seal_seams=false}\n'
                        f'[[variants]]\nname="copies"\ntriangles={triangles}\nceiling={ceiling}\n')
        return albedo_jobs(load_import_settings(path))[0]

    @needs_llvm
    def test_uncarved_import_avoids_views_and_carved_import_needs_no_rules(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.two_copies_job(directory, 10, 20)
            flat = mesh_import.bake_geometry(job, None, carve=False)
            self.assertIsNone(flat.shapes)
            self.assertEqual(len(flat.tris), 2)
            carved = mesh_import.bake_geometry(job, NS(), carve=True)
            self.assertIsNotNone(carved.shapes)
            self.assertEqual(len(carved.shapes), 1)
            self.assertEqual(len(carved.shapes[0]['copies']), 2)

    @needs_llvm
    def test_the_ceiling_pays_a_prototype_once_however_many_copies_draw_it(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.two_copies_job(directory, 2, 1)
            carved = mesh_import.bake_geometry(job, NS(), carve=True)
        self.assertEqual(len(carved.shapes[0]['copies']), 2)
        self.assertEqual(carved.mesh_report['scene_total'], 2, "two copies draw the one stored triangle")
        self.assertEqual(carved.mesh_report['stored_total'], 1)

    def test_prototype_accounting_matches_the_writer_for_handedness_and_sidedness(self):
        from r3d.objects.budget import bundle_count, prototype_groups, stored_count
        with tempfile.TemporaryDirectory() as directory:
            blob, _, geometry, _, _ = bundle_fixture(directory, materials=(0, 1, 0))
        entries, copies = mesh_bundle.decode(blob)
        count = stored_count(prototype_groups(geometry.shapes[0], [1]))
        self.assertEqual(count, 3)
        self.assertEqual(count, bundle_count(entries, [row['mesh'] for row in copies]))


class FitCeilingTests(unittest.TestCase):
    def run_fit(self, ceiling, output_extra=False):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        work = pathlib.Path(directory.name)
        recipe = NS(steps=1, batch=1, laplacian=0, normal_weight=0, surface_weight=0, surface_margin=0)
        blob, copies, geometry, job, scene = bundle_fixture(work, fit=recipe)
        job.renderer.variant = NS(name='start', triangles=len(copies) + len(geometry.tris), ceiling=ceiling)
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
        calls = self.run_fit(3)
        self.assertTrue(calls)


    def test_fitted_bundle_is_checked_against_the_ceiling(self):
        with self.assertRaisesRegex(SettingsError, 'ceiling'):
            self.run_fit(4, output_extra=True)

    def test_fitted_bundle_checks_drawn_when_stored_fits(self):
        with self.assertRaisesRegex(SettingsError, r"6 drawn triangles \(budget 4\).*5 stored triangles \(ceiling 5\)"):
            self.run_fit(5, output_extra=True)


class CarvedReportTests(unittest.TestCase):
    def test_each_carved_shape_counts_its_copies_cut_texels_and_measured_error_with_its_baked_level_on_its_source(self):
        with tempfile.TemporaryDirectory() as directory:
            blob, copies, geometry, job, _ = bundle_fixture(directory)
        entries, rows = mesh_bundle.decode(blob)
        cut = NS(levels=[np.zeros((1, 1, 4))])
        positions = np.concatenate([placed for _, (placed, *_) in copies])
        faces = np.concatenate([own + 3 * k for k, (_, (_, _, own, _)) in enumerate(copies)])
        # Copies a and b share one mesh; c is mirrored, so it is its own, and its one triangle cuts texels.
        src = NS(p=positions, tri_v=faces, tri_o=np.arange(3), tri_m=np.array([0, 0, 1]), textures=[None, cut],
                 object_names=['a', 'b', 'c'])
        shapes, rest = objects_report.carved_shapes(src, entries, rows)
        self.assertEqual([(s['shape'], s['copies'], s['source'], s['baked'], s['drawn'], s['alpha_tested'])
                          for s in shapes], [('a', 2, 1, 1, 2, 0.0), ('c', 1, 1, 1, 1, 1.0)])
        self.assertEqual(rest, len(geometry.tris))
        mirrored = shapes[1]
        np.testing.assert_allclose(np.sort(mirrored['baked_positions'], axis=0), np.sort(positions[6:9], axis=0),
                                   atol=1e-2)
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory)
            (out / 'start.bundle').write_bytes(blob)
            objects_report.write_carved(job, src, out / 'start.bundle', out)
            table = (out / 'carved.md').read_text().splitlines()
            self.assertTrue((out / 'carved.png').stat().st_size)
        self.assertIn('| a | 2 | 1 | 1 | 2 | 0% | 0 | 0.283 |', table)
        self.assertIn('| c | 1 | 1 | 1 | 1 | 100% | 0 | 0.283 |', table)


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


if __name__ == '__main__':
    unittest.main()
