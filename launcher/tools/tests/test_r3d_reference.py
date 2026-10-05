"""Checks the source-reference renderer on a one-triangle lit mesh: its light, its pixels and its normal buffer."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

try:
    import numpy as np
    from tests import soup

    from r3d.geometry import corner_normals
    from r3d.reference_render import device_picture, render_linear, trace
    from render_compare import expand_565
except ImportError:
    np = None


def plane_source(corners):
    """A one-material quad of two triangles facing +z, white, unlit by anything but the scene."""
    source = SimpleNamespace(
        p=np.array(corners), uv=np.zeros((4, 2)), tri_v=np.array([[0, 1, 2], [0, 2, 3]]), tri_t=np.array([[0, 1, 2], [0, 2, 3]]),
        tri_m=np.array([0, 0]), names=["plane"], materials={"plane": {"Kd": (1.0, 1.0, 1.0)}}, textures=[None],
    )
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.bounce = None
    source.intersector = soup.rays(soup.Soup(source.p, source.tri_v))
    return source


def sun_scene(direction):
    settings = SimpleNamespace(double_sided=set(), seed=1)
    job = SimpleNamespace(settings=settings, bake=SimpleNamespace(ray_offset=0.01, ao=None))
    scene = SimpleNamespace(lights=[{"type": "directional", "direction": direction, "color": [1.0, 1.0, 1.0], "intensity": 1.0}])
    return job, scene


LOOK_DOWN = None if np is None else np.array([0.0, 0.0, 1.0, 0.0, 0.0, -1.0])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ReferenceRenderTest(unittest.TestCase):
    def test_main_reports_occlusion_value_error_on_serial_path(self):
        import contextlib
        import io
        import tempfile
        from unittest.mock import patch
        from r3d import reference_render

        source = plane_source([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        job, scene = sun_scene([0., 0., 1.])
        job.bake.ao = SimpleNamespace(distance=1., strength=1., rays=1, indirect=False)
        scene.tonemap_white = 2.
        scene.camera = SimpleNamespace(component=SimpleNamespace(background=0))
        message = "invalid local occlusion setting"
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()) as stderr:
            with patch.object(reference_render, 'load_scene', return_value=scene), \
                    patch.object(reference_render, 'read_poses', return_value=(1, 1, .1, .01, [LOOK_DOWN])), \
                    patch.object(reference_render, 'source_for', return_value=(source, job)), \
                    patch.object(reference_render, 'occlusion_map', side_effect=ValueError(message)) as occlusion:
                with self.assertRaises(SystemExit) as error:
                    reference_render.main(['scene', '--poses', 'poses', '--out', directory,
                                           '--samples', '1', '--workers', '1', '--occlusion'])
        self.assertEqual(error.exception.code, 2)
        self.assertIn(message, stderr.getvalue())
        occlusion.assert_called_once()

    def test_an_unshadowed_plane_is_exact_lambert(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[1.0, 1.0, 1.0]]], atol=1e-12, rtol=0)

    def test_a_tilted_sun_scales_the_value_by_the_cosine_to_the_normal(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.8, 0.0, 0.6]), LOOK_DOWN, 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[0.6, 0.6, 0.6]]], atol=1e-12, rtol=0)

    def test_subpixels_land_in_their_own_pixel_and_average_to_its_coverage(self):
        # The quad covers -0.5 < x < 0 and y > 0.5 of a view spanning -1..1: of the 2 x 2 pixels only the
        # top-left is reached, by its upper-right subpixel, one of its four.
        source = plane_source([[-0.5, 0.5, 0.0], [0.0, 0.5, 0.0], [0.0, 2.0, 0.0], [-0.5, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 2, 2, 1.0, 2)
        np.testing.assert_allclose(picture[..., 0], [[0.25, 0.0], [0.0, 0.0]], atol=1e-12, rtol=0)

    def test_the_normal_buffer_faces_the_eye_and_is_zero_where_rays_miss(self):
        # Wound to face away from the eye at z = 1: the buffer still turns its normal toward the eye.
        source = plane_source([[-0.5, 0.5, 0.0], [-0.5, 2.0, 0.0], [0.0, 2.0, 0.0], [0.0, 0.5, 0.0]])
        _linear, _covered, normal = trace(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 2, 2, 1.0, 2)
        np.testing.assert_allclose(normal[0, 0], [0.0, 0.0, 1.0], atol=1e-12, rtol=0)
        np.testing.assert_allclose(normal[1], 0.0, atol=1e-12, rtol=0)

    def test_a_pose_at_empty_sky_is_the_scene_background_as_the_device_shows_it(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        away = np.array([0.0, 0.0, 1.0, 0.0, 0.0, 1.0])
        linear, covered, _normal = trace(source, *sun_scene([0.0, 0.0, 1.0]), away, 3, 3, 1.0, 2)
        picture = device_picture(linear, covered, 0.35, 0x9CC0E6)
        shown = expand_565(np.full((3, 3, 3), (0x9C, 0xC0, 0xE6), dtype=np.uint8))
        np.testing.assert_array_equal(picture, shown)

    def test_a_half_covered_pixel_blends_the_mesh_with_the_background(self):
        linear, covered = np.full((1, 1, 3), 1.0), np.full((1, 1), 0.5)
        picture = device_picture(linear, covered, 0.0, 0x000000)
        np.testing.assert_array_equal(picture, expand_565(np.full((1, 1, 3), 128, dtype=np.uint8)))


@unittest.skipIf(np is None, "needs NumPy")
class PooledReferenceTests(unittest.TestCase):
    def test_pool_matches_serial_files(self):
        import multiprocessing
        import tempfile
        from r3d.reference_render import render_poses
        from r3d import reference_render
        from unittest.mock import patch
        if "fork" not in multiprocessing.get_all_start_methods():
            self.skipTest("copy-on-write pose pool needs fork")
        source = plane_source([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        wall = soup.box(extents=(.1, 4., 2.))
        wall.apply_translation((1.2, 0., 1.))
        mesh = soup.concatenate([soup.Soup(source.p, source.tri_v), wall])
        source.p, source.tri_v = mesh.vertices, mesh.faces
        source.tri_t = np.zeros_like(source.tri_v)
        source.tri_m = np.zeros(len(source.tri_v), dtype=int)
        source.corner_normals = corner_normals(source.p, source.tri_v)
        source.intersector = soup.rays(mesh)
        job, scene = sun_scene([0., 0., -1.])
        job.bake.ao = SimpleNamespace(distance=3., strength=.8, rays=8, indirect=True)
        scene.lights[0]["disc_degrees"] = 4.
        scene.lights[0]["rays"] = 3
        scene.lights.append({"type": "ambient", "color": [1., 1., 1.], "intensity": .5})
        scene.tonemap_white = 2.
        scene.camera = SimpleNamespace(component=SimpleNamespace(background=0x123456))
        poses = [LOOK_DOWN + np.array([i * .1, 0., 0., 0., 0., 0.]) for i in range(6)]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            serial, pooled = root / "serial", root / "pooled"
            serial.mkdir(); pooled.mkdir()
            render_poses(source, job, scene, poses, 8, 8, 1., 2, serial, True, 1, True)
            from PIL import Image
            from r3d.reference_render import occlusion_map
            for index, pose in enumerate(poses):
                linear, covered, normal = trace(source, job, scene, pose, 8, 8, 1., 2)
                factor, share = occlusion_map(source, job, pose, 8, 8, 1., 2)
                self.assertLess(factor.min(), 1.)
                np.testing.assert_array_equal(np.load(serial / f'{index:04d}.linear.npy'), linear)
                np.testing.assert_array_equal(np.load(serial / f'{index:04d}.normal.npy'), normal.astype(np.float32))
                np.testing.assert_array_equal(np.load(serial / f'{index:04d}.occlusion.npy'), factor.astype(np.float32))
                background = scene.camera.component.background
                shade = np.round(255 * factor)[..., None] * np.ones(3)
                colour = np.array([background >> 16, (background >> 8) & 255, background & 255], dtype=float)
                shown = shade * share[..., None] + colour * (1.0 - share[..., None])
                np.testing.assert_array_equal(np.asarray(Image.open(serial / f'{index:04d}.occlusion.png')),
                                              np.round(shown).astype(np.uint8))
                np.testing.assert_array_equal(np.asarray(Image.open(serial / f'{index:04d}.png')),
                                              device_picture(linear, covered, scene.tonemap_white, background))
                job.bake.ao, ao = None, job.bake.ao
                unoccluded = trace(source, job, scene, pose, 8, 8, 1., 2)[0]
                job.bake.ao = ao
                self.assertFalse(np.array_equal(linear, unoccluded))
            global POSE_BARRIER
            POSE_BARRIER = multiprocessing.get_context('fork').Barrier(2)
            with patch.object(reference_render, '_write_pose', record_pose_pid):
                render_poses(source, job, scene, poses, 8, 8, 1., 2, pooled, True, 2, True)
            pids = list(pooled.glob('pid-*'))
            self.assertGreater(len(pids), 1)
            for marker in pids:
                marker.unlink()
            self.assertEqual(sorted(p.name for p in serial.iterdir()), sorted(p.name for p in pooled.iterdir()))
            for path in serial.iterdir():
                self.assertEqual(path.read_bytes(), (pooled / path.name).read_bytes(), path.name)


@unittest.skipIf(np is None, "needs NumPy")
class SkyStreamingTests(unittest.TestCase):
    def test_sky_directions_are_streamed_and_match_list(self):
        from unittest.mock import patch
        from r3d import light as lighting
        source = plane_source([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        original = lighting.unshadowed_count
        sky = {"type": "sky", "rays": 7, "color": [.5, .7, .9], "intensity": .8}
        points = np.array([[0., 0., .5], [.2, .3, .1]])
        normals = np.array([[0., 0., -1.], [0., 0., 1.]])

        def render():
            return lighting.light(points, normals, np.array([False, False]), source.intersector, [sky], .01)

        def streamed(intersector, origin, directions):
            self.assertFalse(isinstance(directions, list))
            return original(intersector, origin, directions)

        with patch.object(lighting, 'unshadowed_count', side_effect=streamed):
            actual = render()

        def list_bake_sky(light, ctx):
            n = ctx.normals
            tu, tv = lighting.tangent_frame(n)
            directions = [tu * x + tv * y + n * z for x, y, z in lighting.sky_directions(light['rays'])]
            visible = original(ctx.intersector, ctx.origin, directions) / light['rays']
            return visible[:, None] * np.array(light['color']) * light['intensity']

        with patch.dict(lighting.LIGHTS, sky=(lighting.LIGHTS['sky'][0], list_bake_sky)):
            expected = render()
        self.assertEqual(actual.tobytes(), expected.tobytes())


POSE_BARRIER = None
if np is not None:
    from r3d.reference_render import _write_pose as original_write_pose


def record_pose_pid(item):
    from r3d.reference_render import POSE_STATE
    POSE_BARRIER.wait(timeout=10)
    pid, peak = original_write_pose(item)
    (POSE_STATE[7] / f'pid-{pid}').touch()
    return pid, peak




@unittest.skipIf(np is None, "needs NumPy")
class PoseMetricTests(unittest.TestCase):
    def test_pool_budget_and_pss_aggregation(self):
        from unittest.mock import Mock, patch
        from r3d import process_budget as budget, reference_render as reference
        pool = Mock()
        pool.__enter__ = Mock(return_value=pool)
        pool.__exit__ = Mock(return_value=False)
        pool.map.return_value = [(101, 30), (102, 40), (101, 20), (102, 50)]
        with patch.object(budget, 'task_reservation', return_value=budget.PREPARE_BYTES), \
                patch.object(budget, 'resident_bytes', return_value=(3290000000, 0, 3290000000)), \
                patch.object(budget, 'cores_available', return_value=10), \
                patch.object(reference.multiprocessing, 'get_all_start_methods', return_value=['fork']), \
                patch.object(reference.multiprocessing, 'get_context'), \
                patch.object(reference.concurrent.futures, 'ProcessPoolExecutor', return_value=pool) as executor:
            result = reference.render_poses(None, None, None, [None] * 4,
                                            368, 448, 1., 4, '.')
        self.assertEqual(result, (80, 2))
        self.assertEqual(executor.call_args.kwargs['max_workers'], 2)
        self.assertIsNone(reference.POSE_STATE)

    def test_serial_pss_and_smoke_budget(self):
        from unittest.mock import patch
        from r3d import process_budget as budget, reference_render as reference
        with patch.object(budget, 'task_reservation', return_value=budget.SMOKE_PREPARE_BYTES), \
                patch.object(budget, 'resident_bytes', return_value=(budget.GIB, 0, budget.GIB)), \
                patch.object(budget, 'cores_available', return_value=10), \
                patch.object(reference, '_write_pose', side_effect=[(1, 20), (1, 30), (1, 10)]):
            self.assertEqual(reference.render_poses(None, None, None, [None] * 3,
                                                   368, 448, 1., 4, '.'), (30, 1))
        self.assertIsNone(reference.POSE_STATE)


if __name__ == "__main__":
    unittest.main()
