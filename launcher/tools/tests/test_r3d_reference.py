"""Checks the source-reference renderer on a one-triangle lit mesh: its light, its pixels and its normal buffer."""

import contextlib
import io
import pathlib
import sys
import tempfile
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

from tests.r3d_env import needs_mitsuba  # noqa: E402
from tests.test_r3d_path_bake import needs_llvm  # noqa: E402


def plane_source(corners):
    """A one-material quad of two triangles facing +z, white, unlit by anything but the scene."""
    source = SimpleNamespace(
        p=np.array(corners), uv=np.zeros((4, 2)), tri_v=np.array([[0, 1, 2], [0, 2, 3]]), tri_t=np.array([[0, 1, 2], [0, 2, 3]]),
        tri_m=np.array([0, 0]), names=["plane"], materials={"plane": {"Kd": (1.0, 1.0, 1.0)}}, textures=[None], colors=None,
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
    @needs_mitsuba
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

    @needs_mitsuba
    def test_an_unshadowed_plane_is_exact_lambert(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[1.0, 1.0, 1.0]]], atol=1e-12, rtol=0)

    @needs_mitsuba
    def test_a_tilted_sun_scales_the_value_by_the_cosine_to_the_normal(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.8, 0.0, 0.6]), LOOK_DOWN, 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[0.6, 0.6, 0.6]]], atol=1e-12, rtol=0)

    @needs_mitsuba
    def test_subpixels_land_in_their_own_pixel_and_average_to_its_coverage(self):
        # The quad covers -0.5 < x < 0 and y > 0.5 of a view spanning -1..1: of the 2 x 2 pixels only the
        # top-left is reached, by its upper-right subpixel, one of its four.
        source = plane_source([[-0.5, 0.5, 0.0], [0.0, 0.5, 0.0], [0.0, 2.0, 0.0], [-0.5, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 2, 2, 1.0, 2)
        np.testing.assert_allclose(picture[..., 0], [[0.25, 0.0], [0.0, 0.0]], atol=1e-12, rtol=0)

    @needs_mitsuba
    def test_the_normal_buffer_faces_the_eye_and_is_zero_where_rays_miss(self):
        # Wound to face away from the eye at z = 1: the buffer still turns its normal toward the eye.
        source = plane_source([[-0.5, 0.5, 0.0], [-0.5, 2.0, 0.0], [0.0, 2.0, 0.0], [0.0, 0.5, 0.0]])
        _linear, _covered, normal = trace(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 2, 2, 1.0, 2)
        np.testing.assert_allclose(normal[0, 0], [0.0, 0.0, 1.0], atol=1e-12, rtol=0)
        np.testing.assert_allclose(normal[1], 0.0, atol=1e-12, rtol=0)

    @needs_mitsuba
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
            self.skipTest("pose pool needs fork")
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
class ReferenceSetTests(unittest.TestCase):
    def test_render_sets_writes_the_files_main_writes_for_each_pose_file_with_normals(self):
        import contextlib
        import io
        import tempfile
        from unittest.mock import patch
        from r3d import process_budget as budget, reference_render
        from r3d.fitted_variant import ReferenceInputs, poses_text

        size, lens, near = 8, 1., .01
        source = plane_source([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        job, scene = sun_scene([0., 0., 1.])
        scene.tonemap_white = 2.
        background = SimpleNamespace(background=0x123456)
        scene.camera = SimpleNamespace(component=background)
        inputs = ReferenceInputs(settings=job.settings, bake=job.bake, lights=scene.lights, indirect=None,
                                 tonemap_white=scene.tonemap_white, camera=background, size=(size, size), poses={})
        shifts = ([0.], [.5], [-.5, 1.])
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            files = []
            for index, shift in enumerate(shifts):
                poses = [(LOOK_DOWN + np.array([x, 0., 0., 0., 0., 0.])).tolist() for x in shift]
                files.append(root / f"poses{index}.txt")
                files[-1].write_text(poses_text(size, size, lens, near, poses))
            together = [(file, root / "together" / file.stem) for file in files]
            # One worker, whatever this host's memory and cores: the pool's budget reads /proc.
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(patch.object(budget, "task_reservation", return_value=None))
                stack.enter_context(patch.object(budget, "available_bytes", return_value=(1 << 60,) * 3))
                stack.enter_context(patch.object(budget, "cores_available", return_value=1))
                stack.enter_context(patch.object(budget, "worker_capacity", return_value=1))
                with patch.object(reference_render, "lit_source", return_value=source):
                    reference_render.render_sets(inputs, together)
                stack.enter_context(patch.object(reference_render, "load_scene", return_value=scene))
                stack.enter_context(patch.object(reference_render, "source_for", return_value=(source, job)))
                for file in files:
                    reference_render.main(["scene", "--poses", str(file), "--out", str(root / "alone" / file.stem),
                                           "--normals"])
            for file in files:
                names = sorted(path.name for path in (root / "alone" / file.stem).iterdir())
                self.assertTrue(any(name.endswith(".normal.npy") for name in names), "main wrote normals")
                self.assertTrue(any(name.endswith(".png") for name in names))
                self.assertEqual(sorted(path.name for path in (root / "together" / file.stem).iterdir()), names)
                for name in names:
                    self.assertEqual((root / "together" / file.stem / name).read_bytes(),
                                     (root / "alone" / file.stem / name).read_bytes(), name)


@unittest.skipIf(np is None, "needs NumPy")
class BounceReferenceTests(unittest.TestCase):
    class Constant:
        def bounce(self, points, normals, ray_offset):
            return np.full((len(points), 3), 0.25)

    @needs_mitsuba
    def test_the_reference_adds_the_bounce_to_each_hit(self):
        source = plane_source([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        without = render_linear(source, *sun_scene([0., 0., 1.]), LOOK_DOWN, 1, 1, 0.1, 1)
        source.bounce = self.Constant()
        job, scene = sun_scene([0., 0., 1.])
        for intensity in (1.0, 2.0):
            scene.indirect = SimpleNamespace(intensity=intensity)
            with_bounce = render_linear(source, job, scene, LOOK_DOWN, 1, 1, 0.1, 1)
            np.testing.assert_allclose(with_bounce - without, 0.25 * intensity, atol=1e-12)

    def test_poses_with_bounced_light_render_in_this_process_and_keep_the_pool_otherwise(self):
        from unittest.mock import patch
        from r3d import reference_render as reference
        with patch.object(reference, '_write_pose', side_effect=[(1, 20)] * 3), \
                patch.object(reference.multiprocessing, 'get_all_start_methods', return_value=['fork']), \
                patch.object(reference.concurrent.futures, 'ProcessPoolExecutor') as executor:
            result = reference.render_poses(SimpleNamespace(bounce=object()), None, None, [None] * 3, 8, 8, 1., 1, '.')
        executor.assert_not_called()
        self.assertEqual(result[1], 1)

    def test_more_workers_than_one_are_refused_for_bounced_light(self):
        from r3d import reference_render as reference
        with self.assertRaisesRegex(ValueError, "leave --workers unset"):
            reference.render_poses(SimpleNamespace(bounce=object()), None, None, [None] * 3, 8, 8, 1., 1, '.', workers=4)


@unittest.skipIf(np is None, "needs NumPy")
class TraceDeviceTests(unittest.TestCase):
    """Where a reference set traces: CUDA when it loads a scene, else the mesh bake's variant; mesh bakes never ask."""

    def test_a_reference_traces_on_cuda_when_it_loads_and_on_the_bake_variant_otherwise(self):
        from unittest.mock import patch
        from r3d import ray_query
        for found, want in (("llvm_ad_rgb", ray_query.VARIANT), ("scalar_rgb", ray_query.VARIANT),
                            (ray_query.GPU_VARIANT, ray_query.GPU_VARIANT)):
            mi = SimpleNamespace(now="scalar_rgb", variant=lambda: mi.now)
            mi.set_variant = lambda name: setattr(mi, "now", name)

            def probe(mi):
                mi.set_variant(found)
                return found

            with patch.object(ray_query, "import_mitsuba", return_value=mi),                     patch.object(ray_query, "default_variant", side_effect=probe):
                self.assertEqual(ray_query.trace_variant(), want, found)
            self.assertEqual(mi.now, "scalar_rgb", "the probe leaves Mitsuba on the variant it was on")

    def test_a_set_on_the_gpu_renders_in_this_process_and_frees_the_device_after(self):
        from unittest.mock import patch
        from r3d import ray_query, reference_render
        from r3d.fitted_variant import poses_text

        inputs = SimpleNamespace(job=lambda: None, scene=lambda: None)
        for variant, freed in ((ray_query.GPU_VARIANT, 1), ("llvm_ad_rgb", 0)):
            with tempfile.TemporaryDirectory() as directory:
                poses = pathlib.Path(directory) / "poses.txt"
                poses.write_text(poses_text(2, 2, 1.0, 0.01, [LOOK_DOWN.tolist()]))
                with contextlib.ExitStack() as stack:
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    stack.enter_context(patch.object(ray_query, "trace_variant", return_value=variant))
                    lit = stack.enter_context(patch.object(reference_render, "lit_source", return_value=object()))
                    render = stack.enter_context(patch.object(reference_render, "render_poses", return_value=(0, 1)))
                    release = stack.enter_context(patch.object(ray_query, "release_gpu"))
                    self.assertEqual(reference_render.render_sets(inputs, [(poses, pathlib.Path(directory) / "out")]),
                                     variant)
            self.assertEqual(lit.call_args.kwargs["variant"], variant)
            self.assertEqual(render.call_args.kwargs["workers"], 1, "the probe may have opened CUDA: no forked workers")
            self.assertEqual(release.call_count, freed, variant)

    def test_the_device_is_freed_with_the_bounced_light_cache_empty_even_when_a_pose_fails(self):
        from unittest.mock import patch
        from r3d import mesh_import, ray_query, reference_render
        from r3d.fitted_variant import poses_text

        self.addCleanup(mesh_import.PATH_LIGHTS.clear)
        inputs = SimpleNamespace(job=lambda: None, scene=lambda: None)
        seen = []
        for failure in (None, RuntimeError("pose failed")):
            mesh_import.PATH_LIGHTS["cached"] = object()
            with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
                poses = pathlib.Path(directory) / "poses.txt"
                poses.write_text(poses_text(2, 2, 1.0, 0.01, [LOOK_DOWN.tolist()]))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(patch.object(ray_query, "trace_variant", return_value=ray_query.GPU_VARIANT))
                stack.enter_context(patch.object(reference_render, "lit_source", return_value=object()))
                stack.enter_context(patch.object(reference_render, "render_poses", side_effect=failure,
                                                 return_value=(0, 1)))
                stack.enter_context(patch.object(ray_query, "release_gpu",
                                                 side_effect=lambda: seen.append(dict(mesh_import.PATH_LIGHTS))))
                sets = [(poses, pathlib.Path(directory) / "out")]
                if failure is None:
                    reference_render.render_sets(inputs, sets)
                else:
                    with self.assertRaisesRegex(RuntimeError, "pose failed"):
                        reference_render.render_sets(inputs, sets)
        self.assertEqual(seen, [{}, {}], "released after success and after a failure, the cache already empty")

    def test_a_lit_source_traces_its_rays_and_its_bounced_light_on_one_variant(self):
        from unittest.mock import patch
        from r3d import reference_render

        source = plane_source([[-2., -2., 0.], [2., -2., 0.], [2., 2., 0.], [-2., 2., 0.]])
        job = SimpleNamespace(settings=SimpleNamespace(alpha_keep=None))
        for given in (None, "cuda_ad_rgb"):
            with patch.object(reference_render, "load_source", return_value=source),                     patch.object(reference_render, "RayQuery") as query,                     patch.object(reference_render, "path_light_for") as bounce:
                reference_render.lit_source(job, None, variant=given)
            self.assertEqual(query.call_args.args[2], given)
            self.assertEqual(bounce.call_args.args[3], given)

    def test_the_device_probe_answers_in_a_fresh_process_and_without_mitsuba(self):
        from unittest.mock import patch
        from r3d import ray_query

        with patch.object(ray_query, "import_mitsuba", return_value=None):
            self.assertEqual(ray_query.trace_variant(), ray_query.VARIANT)
        mi = SimpleNamespace(now=None, variant=lambda: mi.now)

        def set_variant(name):
            if name is None:
                raise TypeError("no variant to set")
            mi.now = name
        mi.set_variant = set_variant
        with patch.object(ray_query, "import_mitsuba", return_value=mi),                 patch.object(ray_query, "default_variant", return_value=ray_query.GPU_VARIANT):
            self.assertEqual(ray_query.trace_variant(), ray_query.GPU_VARIANT)

    @needs_mitsuba
    def test_releasing_the_device_runs_on_a_host_without_one(self):
        from r3d import ray_query
        ray_query.release_gpu()


def have_cuda():
    """Whether Mitsuba's CUDA variant traces here; asked only when a test runs, so a CPU host never opens CUDA."""
    from r3d import ray_query
    return ray_query.trace_variant() == ray_query.GPU_VARIANT


@needs_mitsuba
@unittest.skipIf(np is None, "needs NumPy")
class CudaParityTests(unittest.TestCase):
    """The reference on CUDA against the reference on LLVM, both with bounced light, on a sunlit corridor seen from a
    few poses. The same sampler seeds draw the same paths, so the two differ only in float rounding."""

    # Largest per-pixel difference over the mean pixel. None until a first run on a CUDA host measures it: until then
    # the test reports the difference and skips as not measured, so no bound passes unmeasured.
    RELATIVE_BOUND = None

    def setUp(self):
        if not have_cuda():
            self.skipTest("Mitsuba's CUDA variant does not trace here (no CUDA device, or no OptiX)")

    def tearDown(self):
        from r3d import mesh_import, mitsuba_reference
        mesh_import.PATH_LIGHTS.clear()
        mitsuba_reference.import_mitsuba().set_variant("scalar_rgb")

    def render(self, variant):
        from r3d.path_bake import PathLight
        from r3d.ray_query import RayQuery
        from tests.test_r3d_path_bake import SKY, SUN, corridor

        source = corridor()
        source.intersector = RayQuery(source.p, source.tri_v, variant)
        source.bounce = PathLight(source, [SUN, SKY], set(), SimpleNamespace(bounces=2, rays=32), 1.0, variant)
        job = SimpleNamespace(settings=SimpleNamespace(double_sided=set()),
                              bake=SimpleNamespace(ray_offset=0.01, ao=None))
        scene = SimpleNamespace(lights=[SUN, SKY], indirect=SimpleNamespace(intensity=1.0))
        poses = ([30.0, 20.0, 0.0, -0.3, -0.4, 0.0], [60.0, 5.0, 20.0, -1.0, -0.1, -0.3], [5.0, 40.0, -30.0, 0.2, -1.0, 0.4])
        return np.stack([render_linear(source, job, scene, np.array(pose), 16, 12, 0.6, 2) for pose in poses])

    @needs_llvm
    def test_cuda_and_llvm_references_agree(self):
        llvm = self.render("llvm_ad_rgb")
        cuda = self.render("cuda_ad_rgb")
        self.assertGreater(llvm.mean(), 0.0, "the poses see lit surfaces")
        worst = np.abs(cuda - llvm).max() / llvm.mean()
        if self.RELATIVE_BOUND is None:
            self.skipTest(f"not measured: largest pixel difference {worst:.3g} of the mean pixel; set RELATIVE_BOUND "
                          "from it")
        self.assertLess(worst, self.RELATIVE_BOUND)


@needs_mitsuba
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
                patch.object(budget, 'resident_bytes', return_value=(budget.GIB, 0, budget.GIB)), \
                patch.object(budget, 'available_bytes', return_value=(1 << 60,) * 3), \
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
                patch.object(budget, 'available_bytes', return_value=(1 << 60,) * 3), \
                patch.object(budget, 'cores_available', return_value=10), \
                patch.object(reference, '_write_pose', side_effect=[(1, 20), (1, 30), (1, 10)]):
            self.assertEqual(reference.render_poses(None, None, None, [None] * 3,
                                                   368, 448, 1., 4, '.'), (30, 1))
        self.assertIsNone(reference.POSE_STATE)


if __name__ == "__main__":
    unittest.main()
