"""Check probe-grid reconstruction, sun occlusion and asset pack replacement."""
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import irradiance_figures as figures
from asset.asset_pack import build_pack, parse_pack
from asset.engine_frame import to_engine
from r3d import sh_probe_asset as asset, sh_probe_bake as sh
from r3d.import_settings import LIGHT_MODE_BAKED
from r3d.sh_probe_settings import DIRECTIONS
from tests.soup import box
from tests.r3d_env import needs_llvm


class IrradianceSplitTests(unittest.TestCase):
    @needs_llvm
    def test_baked_grid_reconstructs_and_occluded_probes_have_no_sun(self):
        solid = box([4, 4, 4])
        source = SimpleNamespace(p=solid.vertices, tri_v=solid.faces)
        points = np.array([[-5., 0., -5.], [0., 0., -5.], [5., 0., -5.],
                           [-5., 0., 0.], [0., 0., 0.], [5., 0., 0.]])
        dims = (2, 1, 3)
        lattice = (points, 1.0, points[0], np.ones(3), dims)
        settings = SimpleNamespace(seed=0, alpha_keep=None, double_sided=set())
        job = SimpleNamespace(settings=settings, bake=SimpleNamespace(indirect=SimpleNamespace(bounces=2),
                                                               ray_offset=0.001))
        sun = dict(type='directional', mode=LIGHT_MODE_BAKED, direction=(0, 0, 1), color=(1, 0.9, 0.8), intensity=1)
        scene = SimpleNamespace(path=None, lights=[sun],
                                camera=SimpleNamespace(component=SimpleNamespace(near_z=0.001)),
                                indirect=SimpleNamespace(albedo_boost=1.0))
        requested = SimpleNamespace(spacing=None, directions=DIRECTIONS, spp=1)
        transport = mock.Mock()
        transport._radiance.side_effect = lambda origins, rays, seed: np.abs(origins) / 20
        with mock.patch.object(sh, 'load', return_value=requested), \
                mock.patch.object(sh, 'load_source', return_value=source), \
                mock.patch.object(sh, 'grid', return_value=lattice), \
                mock.patch.object(sh, 'PathLight', return_value=transport) as path_light:
            total = asset.decode(to_engine(asset.TYPE, sh.bake(job, scene)))
            path_light.reset_mock()
            sun_data, bounce_data = figures.split_probes(job, scene, total)
            sun, bounce = map(asset.decode, (sun_data, bounce_data))
            path_light.assert_not_called()
        np.testing.assert_allclose(sun[4] + bounce[4], total[4], atol=total[3], rtol=0)
        np.testing.assert_array_equal(sun[5], total[5])
        np.testing.assert_array_equal(bounce[5], total[5])
        source_sun = asset.decode(to_engine(asset.TYPE, sun_data))[4]
        np.testing.assert_array_equal(source_sun[2], 0)
        self.assertGreater(source_sun[0, 0, 0], 0)

    def test_asset_copies_keep_other_pack_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            assets = work / 'assets'
            assets.mkdir()
            data = asset.encode((0, 0, 0), (1, 1, 1), (1, 1, 1), np.zeros((1, sh.COEFFICIENTS, sh.CHANNELS)))
            entry = 'probe' + asset.SUFFIX
            packed = build_pack([(entry, asset.TYPE, data), ('mesh', b'LMSH', b'mesh bytes')])
            path = assets / ('fixture' + figures.PACK_SUFFIX)
            path.write_bytes(packed)
            scene_path = work / 'fixture.scene.toml'
            probe = SimpleNamespace(kind='sh_probes', output=entry, job=SimpleNamespace(settings=None),
                                    scene=SimpleNamespace(tonemap_white=0.35))
            with mock.patch.object(figures, 'pack_jobs', return_value=({'fixture': {'scene': scene_path}}, {})), \
                    mock.patch.object(figures.bake, 'bakes_in', return_value=[probe]), \
                    mock.patch.object(figures.bake, 'is_blend', return_value=False), \
                    mock.patch.object(figures, 'split_probes', return_value=(b'sun', b'bounce')):
                copies, white = figures.split_assets(assets, scene_path, work)
            self.assertEqual(white, probe.scene.tonemap_white)
            self.assertEqual(path.read_bytes(), packed)
            for term, destination in copies.items():
                rows = parse_pack((destination / path.name).read_bytes())
                self.assertEqual(rows, {entry: (asset.TYPE, term.encode()), 'mesh': (b'LMSH', b'mesh bytes')})


if __name__ == '__main__':
    unittest.main()
