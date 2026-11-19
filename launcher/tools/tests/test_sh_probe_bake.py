"""SH probe projection, wire contract, filling and isolated bake keys."""
import math
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import sh_probe_bake as sh
from r3d.sh_probe_settings import DIRECTIONS, SPP
from r3d.views.probes import sphere_directions
from bake import bake
from asset.engine_frame import AXIS_SIGNS, Z_AXIS


class ProbeTests(unittest.TestCase):
    def test_constant_sphere(self):
        directions = sphere_directions(DIRECTIONS)
        colour = np.array([0.2, 0.6, 1.0])
        got = sh.project(directions, np.broadcast_to(colour, (len(directions), 3)))
        np.testing.assert_allclose(got[0], colour * math.sqrt(4 * math.pi))
        np.testing.assert_allclose(got[1:], 0, atol=4 * math.pi * sh.Y1 / len(directions))

    def test_direct_sky_reaches_the_record(self):
        from types import SimpleNamespace
        from tests.soup import corridor

        source = corridor(albedo=0)
        points = np.array([[0.5, 0.5, 0.0]])
        settings = SimpleNamespace(path=None, seed=0, alpha_keep=None, double_sided=set())
        requested = SimpleNamespace(spacing=None, directions=DIRECTIONS, spp=SPP)
        indirect = SimpleNamespace(bounces=2, rays=1)
        job = SimpleNamespace(settings=settings, bake=SimpleNamespace(indirect=indirect))
        colour = np.array([0.4, 0.6, 1.0])
        scene = SimpleNamespace(path=None, lights=[dict(type='sky', color=colour.tolist(), intensity=1.0)],
                                camera=SimpleNamespace(component=SimpleNamespace(near_z=0.001)),
                                indirect=SimpleNamespace(albedo_boost=1.0))
        lattice = (points, 1.0, points[0], np.ones(3), np.ones(3, dtype=int))
        with mock.patch.object(sh, 'load', return_value=requested), \
                mock.patch.object(sh, 'load_source', return_value=source), mock.patch.object(sh, 'grid', return_value=lattice):
            data = sh.bake(job, scene)
        directions = sphere_directions(DIRECTIONS)
        visible = (directions[:, 0] > 0) & (directions[:, 1] > 0)
        expected = sh.project(directions, visible[:, None] * colour)
        got = sh.decode(data)[-1][0]
        np.testing.assert_allclose(got, expected, atol=0.02)

    def test_grid_wire_order(self):
        from types import SimpleNamespace
        from tests.soup import corridor

        dims = np.array([2, 3, 4])
        cells = np.stack(np.meshgrid(*map(np.arange, dims), indexing='ij'), axis=-1).reshape(-1, 3)
        points = cells.astype(float)
        requested = SimpleNamespace(spacing=None, directions=DIRECTIONS, spp=1)
        settings = SimpleNamespace(path=None, seed=0, alpha_keep=None, double_sided=set())
        indirect = SimpleNamespace(bounces=2)
        job = SimpleNamespace(settings=settings, bake=SimpleNamespace(indirect=indirect))
        scene = SimpleNamespace(path=None, lights=[], camera=SimpleNamespace(component=SimpleNamespace(near_z=0.001)),
                                indirect=SimpleNamespace(albedo_boost=1.0))
        transport = mock.Mock()
        transport._radiance.side_effect = lambda origins, rays, seed: origins * (1 + rays[:, Z_AXIS, None])
        lattice = (points, 1.0, np.zeros(3), np.ones(3), dims)
        with mock.patch.object(sh, 'load', return_value=requested), \
                mock.patch.object(sh, 'load_source', return_value=corridor()), \
                mock.patch.object(sh, 'grid', return_value=lattice), \
                mock.patch.object(sh, 'classify', return_value=(points, np.full(len(points), 'kept'))), \
                mock.patch.object(sh, 'PathLight', return_value=transport):
            data = sh.bake(job, scene)
        origin, step, _, scale, coefficients = sh.decode(data)
        for x, y, z in cells:
            engine = np.array([x, y, z]) * AXIS_SIGNS
            at = np.rint((engine - origin) / step).astype(int)
            self.assertTrue(np.all(at >= 0) and np.all(at < dims))
            index = at[0] + dims[0] * (at[1] + dims[1] * at[2])
            expected = sh.project(sphere_directions(DIRECTIONS),
                                  np.array([x, y, z]) * (1 + sphere_directions(DIRECTIONS)[:, Z_AXIS, None]))
            expected[sh.Z_COEFFICIENT] *= AXIS_SIGNS[Z_AXIS]
            np.testing.assert_allclose(coefficients[index], expected, atol=scale / 2 + 1e-6)

    def test_single_direction(self):
        direction = np.array([[1, 2, 3]]) / math.sqrt(14)
        colour = np.array([[0.2, 0.4, 0.8]])
        expected = np.array([sh.Y00, sh.Y1 * 2 / math.sqrt(14),
                             sh.Y1 * 3 / math.sqrt(14), sh.Y1 / math.sqrt(14)])[:, None]
        np.testing.assert_allclose(sh.project(direction, colour), 4 * math.pi * expected * colour)

    def test_sun_against_traced_disc(self):
        from r3d.ray_query import RayQuery
        from r3d import mitsuba_reference
        mi = mitsuba_reference.import_mitsuba()
        # A finite disc subtends a small solid angle at the origin, normalized to the delta light's power.
        radius, distance, samples = 0.02, 1.0, 4096
        angle = np.arange(samples) * math.pi * (3 - math.sqrt(5))
        radial = radius * np.sqrt((np.arange(samples) + 0.5) / samples)
        targets = np.column_stack((radial * np.cos(angle), np.full(samples, distance), radial * np.sin(angle)))
        directions = targets / np.linalg.norm(targets, axis=1)[:, None]
        query = RayQuery(np.array([[-2, 0.5, -2], [2, 0.5, -2], [0, 0.5, 2]]), np.array([[0, 1, 2]]))
        sun = {"type": "directional", "direction": [0, 1, 0], "color": [1, 0.5, 0.2], "intensity": 2}
        irradiance = mitsuba_reference.emitters(mi, [sun])['light_0']['irradiance']['value']
        for origin in ([0, 0, 0], [4, 0, 0]):
            origins = np.broadcast_to(origin, directions.shape)
            visible = ~query.blocked(origins, directions)
            traced = np.einsum('nk,n,nc->kc', sh.basis(directions), visible,
                               np.broadcast_to(irradiance, directions.shape)) / samples
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
        self.assertEqual(golden[:4], struct.pack('<HH', sh.VERSION, sh.RESERVED_FLAGS))
        _, _, dims, scale, decoded = sh.decode(golden)
        self.assertEqual(tuple(dims), (2, 1, 1))
        self.assertEqual(scale, 0.25)
        np.testing.assert_array_equal(decoded, coefficients * scale)
        for bad in (data[:-1], data + b'\0', bytes([0]) + data[1:]):
            with self.assertRaises(ValueError):
                sh.decode(bad)

    def test_scene_sibling_request_keys_only_sh(self):
        from r3d import sh_probe_settings
        from r3d.build_pack import pack_jobs
        from tests.test_bake import AnyStart

        scene_path = bake.REPO / 'launcher/demo/sponza/sponza.scene.toml'
        packs, jobs = pack_jobs([scene_path])
        tables = bake.read_lock_tables()
        poses = bake.Poses(tables['poses'])
        with tempfile.TemporaryDirectory() as work:
            scene = pathlib.Path(work) / scene_path.name
            request = scene.with_name('sponza' + sh_probe_settings.REQUEST_SUFFIX)
            self.assertIsNone(sh_probe_settings.load(scene))
            request.write_text('directions = 16\nspp = 1\n', encoding='utf-8')
            first = sh_probe_settings.load(scene)
            self.assertEqual(first.directions, 16)
            request.write_text('directions = 32\nspp = 2\n', encoding='utf-8')
            second = sh_probe_settings.load(scene)

        def requested(path, value):
            self.assertEqual(path, scene_path)
            return value

        with mock.patch.object(sh_probe_settings, 'load', side_effect=lambda path: requested(path, first)):
            before = bake.bakes_in(packs, jobs, AnyStart(), poses)
        with mock.patch.object(sh_probe_settings, 'load', side_effect=lambda path: requested(path, second)):
            after = bake.bakes_in(packs, jobs, AnyStart(), poses)
        keys = {item.output: item.key for item in before}
        for item in after:
            with self.subTest(output=item.output):
                if item.kind == 'sh_probes':
                    self.assertTrue(item.packed)
                    self.assertNotEqual(keys[item.output], item.key)
                else:
                    self.assertEqual(keys[item.output], item.key)

    def test_pack_copies_engine_record_unchanged(self):
        from types import SimpleNamespace
        from asset.asset_pack import parse_pack
        from asset import table_asset
        from r3d import build_pack
        from r3d.sh_probe_settings import SUFFIX, TYPE

        with tempfile.TemporaryDirectory() as work:
            root = pathlib.Path(work)
            source = root / 'scene.dat'
            source.write_bytes(b'')
            cached = root / 'cached.shpr'
            record = (pathlib.Path(__file__).parent / 'data/sh_probes.shpr').read_bytes()
            cached.write_bytes(record)
            found = bake.Bake(packed=True, output='scene' + SUFFIX, suffix=SUFFIX,
                              source=source, holder='scene', key='probe',
                              tree=root / ('scene' + SUFFIX), kind='sh_probes',
                              scene=SimpleNamespace(path=source))
            with mock.patch.object(build_pack, 'pack_jobs', return_value=({'scene': {'scene': source}}, {})), \
                    mock.patch.object(bake, 'bakes_in', return_value=[found]), \
                    mock.patch.object(bake, 'fetch_all', return_value={found: cached}), \
                    mock.patch.object(build_pack, 'pack_entry', return_value=('scene', table_asset.TYPE, b'')), \
                    mock.patch.object(build_pack.skin_pack, 'entries', return_value=[]):
                packed = build_pack.pack_bytes([source], offline=True)
            self.assertEqual(parse_pack(packed['scene'])[found.output], (TYPE, record))

    def test_probe_pins_include_source_image_decoder(self):
        owner = bake.entry_file(bake.STAGES['sh_probes'][0]).parent / bake.REQUIREMENTS_NAME
        pin = next(line for line in bake.requirement_lines(owner) if bake.package_name(line) == 'pillow')
        self.assertIn((bake.relative(owner), pin), bake.requirement_pins('sh_probes'))

    def test_existing_tool_closures_exclude_sh(self):
        forbidden = {'sh_probe_settings.py', 'sh_probe_bake.py'}
        for stage in bake.STAGES:
            if stage != 'sh_probes':
                with self.subTest(stage=stage):
                    self.assertFalse(forbidden & {p.name for p in bake.stage_files(stage)})

    def test_header_alignment(self):
        data = sh.encode([1, 2, 3], [4, 5, 6], [1, 1, 1], np.ones((1, sh.COEFFICIENTS, sh.CHANNELS)))
        self.assertEqual(sh.HEADER.size, 40)
        self.assertEqual(struct.unpack_from('<HH', data), (sh.VERSION, 0))
        self.assertEqual(struct.unpack_from('<3f', data, 4), (1, 2, 3))
        self.assertEqual(struct.unpack_from('<3f', data, 16), (4, 5, 6))
        self.assertEqual(struct.unpack_from('<f', data, 28)[0], sh.decode(data)[3])
        self.assertEqual(struct.unpack_from('<3H', data, 32), (1, 1, 1))
        self.assertEqual(data[38:40], bytes(2))
        with self.assertRaises(ValueError):
            sh.decode(data[:2] + struct.pack('<H', 1) + data[4:])

    def test_tool_key_is_local(self):
        from tests.test_bake import AnyStart

        poses = bake.Poses(bake.read_lock_tables()['poses'])
        recipes_before = bake.bakes(starts=AnyStart(), poses=poses)
        before = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        original = bake.code_tokens
        def changed(path, *args, **kwargs):
            tokens = original(path, *args, **kwargs)
            return tokens + [('NAME', 'changed')] if pathlib.Path(path).name == 'sh_probe_bake.py' else tokens
        bake.tool_digest.cache_clear()
        with mock.patch.object(bake, 'code_tokens', changed):
            after = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
            recipes_after = bake.bakes(starts=AnyStart(), poses=poses)
        bake.tool_digest.cache_clear()
        self.assertEqual({item.output: item.key for item in recipes_before if item.kind != 'sh_probes'},
                         {item.output: item.key for item in recipes_after if item.kind != 'sh_probes'})
        self.assertNotEqual([item.key for item in recipes_before if item.kind == 'sh_probes'],
                            [item.key for item in recipes_after if item.kind == 'sh_probes'])
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
