"""Thin surface detection, curve fitting and the LCRV encoding on built shapes."""

import pathlib
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.thin.thin_parts import ThinPart, budget_side, find_thin_parts
from r3d.thin.curve_fit import CURVE_LIGHT_RING, TUBE_SIDES, Curve, fit_curve, light_curve
from r3d.thin.curve_asset import read_curves, write_curves
from r3d.import_settings import SettingsError, load_geometry
from r3d.light import LIGHT_GAMMA
from tests import soup

SIDE = 53.5
SCALE = 64


def cylinder():
    angle = np.arange(96) * 2 * np.pi / 96
    p = np.array([[2 * np.cos(a), 2 * np.sin(a), z] for z in (0, 200) for a in angle])
    t = np.array([[i, (i + 1) % len(angle), i + len(angle)] for i in range(len(angle))] +
                 [[(i + 1) % len(angle), (i + 1) % len(angle) + len(angle), i + len(angle)] for i in range(len(angle))])
    return p, t


class ThinCurvesTests(unittest.TestCase):
    def test_thin_box(self):
        shape = soup.box((4.5, 4.5, 310))
        self.assertEqual(len(find_thin_parts(shape.vertices, shape.faces, SIDE)), 1)

    def test_triangle_sliver(self):
        p = np.array([[0, 0, 0], [3.6, 0, 0], [0, 0, 83]])
        self.assertEqual(len(find_thin_parts(p, np.array([[0, 1, 2]]), SIDE)), 1)

    def test_leaf_strip(self):
        p = np.array([[x, np.sin(z / 59), z] for z in np.linspace(0, 59, 10) for x in (0, 9)])
        t = np.array([[2*i, 2*i+1, 2*i+2] for i in range(9)] + [[2*i+1, 2*i+3, 2*i+2] for i in range(9)])
        self.assertEqual(find_thin_parts(p, t, SIDE), [])

    def test_band(self):
        shape = soup.box((36, 36, 330))
        self.assertEqual(find_thin_parts(shape.vertices, shape.faces, SIDE), [])

    def test_short_part(self):
        shape = soup.box((4.5, 4.5, 50))
        self.assertEqual(find_thin_parts(shape.vertices, shape.faces, SIDE), [])

    def test_position_weld(self):
        shape = soup.box((4.5, 4.5, 155))
        p, t = shape.vertices, shape.faces
        p = np.concatenate([p, p + [0, 0, 155]])
        parts = find_thin_parts(p, np.concatenate([t, t + len(p)//2]), SIDE)
        self.assertEqual(len(parts), 1)
        self.assertEqual(len(parts[0].triangles), 2 * len(t))

    def test_budget_area(self):
        p = np.array([[0, 0, 0], [4, 0, 0], [0, 2, 0]])
        self.assertEqual(budget_side(p, np.array([[0, 1, 2]]), 4), 1)

    def test_cylinder_fit(self):
        p, t = cylinder()
        for seed in (0, 1, 19):
            with self.subTest(seed=seed), mock.patch('r3d.thin.curve_fit.SAMPLE_SEED', seed):
                curve = fit_curve(p, t, find_thin_parts(p, t, SIDE)[0])
                self.assertEqual(len(curve.knots), 2)
                np.testing.assert_allclose(curve.radius, 2, rtol=0.05)
                np.testing.assert_allclose(sorted(curve.knots[:, 2]), [0, 200], atol=1e-10)

    def test_bend_fit(self):
        p, t = cylinder()
        ring = p[:96]
        p = np.concatenate([ring, ring + [0, 0, 100], ring + [100, 0, 100]])
        t = np.concatenate([t, t + 96])
        axes = np.array([[1, -1, 0], [0, 0, np.sqrt(2)], [1, 1, 0]]) / np.sqrt(2)
        part = ThinPart(np.arange(len(t)), p.mean(axis=0), axes, np.array([145, 4, 4]))
        curve = fit_curve(p, t, part)
        self.assertGreater(len(curve.knots), 2)
        self.assertLess(np.linalg.norm(curve.knots - [0, 0, 100], axis=1).min(), 6)

    def test_asset_round_trip(self):
        curve = Curve(np.array([[1.23, -2.1, 3.5], [4.1, 5, 6.23]]), np.array([0.25, 0.7]),
                      np.array([[1, 0, 0], [0, 1, 0]], dtype=float))
        result, = read_curves(write_curves([curve], SCALE), SCALE)
        np.testing.assert_allclose(curve.knots, result.knots, atol=1/SCALE)
        np.testing.assert_allclose(curve.radius, result.radius, atol=1/SCALE)
        np.testing.assert_allclose(curve.color, result.color)

    def test_ring_lighting(self):
        p, t = cylinder()
        part = find_thin_parts(p, t, SIDE)[0]
        curve = fit_curve(p, t, part)
        source = SimpleNamespace(p=p, tri_v=t, tri_t=t, tri_m=np.zeros(len(t), dtype=int),
                                 uv=np.zeros((len(p), 2)), materials={'surface': {'Kd': (0.25, 0.25, 0.25)}},
                                 names=['surface'], textures=[None], colors=None)
        from r3d.light import light
        with mock.patch('r3d.light.light', wraps=light) as shade:
            lit = light_curve(curve, source, part, None,
                              [{'type': 'ambient', 'color': [1, 1, 1], 'intensity': 2}], 0.5)
        points, normals = shade.call_args.args[:2]
        self.assertEqual(len(points), len(curve.knots) * CURVE_LIGHT_RING)
        np.testing.assert_allclose(np.linalg.norm(normals, axis=1), 1)
        radial = points - np.repeat(curve.knots, CURVE_LIGHT_RING, axis=0)
        np.testing.assert_allclose((radial * normals).sum(axis=1), np.repeat(curve.radius, CURVE_LIGHT_RING))
        np.testing.assert_allclose(lit.color, 2 * 0.25 ** LIGHT_GAMMA)
        np.testing.assert_array_equal(lit.knots, curve.knots)

    def test_frozen_tubes(self):
        from r3d.fit.copies import placed_curves, joined
        curve = Curve(np.array([[0., 0, 0], [0., 0, 10]]), np.ones(2), np.ones((2, 3)))
        for flat in (False, True):
            frozen = placed_curves([curve], flat)
            self.assertEqual(len(frozen.tris), 2 * TUBE_SIDES + 2 * (TUBE_SIDES - 2))
            self.assertEqual(len(frozen.points), 2 * TUBE_SIDES)
            self.assertEqual(len(frozen.rgb), len(frozen.tris) if flat else len(frozen.points))
            before = frozen.points.copy()
            both = joined(frozen, frozen)
            self.assertEqual(len(both.tris), 2 * len(frozen.tris))
            np.testing.assert_array_equal(frozen.points, before)

    def test_start_excludes_curve_triangles(self):
        from r3d import mesh_import
        from r3d.import_settings import albedo_jobs, load_import_settings
        from r3d.thin import curve_asset
        from r3d import mesh_bundle, mesh_asset

        thin = soup.box((4.5, 4.5, 310))
        body = soup.box((50, 50, 50))
        p, t = thin.vertices, thin.faces
        rest, rest_t = body.vertices, body.faces
        rest += [125, 0, 0]
        source = SimpleNamespace(p=np.concatenate([p, rest]), tri_v=np.concatenate([t, rest_t + len(p)]),
                                 tri_m=np.zeros(len(t) + len(rest_t), dtype=int), names=['surface'],
                                 tri_o=np.zeros(len(t) + len(rest_t), dtype=int), object_names=['object'],
                                 materials={'surface': {'Kd': (1, 1, 1)}}, textures=[None], colors=None,
                                 uv=np.zeros((len(p) + len(rest), 2)), tri_t=np.concatenate([t, rest_t + len(p)]))
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'mesh.import.toml'
            path.write_text('[source]\npath = "mesh.obj"\ncredit = "test"\n[output]\n'
                            '[geometry]\ncurves = true\n'
                            'simplify = { dense_edge = 1000, props = [], props_share = 0.3, '
                            'seal_seams = true, colour_deviation = 1.0 }\n'
                            '[[variants]]\nname = "mesh"\ntriangles = 8\n')
            job, = albedo_jobs(load_import_settings(path))
            self.assertEqual(job.asset_path.suffix, mesh_bundle.SUFFIX)
            with mock.patch.object(mesh_import, 'load_source', return_value=source), \
                    mock.patch.object(mesh_import, 'simplify', side_effect=lambda p, c, t, m, *a, **k: (p, c, t, m)):
                geometry = mesh_import.bake_geometry(job, None)
            self.assertEqual(len(geometry.curves), 1)
            self.assertEqual(len(geometry.tris), len(rest_t))
            self.assertGreaterEqual(geometry.positions[:, 0].min(), 100)
            mesh_import.write_baked(job, None, folder, 'mesh', geometry)
            entries, copies = mesh_bundle.decode((pathlib.Path(folder) / 'mesh.bundle').read_bytes())
            self.assertEqual(copies, [])
            self.assertEqual({kind for _, kind, _ in entries}, {mesh_asset.TYPE, curve_asset.TYPE})
            from r3d.fit.copies import placed_copies
            frozen = placed_copies(pathlib.Path(folder) / 'mesh.bundle', False)
            self.assertGreater(len(frozen.tris), 0)
            self.assertLess(frozen.points[:, 0].max(), 100)
            source.p, source.tri_v, source.tri_t = p, t, t
            source.tri_m = source.tri_o = np.zeros(len(t), dtype=int)
            source.uv = np.zeros((len(p), 2))
            with mock.patch.object(mesh_import, 'load_source', return_value=source):
                only_curves = mesh_import.bake_geometry(job, None)
            self.assertEqual(len(only_curves.tris), 0)
            mesh_import.write_baked(job, None, folder, 'mesh', only_curves)
            frozen = placed_copies(pathlib.Path(folder) / 'mesh.bundle', False)
            self.assertGreater(len(frozen.tris), 0)

    def test_curve_only_start(self):
        from r3d.lit_mesh import bake_lit_mesh, mesh_blob
        from r3d.mesh_asset import read_mesh_header
        mesh = bake_lit_mesh(np.zeros((1, 3)), np.ones((1, 3)), np.empty((0, 3), dtype=int), np.empty(0))
        self.assertEqual(read_mesh_header(mesh_blob(mesh)).triangles, 0)
        self.assertEqual(len(mesh.nodes), 1)
        self.assertEqual(len(mesh.clusters), 1)

    def test_settings_default(self):
        steps = SimpleNamespace()
        load_geometry({}, steps)
        self.assertFalse(steps.curves)

    def test_settings_type(self):
        with self.assertRaises(SettingsError):
            load_geometry({'curves': 1}, SimpleNamespace())


if __name__ == '__main__':
    unittest.main()
