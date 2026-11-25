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
from r3d import sh_probe_asset as asset
from r3d.import_settings import LIGHT_MODE_BAKED
from r3d.sh_probe_settings import DIRECTIONS, SPP
from r3d.views.probes import sphere_directions
from bake import bake
from asset.engine_frame import AXIS_SIGNS, Z_AXIS
from tests.r3d_env import needs_llvm  # noqa: E402


def golden_record():
    coefficients = np.arange(-12, 12, dtype='<i2')
    return (asset.HEADER.pack(asset.VERSION, asset.SH_PROBES_FLAG_INSIDE_MASK, 1, 2, 3, 4, 5, 6, 0.25, 2, 1, 1)
            + coefficients.tobytes() + np.packbits([True, False], bitorder='little').tobytes())


class ProbeTests(unittest.TestCase):
    def test_constant_sphere(self):
        directions = sphere_directions(DIRECTIONS)
        colour = np.array([0.2, 0.6, 1.0])
        got = sh.project(directions, np.broadcast_to(colour, (len(directions), 3)))
        np.testing.assert_allclose(got[0], colour * math.sqrt(4 * math.pi))
        np.testing.assert_allclose(got[1:], 0, atol=4 * math.pi * sh.Y1 / len(directions))

    @needs_llvm
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
        scene = SimpleNamespace(path=None, lights=[dict(type='sky', mode=LIGHT_MODE_BAKED, color=colour.tolist(), intensity=1.0)],
                                camera=SimpleNamespace(component=SimpleNamespace(near_z=0.001)),
                                indirect=SimpleNamespace(albedo_boost=1.0))
        lattice = (points, 1.0, points[0], np.ones(3), np.ones(3, dtype=int))
        with mock.patch.object(sh, 'load', return_value=requested), \
                mock.patch.object(sh, 'load_source', return_value=source), mock.patch.object(sh, 'grid', return_value=lattice):
            data = sh.bake(job, scene)
        directions = sphere_directions(DIRECTIONS)
        visible = (directions[:, 0] > 0) & (directions[:, 1] > 0)
        expected = sh.project(directions, visible[:, None] * colour)
        got = asset.decode(data)[4][0]
        np.testing.assert_allclose(got, expected, atol=0.02)

    @needs_llvm
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
            from asset.engine_frame import to_engine
            from r3d.sh_probe_asset import TYPE as SH_PROBES
            data = to_engine(SH_PROBES, sh.bake(job, scene))
        origin, step, _, scale, coefficients, _ = asset.decode(data)
        for x, y, z in cells:
            engine = np.array([x, y, z]) * AXIS_SIGNS
            at = np.rint((engine - origin) / step).astype(int)
            self.assertTrue(np.all(at >= 0) and np.all(at < dims))
            index = at[0] + dims[0] * (at[1] + dims[1] * at[2])
            expected = sh.project(sphere_directions(DIRECTIONS),
                                  np.array([x, y, z]) * (1 + sphere_directions(DIRECTIONS)[:, Z_AXIS, None]))
            expected[sh.L1_AXES.index(Z_AXIS) + 1] *= AXIS_SIGNS[Z_AXIS]
            np.testing.assert_allclose(coefficients[index], expected, atol=scale / 2 + 1e-6)

    @needs_llvm
    def test_lit_corridor_pack_aligns_probes_and_mesh(self):
        from types import SimpleNamespace
        from asset.asset_pack import build_pack, parse_pack
        from asset.engine_frame import to_engine
        from r3d.sh_probe_asset import TYPE
        from r3d import mesh_asset
        from tests.soup import corridor, sun
        from tests.test_engine_frame import mesh_entry, POSITION

        source = corridor(albedo=0)
        settings = SimpleNamespace(path=None, seed=0, alpha_keep=None, double_sided=set())
        requested = SimpleNamespace(spacing=0.5, directions=DIRECTIONS, spp=1)
        job = SimpleNamespace(settings=settings, bake=SimpleNamespace(indirect=SimpleNamespace(bounces=1, rays=1)))
        camera = SimpleNamespace(near_z=0.001, region=([0.25, 0.25, 1.0], [1.25, 1.25, 2.0]))
        scene = SimpleNamespace(path=None, lights=[sun([0, 0, 1])], camera=SimpleNamespace(component=camera),
                                indirect=SimpleNamespace(albedo_boost=1.0))
        with mock.patch.object(sh, 'load', return_value=requested), \
                mock.patch.object(sh, 'load_source', return_value=source):
            raw = sh.bake(job, scene)
        source_origin, spacing, dims, _, source_coefficients, _ = asset.decode(raw)
        self.assertTrue(np.all(source_coefficients[:, sh.L1_AXES.index(Z_AXIS) + 1] > 0))
        point = tuple(source_origin)
        position_scale = 4
        mesh = mesh_entry(tuple(int(v * position_scale) for v in point), position_scale=position_scale)
        pack = build_pack([('corridor-grid', TYPE, to_engine(TYPE, raw)),
                           ('corridor', mesh_asset.TYPE, to_engine(mesh_asset.TYPE, mesh))])
        entries = parse_pack(pack)
        origin, step, _, scale, coefficients, _ = asset.decode(entries['corridor-grid'][1])
        mirrored = np.asarray(point) * AXIS_SIGNS
        cell = np.rint((mirrored - origin) / step).astype(int)
        index = cell[0] + dims[0] * (cell[1] + dims[1] * cell[2])
        self.assertTrue(np.all(coefficients[index, sh.L1_AXES.index(Z_AXIS) + 1] < 0))
        expected = source_coefficients[0].copy()
        expected[sh.L1_AXES.index(Z_AXIS) + 1] *= -1
        np.testing.assert_allclose(coefficients[index], expected, atol=scale)
        position_at = mesh_asset.BLOB_HEADER.unpack_from(entries['corridor'][1])[5]
        packed_scale = mesh_asset.BLOB_HEADER.unpack_from(entries['corridor'][1])[4]
        np.testing.assert_allclose(np.array(POSITION.unpack_from(entries['corridor'][1], position_at)) / packed_scale,
                                   mirrored)

    def test_single_direction(self):
        direction = np.array([[1, 2, 3]]) / math.sqrt(14)
        colour = np.array([[0.2, 0.4, 0.8]])
        expected = np.array([sh.Y00, sh.Y1 * 2 / math.sqrt(14),
                             sh.Y1 * 3 / math.sqrt(14), sh.Y1 / math.sqrt(14)])[:, None]
        np.testing.assert_allclose(sh.project(direction, colour), 4 * math.pi * expected * colour)

    @needs_llvm
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
        from tests.soup import sun as test_sun
        sun = test_sun([0, 1, 0], [1, 0.5, 0.2], 2)
        irradiance = mitsuba_reference.emitters(mi, [sun])['light_0']['irradiance']['value']
        for origin in ([0, 0, 0], [4, 0, 0]):
            origins = np.broadcast_to(origin, directions.shape)
            visible = ~query.blocked(origins, directions)
            traced = np.einsum('nk,n,nc->kc', sh.basis(directions), visible,
                               np.broadcast_to(irradiance, directions.shape)) / samples
            got = sh.sun_term(query, np.array([origin]), [sun])[0]
            np.testing.assert_allclose(got, traced, atol=4e-4)

    def test_sun_uses_only_baked_lights(self):
        from r3d.import_settings import load_light

        query = mock.Mock()
        query.blocked.side_effect = lambda points, directions: np.zeros(len(points), dtype=bool)
        points = np.array([[0., 0., 0.], [1., 0., 0.]])
        component = dict(type='directional', color=[0.2, 0.4, 0.8], intensity=2)
        realtime = load_light(component, [0, 0, 0], 'light')
        baked = load_light(dict(component, mode=LIGHT_MODE_BAKED), [0, 0, 0], 'light')
        expected = np.broadcast_to(math.pi * sh.basis(np.array([[0., 1., 0.]]))[:, :, None]
                                   * np.array(component['color']) * component['intensity'],
                                   (len(points), sh.COEFFICIENTS, sh.CHANNELS)).copy()
        self.assertEqual(sh.sun_term(query, points, [realtime]).tobytes(), np.zeros_like(expected).tobytes())
        self.assertEqual(sh.sun_term(query, points, [baked]).tobytes(), expected.tobytes())
        self.assertEqual(sh.sun_term(query, points, [realtime, baked]).tobytes(), expected.tobytes())
        invalid = baked.copy()
        del invalid['mode']
        with self.assertRaises(KeyError):
            sh.sun_term(query, points, [invalid])
        with self.assertRaises(ValueError):
            load_light(dict(component, mode='unknown'), [0, 0, 0], 'light')

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
        data = asset.encode([1, 2, 3], [4, 5, 6], [2, 1, 1], coefficients)
        origin, spacing, dims, scale, decoded, inside = asset.decode(data)
        np.testing.assert_array_equal(origin, [1, 2, 3])
        np.testing.assert_array_equal(spacing, [4, 5, 6])
        self.assertEqual(tuple(dims), (2, 1, 1))
        np.testing.assert_allclose(decoded, coefficients, atol=scale / 2 + 1e-6)
        self.assertEqual(len(data), asset.HEADER.size + coefficients.size * np.dtype('<i2').itemsize + 1)
        np.testing.assert_array_equal(inside, [False, False])
        fixture = pathlib.Path(__file__).parent / 'data/sh_probes.shpr'
        golden = fixture.read_bytes()
        self.assertEqual(golden, golden_record())
        self.assertEqual(golden[:4], struct.pack('<HH', asset.VERSION, asset.SH_PROBES_FLAG_INSIDE_MASK))
        _, _, dims, scale, decoded, inside = asset.decode(golden)
        self.assertEqual(tuple(dims), (2, 1, 1))
        self.assertEqual(scale, 0.25)
        np.testing.assert_array_equal(inside, [True, False])
        np.testing.assert_array_equal(decoded, coefficients * scale)
        for bad in (data[:-1], data + b'\0', bytes([0]) + data[1:]):
            with self.assertRaises(ValueError):
                asset.decode(bad)

    def test_inside_mask_round_trip_and_padding(self):
        inside = np.array([True, False, True, False, False, True, False, True, True, False])
        coefficients = np.arange(len(inside) * sh.COEFFICIENTS * sh.CHANNELS).reshape(-1, sh.COEFFICIENTS, sh.CHANNELS)
        data = asset.encode([0, 0, 0], [1, 1, 1], [5, 2, 1], coefficients, inside)
        np.testing.assert_array_equal(asset.decode(data)[5], inside)
        self.assertEqual(data[-2:], bytes([0b10100101, 0b00000001]))
        self.assertEqual(asset.HEADER.unpack_from(data)[1], asset.SH_PROBES_FLAG_INSIDE_MASK)
        for bad in (data[:-1], data + b'\0'):
            with self.assertRaises(ValueError):
                asset.decode(bad)
        with self.assertRaises(ValueError):
            asset.decode(data[:2] + struct.pack('<H', asset.SH_PROBES_FLAG_INSIDE_MASK << 1) + data[4:])
        legacy = data[:2] + bytes(2) + data[4:-2]
        np.testing.assert_array_equal(asset.decode(legacy)[5], np.zeros(len(inside), dtype=bool))
        np.testing.assert_array_equal(asset.decode(legacy)[4], asset.decode(data)[4])

    def test_engine_mirror_moves_mask_with_probes(self):
        from asset import engine_frame

        dims = (2, 3, 4)
        count = math.prod(dims)
        coefficients = np.arange(count * sh.COEFFICIENTS * sh.CHANNELS).reshape(count, sh.COEFFICIENTS, sh.CHANNELS)
        inside = np.arange(count) % 5 == 0
        data = asset.encode([1, 2, 3], [4, 5, 6], dims, coefficients, inside)
        before = asset.decode(data)
        for signs in ((1, 1, -1), (-1, 1, 1), (1, -1, 1), (-1, -1, -1)):
            with self.subTest(signs=signs), mock.patch.object(engine_frame, 'AXIS_SIGNS', signs):
                mirrored = engine_frame.to_engine(asset.TYPE, data)
                origin, spacing, got_dims, scale, got, mask = asset.decode(mirrored)
                slices = tuple(slice(None, None, sign) for sign in reversed(signs))
                expected = before[4].reshape(*reversed(dims), sh.COEFFICIENTS, sh.CHANNELS)[slices].copy()
                expected *= np.array([1, signs[1], signs[2], signs[0]])[:, None]
                np.testing.assert_array_equal(got, expected.reshape(got.shape))
                np.testing.assert_array_equal(mask, inside.reshape(tuple(reversed(dims)))[slices].ravel())
                np.testing.assert_array_equal(origin, [v if sign > 0 else -(v + (n - 1) * step)
                                                       for v, n, step, sign in zip(before[0], dims, before[1], signs)])
                self.assertEqual(scale, before[3])
                self.assertEqual(tuple(got_dims), dims)
                np.testing.assert_array_equal(spacing, before[1])
                self.assertEqual(engine_frame.to_engine(asset.TYPE, mirrored), data)

    @needs_llvm
    def test_closed_box_inside_and_coefficient_bytes(self):
        from types import SimpleNamespace
        from tests.soup import box
        import hashlib

        solid = box([4, 4, 4])
        source = SimpleNamespace(p=solid.vertices, tri_v=solid.faces)
        dims = np.array([2, 2, 3])
        points = np.stack(np.meshgrid([-5., 0.], [-5., 0.], [-5., 0., 5.], indexing='ij'), axis=-1).reshape(-1, 3)
        settings = SimpleNamespace(seed=0, alpha_keep=None, double_sided=set())
        requested = SimpleNamespace(spacing=None, directions=DIRECTIONS, spp=1)
        job = SimpleNamespace(settings=settings, bake=SimpleNamespace(indirect=SimpleNamespace(bounces=2)))
        scene = SimpleNamespace(path=None, lights=[], camera=SimpleNamespace(component=SimpleNamespace(near_z=0.001)),
                                indirect=SimpleNamespace(albedo_boost=1.0))
        transport = mock.Mock()
        transport._radiance.side_effect = lambda origins, rays, seed: np.abs(origins)
        lattice = (points, 1.0, points[0], np.ones(3), dims)
        with mock.patch.object(sh, 'load', return_value=requested), \
                mock.patch.object(sh, 'load_source', return_value=source), \
                mock.patch.object(sh, 'grid', return_value=lattice), \
                mock.patch.object(sh, 'PathLight', return_value=transport):
            data = sh.bake(job, scene)
        payload = data[asset.HEADER.size:asset.HEADER.size + len(points) * sh.COEFFICIENTS * sh.CHANNELS * np.dtype('<i2').itemsize]
        self.assertEqual(hashlib.sha256(payload).hexdigest(), 'ef0d5b63871fea7307f47677709ed5bc1100022055a8eaaa78ac2f6e52f78d71')
        expected = np.all(points == 0, axis=1).reshape(tuple(dims)).transpose(2, 1, 0).ravel()
        np.testing.assert_array_equal(asset.decode(data)[5], expected)

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
                    self.assertNotEqual(keys[item.output], item.key)
                else:
                    self.assertEqual(keys[item.output], item.key)

    def test_probe_pins_include_source_image_decoder(self):
        owner = bake.entry_file(bake.STAGES['sh_probes'][0]).parent / bake.REQUIREMENTS_NAME
        pin = next(line for line in bake.requirement_lines(owner) if bake.package_name(line) == 'pillow')
        self.assertIn((bake.relative(owner), pin), bake.requirement_pins('sh_probes'))

    def test_stage_units_exclude_probe_baker(self):
        for stage, entries in bake.STAGES.items():
            if not all(":" in entry for entry in entries):
                continue
            with self.subTest(stage=stage):
                paths = {unit.path.name for unit in bake.stage_units(stage)[0]}
                self.assertEqual('sh_probe_bake.py' in paths, stage == 'sh_probes')

    def test_environment_modes_are_explicit(self):
        from r3d.import_settings import load_environment_light
        from r3d.light import baked_lights

        for kind in ('sky', 'ambient'):
            table = dict(color=[1, 1, 1], intensity=1)
            if kind == 'sky':
                table['rays'] = DIRECTIONS
            light = load_environment_light(kind, table, kind)
            self.assertEqual(light['mode'], LIGHT_MODE_BAKED)
            self.assertEqual(baked_lights([light]), [light])

    def test_existing_tool_closures_exclude_sh(self):
        forbidden = {'sh_probe_settings.py', 'sh_probe_bake.py'}
        for stage in bake.STAGES:
            if stage != 'sh_probes':
                with self.subTest(stage=stage):
                    self.assertFalse(forbidden & {p.name for p in bake.stage_files(stage)})

    def test_header_alignment(self):
        data = asset.encode([1, 2, 3], [4, 5, 6], [1, 1, 1], np.ones((1, sh.COEFFICIENTS, sh.CHANNELS)))
        self.assertEqual(asset.HEADER.size, 40)
        self.assertEqual(struct.unpack_from('<HH', data), (asset.VERSION, asset.SH_PROBES_FLAG_INSIDE_MASK))
        self.assertEqual(struct.unpack_from('<3f', data, 4), (1, 2, 3))
        self.assertEqual(struct.unpack_from('<3f', data, 16), (4, 5, 6))
        self.assertEqual(struct.unpack_from('<f', data, 28)[0], asset.decode(data)[3])
        self.assertEqual(struct.unpack_from('<3H', data, 32), (1, 1, 1))
        self.assertEqual(data[38:40], bytes(2))
        with self.assertRaises(ValueError):
            asset.decode(data[:2] + struct.pack('<H', asset.SH_PROBES_FLAG_INSIDE_MASK << 1) + data[4:])

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
    if sys.argv[1:] == ['--write-golden']:
        (pathlib.Path(__file__).parent / 'data/sh_probes.shpr').write_bytes(golden_record())
    else:
        unittest.main()
