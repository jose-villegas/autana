"""SH probe projection, wire contract, filling and isolated bake keys."""
import math
import pathlib
import sys
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import sh_probe_bake as sh
from r3d.views.probes import sphere_directions
from bake import bake


class ProbeTests(unittest.TestCase):
    def test_constant_sphere(self):
        directions = sphere_directions(sh.DIRECTIONS)
        colour = np.array([0.2, 0.6, 1.0])
        got = sh.project(directions, np.broadcast_to(colour, (len(directions), 3)))
        np.testing.assert_allclose(got[0], colour * math.sqrt(4 * math.pi))
        np.testing.assert_allclose(got[1:], 0, atol=4 * math.pi * sh.Y1 / len(directions))

    def test_direct_sky_reaches_the_record(self):
        from types import SimpleNamespace
        from tests.soup import corridor
        from r3d.sh_probe_settings import load

        source = corridor(albedo=0)
        points = np.array([[0.5, 0.5, 0.0]])
        settings = SimpleNamespace(sh_probes=load({}), seed=0, alpha_keep=None, double_sided=set())
        indirect = SimpleNamespace(bounces=2, rays=1)
        job = SimpleNamespace(settings=settings, bake=SimpleNamespace(indirect=indirect))
        colour = np.array([0.4, 0.6, 1.0])
        scene = SimpleNamespace(lights=[dict(type='sky', color=colour.tolist(), intensity=1.0)],
                                camera=SimpleNamespace(component=SimpleNamespace(near_z=0.001)),
                                indirect=SimpleNamespace(albedo_boost=1.0))
        lattice = (points, 1.0, points[0], np.ones(3), np.ones(3, dtype=int))
        with mock.patch.object(sh, 'load_source', return_value=source), mock.patch.object(sh, 'grid', return_value=lattice):
            data = sh.bake(job, scene)
        directions = sphere_directions(sh.DIRECTIONS)
        visible = (directions[:, 0] > 0) & (directions[:, 1] > 0)
        expected = sh.project(directions, visible[:, None] * colour)
        got = sh.decode(data)[-1][0]
        np.testing.assert_allclose(got, expected, atol=0.02)

    def test_single_direction(self):
        direction = np.array([[1, 2, 3]]) / math.sqrt(14)
        colour = np.array([[0.2, 0.4, 0.8]])
        expected = np.array([sh.Y00, sh.Y1 * 2 / math.sqrt(14),
                             sh.Y1 * 3 / math.sqrt(14), sh.Y1 / math.sqrt(14)])[:, None]
        np.testing.assert_allclose(sh.project(direction, colour), 4 * math.pi * expected * colour)

    def test_sun_against_traced_disc(self):
        from r3d.ray_query import RayQuery
        # A finite disc subtends a small solid angle at the origin, normalized to the delta light's power.
        radius, distance, samples = 0.02, 1.0, 4096
        angle = np.arange(samples) * math.pi * (3 - math.sqrt(5))
        radial = radius * np.sqrt((np.arange(samples) + 0.5) / samples)
        targets = np.column_stack((radial * np.cos(angle), np.full(samples, distance), radial * np.sin(angle)))
        directions = targets / np.linalg.norm(targets, axis=1)[:, None]
        query = RayQuery(np.array([[-2, 0.5, -2], [2, 0.5, -2], [0, 0.5, 2]]), np.array([[0, 1, 2]]))
        sun = {"type": "directional", "direction": [0, 1, 0], "color": [1, 0.5, 0.2], "intensity": 2}
        for origin in ([0, 0, 0], [4, 0, 0]):
            origins = np.broadcast_to(origin, directions.shape)
            visible = ~query.blocked(origins, directions)
            traced = math.pi * np.einsum('nk,n,nc->kc', sh.basis(directions), visible,
                                         np.broadcast_to(np.array(sun['color']) * sun['intensity'], directions.shape)) / samples
            got = sh.sun_term(query, np.array([origin]), [sun])[0]
            np.testing.assert_allclose(got, traced, atol=4e-4)

    def test_dilation_is_synchronous_and_does_not_wrap_edges(self):
        values = np.zeros((1, 1, 5, sh.COEFFICIENTS, sh.CHANNELS))
        values[0, 0, 0] = 2
        values[0, 0, 4] = 6
        inside = np.array([[[False, True, True, True, False]]])
        np.testing.assert_array_equal(sh.dilate(values, inside)[0, 0, :, 0, 0], [2, 2, 4, 6, 6])
        with self.assertRaises(ValueError):
            sh.dilate(values, np.ones_like(inside))

    def test_record_round_trip_and_golden(self):
        coefficients = np.arange(24).reshape(2, sh.COEFFICIENTS, sh.CHANNELS) - 12
        data = sh.encode([1, 2, 3], [4, 5, 6], [2, 1, 1], coefficients)
        origin, spacing, dims, scale, decoded = sh.decode(data)
        np.testing.assert_array_equal(origin, [1, 2, 3])
        np.testing.assert_array_equal(spacing, [4, 5, 6])
        self.assertEqual(tuple(dims), (2, 1, 1))
        np.testing.assert_allclose(decoded, coefficients, atol=scale / 2 + 1e-6)
        self.assertEqual(len(data), sh.HEADER.size + coefficients.size * np.dtype('<i2').itemsize)
        fixture = pathlib.Path(__file__).parent / 'data/sh_probes.shpr'
        golden = fixture.read_bytes()
        self.assertEqual(golden[:2], bytes([sh.VERSION, sh.DIRECT_INCLUDED]))
        _, _, dims, scale, decoded = sh.decode(golden)
        self.assertEqual(tuple(dims), (2, 1, 1))
        self.assertEqual(scale, 0.25)
        np.testing.assert_array_equal(decoded, coefficients * scale)
        for bad in (data[:-1], data + b'\0', bytes([0]) + data[1:]):
            with self.assertRaises(ValueError):
                sh.decode(bad)

    def test_tool_key_is_local(self):
        before = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        original = bake.code_tokens
        def changed(path, *args, **kwargs):
            tokens = original(path, *args, **kwargs)
            return tokens + [('NAME', 'changed')] if pathlib.Path(path).name == 'sh_probe_bake.py' else tokens
        bake.tool_digest.cache_clear()
        with mock.patch.object(bake, 'code_tokens', changed):
            after = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        bake.tool_digest.cache_clear()
        self.assertNotEqual(before['sh_probes'], after['sh_probes'])
        self.assertEqual({k: v for k, v in before.items() if k != 'sh_probes'},
                         {k: v for k, v in after.items() if k != 'sh_probes'})
        def unrelated(path, *args, **kwargs):
            tokens = original(path, *args, **kwargs)
            return tokens + [('NAME', 'changed')] if pathlib.Path(path).name == 'mesh_import.py' else tokens
        with mock.patch.object(bake, 'code_tokens', unrelated):
            self.assertEqual(before['sh_probes'], bake.tool_digest('sh_probes'))
        bake.tool_digest.cache_clear()



if __name__ == '__main__':
    unittest.main()
