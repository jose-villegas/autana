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

    from r3d.import_settings import LIGHT_MODE_BAKED
    from r3d.geometry import corner_normals
    from r3d.light import coincident_faces
    from r3d.reference_render import device_picture, primary_hits, render_linear, trace
    from render_compare import expand_565
except ImportError:
    np = None

from tests.r3d_env import needs_llvm, needs_mitsuba  # noqa: E402


def write_reference_tree(work, sets=None):
    """A reference set of `sets` (reference_set.fit_sets; a camera path's by default) in `work`: what a fit packs,
    returned by name, beside files it leaves out."""
    from r3d import reference_set

    sets = reference_set.fit_sets(True, False) if sets is None else sets
    expected = {name: name.encode() for name in
                [poses for poses, _ in sets.training + sets.held_out] + list(sets.coverage)}
    for _, folder in sets.training:
        for suffix in (".png", ".normal.npy"):
            name = f"{folder}/frame{suffix}"
            expected[name] = name.encode()
    for name, data in expected.items():
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for _, folder in sets.training + sets.held_out:
        path = work / folder
        path.mkdir(exist_ok=True)
        (path / "frame.linear.npy").write_bytes(b"linear light")
    for _, folder in sets.held_out:
        (work / folder / "frame.png").write_bytes(b"held out picture")
    return expected


class PackedReferencesTests(unittest.TestCase):
    def test_only_fit_inputs_round_trip_with_identical_bytes_despite_file_times(self):
        import os
        import tempfile
        from r3d import reference_set

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            work, unpacked = root / "source", root / "unpacked"
            work.mkdir()
            expected = write_reference_tree(work)
            sets = reference_set.fit_sets(True, False)
            packed = reference_set.pack_references(work, sets)
            self.assertEqual(reference_set.pack_references(work, sets), packed)
            for path in work.rglob("*"):
                os.utime(path, (0, 0))
            self.assertEqual(reference_set.pack_references(work, sets), packed)
            reference_set.unpack_references(packed, unpacked)
            self.assertEqual({path.relative_to(unpacked).as_posix(): path.read_bytes()
                              for path in unpacked.rglob("*") if path.is_file()}, expected)

    def packed_names(self, path, probed):
        import io
        import tarfile
        import tempfile
        from r3d import reference_set

        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            write_reference_tree(work)
            for poses, folder in (reference_set.PROBE_TRAINING, reference_set.PROBE_HELD_OUT):
                (work / poses).write_bytes(b"probe poses")
                (work / folder).mkdir()
                for suffix in reference_set.PACKED_SUFFIXES + (".linear.npy",):
                    (work / folder / f"face{suffix}").write_bytes(b"probe face")
            (work / reference_set.HELD_OUT[1] / "frame.normal.npy").write_bytes(b"held out normals")
            packed = reference_set.pack_references(work, reference_set.fit_sets(path, probed))
        with tarfile.open(fileobj=io.BytesIO(packed), mode="r:xz") as tar:
            return tar.getnames()

    def test_a_fit_the_import_does_not_shape_packs_the_path_s_poses_and_training_references_alone(self):
        self.assertEqual(self.packed_names(True, False),
                         ["coverage.txt", "held_out.txt", "train.txt", "train_landscape.txt",
                          "reference/frame.normal.npy", "reference/frame.png",
                          "reference_landscape/frame.normal.npy", "reference_landscape/frame.png"])

    def test_a_probe_fit_packs_the_held_out_references_for_closure(self):
        probes = ["probes.txt", "probes_held_out.txt"]
        faces = [f"{folder}/face{suffix}" for folder in ("reference_probes", "reference_probes_held_out")
                 for suffix in (".normal.npy", ".png")]
        self.assertEqual(self.packed_names(False, True), probes + faces)


def square(z=0.0, up=True):
    """The quad -2 < x, y < 2 at height `z`, wound to face +z, or -z when not `up`."""
    corners = [[-2.0, -2.0, z], [2.0, -2.0, z], [2.0, 2.0, z], [-2.0, 2.0, z]]
    return corners if up else corners[:1] + corners[:0:-1]


def plane_source(*quads):
    """One-material quads of two triangles each, facing +z when wound counter-clockwise seen from above, white, unlit
    by anything but the scene."""
    tris = np.array([[0, 1, 2], [0, 2, 3]])
    tri_v = np.concatenate([tris + 4 * index for index in range(len(quads))])
    source = SimpleNamespace(
        p=np.concatenate([np.array(quad, dtype=float) for quad in quads]), uv=np.zeros((4 * len(quads), 2)), tri_v=tri_v,
        tri_t=tri_v, tri_m=np.zeros(len(tri_v), dtype=int), names=["plane"], materials={"plane": {"Kd": (1.0, 1.0, 1.0)}},
        textures=[None], colors=None,
    )
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.twin = coincident_faces(source.p, source.tri_v)[0]
    source.bounce = None
    source.intersector = soup.rays(soup.Soup(source.p, source.tri_v))
    return source


def sun_scene(direction):
    settings = SimpleNamespace(double_sided=set(), seed=1)
    job = SimpleNamespace(settings=settings, bake=SimpleNamespace(ray_offset=0.01, ao=None))
    scene = SimpleNamespace(lights=[soup.sun(direction, [1.0, 1.0, 1.0], 1.0)])
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

        source = plane_source(square())
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
        source = plane_source(square())
        picture = render_linear(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[1.0, 1.0, 1.0]]], atol=1e-12, rtol=0)

    @needs_mitsuba
    def test_a_tilted_sun_scales_the_value_by_the_cosine_to_the_normal(self):
        source = plane_source(square())
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
        # Double-sided and wound to face away from the eye at z = 1: the buffer still turns its normal toward the eye.
        source = plane_source([[-0.5, 0.5, 0.0], [-0.5, 2.0, 0.0], [0.0, 2.0, 0.0], [0.0, 0.5, 0.0]])
        job, scene = sun_scene([0.0, 0.0, 1.0])
        job.settings.double_sided = {"plane"}
        _linear, _covered, normal = trace(source, job, scene, LOOK_DOWN, 2, 2, 1.0, 2)
        np.testing.assert_allclose(normal[0, 0], [0.0, 0.0, 1.0], atol=1e-12, rtol=0)
        np.testing.assert_allclose(normal[1], 0.0, atol=1e-12, rtol=0)

    @needs_mitsuba
    def test_a_single_sided_face_seen_from_behind_is_passed_as_the_device_culls_it(self):
        # Two crossing shells, as a reduced source leaves them: the eye at z = 1 meets the back of a face at z = 0.5
        # first. The device culls it and draws the floor at z = 0; double-sided, it is drawn itself.
        source = plane_source(square(0.5, up=False), square())
        job, _scene = sun_scene([0.0, 0.0, 1.0])
        _origin, _direction, locations, rays, faces, _normal, _bary, _double = primary_hits(source, job, LOOK_DOWN, 1, 1,
                                                                                            0.1, 1)
        np.testing.assert_array_equal(rays, [0])
        np.testing.assert_allclose(locations[:, 2], [0.0], atol=1e-6)
        job.settings.double_sided = {"plane"}
        locations = primary_hits(source, job, LOOK_DOWN, 1, 1, 0.1, 1)[2]
        np.testing.assert_allclose(locations[:, 2], [0.5], atol=1e-6)

    @needs_mitsuba
    def test_a_back_face_over_its_twin_draws_the_twin(self):
        # The two sides of a thin wall share positions: whichever the tracer meets, the eye sees the front one.
        source = plane_source(square(up=False), square(), square(-1.0))
        faces = primary_hits(source, *sun_scene([0.0, 0.0, 1.0])[:1], LOOK_DOWN, 1, 1, 0.1, 1)[4]
        self.assertIn(faces[0], (2, 3))

    @needs_mitsuba
    def test_a_pose_at_empty_sky_is_the_scene_background_as_the_device_shows_it(self):
        source = plane_source(square())
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
        source = plane_source(square())
        wall = soup.box(extents=(.1, 4., 2.))
        wall.apply_translation((1.2, 0., 1.))
        mesh = soup.concatenate([soup.Soup(source.p, source.tri_v), wall])
        source.p, source.tri_v = mesh.vertices, mesh.faces
        source.tri_t = np.zeros_like(source.tri_v)
        source.tri_m = np.zeros(len(source.tri_v), dtype=int)
        source.corner_normals = corner_normals(source.p, source.tri_v)
        source.twin = coincident_faces(source.p, source.tri_v)[0]
        source.intersector = soup.rays(mesh)
        job, scene = sun_scene([0., 0., -1.])
        job.bake.ao = SimpleNamespace(distance=3., strength=.8, rays=8, indirect=True)
        scene.lights.append({"type": "ambient", "mode": LIGHT_MODE_BAKED, "color": [1., 1., 1.], "intensity": .5})
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
        source = plane_source(square())
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
        source = plane_source(square())
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

    def test_poses_traced_on_cuda_render_in_this_process_and_cpu_poses_keep_the_pool(self):
        from unittest.mock import patch
        from r3d import process_budget as budget
        from r3d import reference_render as reference
        for variant, pooled in (("cuda_ad_rgb", False), ("llvm_ad_rgb", True)):
            source = SimpleNamespace(bounce=None, intersector=SimpleNamespace(variant=variant))
            with self.subTest(variant=variant), patch.object(reference, '_write_pose', return_value=(1, 20)), \
                    patch.object(budget, 'task_reservation', return_value=None), \
                    patch.object(budget, 'available_bytes', return_value=(1 << 60,) * 3), \
                    patch.object(budget, 'cores_available', return_value=3), \
                    patch.object(budget, 'worker_capacity', return_value=3), \
                    patch.object(reference.multiprocessing, 'get_all_start_methods', return_value=['fork']), \
                    patch.object(reference.multiprocessing, 'get_context'), \
                    patch.object(reference.concurrent.futures, 'ProcessPoolExecutor') as executor:
                executor.return_value.__enter__.return_value.map.return_value = [(2, 20)] * 3
                result = reference.render_poses(source, None, None, [None] * 3, 8, 8, 1., 1, '.')
            self.assertEqual(executor.called, pooled)
            self.assertEqual(result[1], 3 if pooled else 1)


def render_one_set(job, **patches):
    """`reference_render.render_sets` over one look-down pose of `job`, with each named attribute of reference_render
    patched by its keyword arguments (render_poses returns no work unless given); returns the patches by name."""
    import contextlib
    import io
    import tempfile
    from unittest.mock import patch
    from r3d import reference_render
    from r3d.fitted_variant import poses_text

    patches.setdefault("render_poses", {"return_value": (0, 1)})
    inputs = SimpleNamespace(job=lambda: job, scene=lambda: None)
    with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
        poses = pathlib.Path(directory) / "poses.txt"
        poses.write_text(poses_text(2, 2, 1.0, 0.01, [LOOK_DOWN.tolist()]))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        mocks = {name: stack.enter_context(patch.object(reference_render, name, **kwargs))
                 for name, kwargs in patches.items()}
        reference_render.render_sets(inputs, [(poses, pathlib.Path(directory) / "out")])
    return mocks


@unittest.skipIf(np is None, "needs NumPy")
class PoseStoreTests(unittest.TestCase):
    """A set traces only the poses its pose store lacks; a pose whose text changes is traced again, alone, in this
    process or in CUDA workers alike."""

    def render(self, poses_text, cache, out, key="render", workers=1):
        """render_sets of one poses file holding `poses_text` with a PoseStore under `key`, its poses traced here or
        by `workers` workers; returns the poses traced. A traced pose's files hold its pose line, so a set shows which
        pose each index got."""
        import contextlib
        import io
        from unittest.mock import patch
        from bake import produce
        from r3d import reference_render

        traced = []

        def fake_render(source, job, scene, poses, width, height, lens, samples, target, normals=False):
            for index, pose in enumerate(poses):
                traced.append(list(pose))
                for suffix in reference_render.POSE_SUFFIXES:
                    (target / f"{index:04d}{suffix}").write_text(repr([float(value) for value in pose]))
            return 0, 1

        def fake_workers(inputs, plans, samples, count):
            self.assertEqual(count, workers)
            for _out, target, _folders, _missing, poses, size in plans:
                fake_render(None, None, None, poses, *size, samples, target)

        poses = out.parent / f"{out.name}.txt"
        poses.write_text(poses_text)
        inputs = SimpleNamespace(job=lambda: None, scene=lambda: None)
        with patch.object(reference_render, "lit_source", return_value=object()), \
                patch.object(reference_render, "render_poses", side_effect=fake_render), \
                patch.object(reference_render, "reference_variant", return_value="cuda_ad_rgb"), \
                patch.object(reference_render, "gpu_workers", return_value=workers), \
                patch.object(reference_render, "trace_in_workers", side_effect=fake_workers), \
                patch.object(produce.keys, "gpu_name", return_value="GPU"), \
                contextlib.redirect_stdout(io.StringIO()):
            reference_render.render_sets(inputs, [(poses, out)], store=produce.PoseStore(cache, key))
        return traced

    def test_a_changed_pose_is_traced_again_and_the_rest_come_from_the_store(self):
        import tempfile
        from r3d.fitted_variant import poses_text
        from r3d.reference_render import POSE_SUFFIXES

        for workers in (1, 3):
            with self.subTest(workers=workers):
                poses = [[0.0, 0.0, float(z), 0.0, 0.0, -1.0] for z in (1, 2, 3)]
                with tempfile.TemporaryDirectory() as directory:
                    root = pathlib.Path(directory)
                    cache = root / "cache"
                    self.assertEqual(self.render(poses_text(4, 4, 1.0, 0.01, poses), cache, root / "first", workers=workers), poses)
                    moved = poses[:1] + [[0.0, 0.0, 9.0, 0.0, 0.0, -1.0]] + poses[2:]
                    self.assertEqual(self.render(poses_text(4, 4, 1.0, 0.01, moved), cache, root / "second", workers=workers), [moved[1]])
                    for index, pose in enumerate(moved):
                        for suffix in POSE_SUFFIXES:
                            self.assertEqual((root / "second" / f"{index:04d}{suffix}").read_text(), repr(pose))
                    self.assertFalse((root / "second" / ".tracing").exists())
                    self.assertEqual(self.render(poses_text(4, 4, 1.0, 0.01, moved), cache, root / "third"), [],
                                     "every pose is held")
                    self.assertEqual(len(self.render(poses_text(4, 4, 1.0, 0.01, moved), cache, root / "fourth", key="other")),
                                     3, "another render key traces every pose")
                    self.assertEqual(len(self.render(poses_text(8, 4, 1.0, 0.01, moved), cache, root / "fifth")), 3,
                                     "another pose size traces every pose")


class GpuWorkerTests(unittest.TestCase):
    """CUDA poses go to as many worker processes as host and GPU memory hold; off CUDA to this process."""

    def workers(self, variant, poses, room):
        from unittest.mock import patch
        from r3d import process_budget as budget
        from r3d import reference_render
        available = tuple(floor + room * cost for floor, cost in zip(budget.FLOORS, budget.REFERENCE_GPU_BYTES))
        with patch.object(reference_render, "reference_variant", return_value=variant), \
                patch.object(budget, "available_bytes", return_value=available), \
                patch.object(budget, "cores_available", return_value=32):
            return reference_render.gpu_workers(poses)

    def test_cuda_takes_the_workers_memory_holds_and_no_more_than_the_poses(self):
        self.assertEqual(self.workers("cuda_ad_rgb", 100, 3), 3)
        self.assertEqual(self.workers("cuda_ad_rgb", 2, 3), 2)
        self.assertEqual(self.workers("cuda_ad_rgb", 100, 0), 1, "one process even without room for a worker")

    def test_off_cuda_or_for_one_pose_this_process_traces(self):
        self.assertEqual(self.workers("llvm_ad_rgb", 100, 3), 1)
        self.assertEqual(self.workers("cuda_ad_rgb", 1, 3), 1)


@unittest.skipIf(np is None, "needs NumPy")
class RepeatTests(unittest.TestCase):
    """A reference set is the fit's target, and a fit magnifies a one-pixel change into a different mesh, so the set
    must repeat byte for byte however often its scene is built (ray_query.py: OptiX may build a different tree)."""

    @needs_mitsuba
    @needs_llvm
    def test_a_reference_repeats_byte_for_byte_from_a_scene_built_again(self):
        from r3d.path_bake import PathLight
        from r3d.ray_query import RayQuery
        from tests.test_r3d_path_bake import SKY, SUN, corridor

        job = SimpleNamespace(settings=SimpleNamespace(double_sided=set()), bake=SimpleNamespace(ray_offset=0.01, ao=None))
        scene = SimpleNamespace(lights=[SUN, SKY], indirect=SimpleNamespace(intensity=1.0))
        poses = ([30.0, 20.0, 0.0, -0.3, -0.4, 0.0], [60.0, 5.0, 20.0, -1.0, -0.1, -0.3], [5.0, 40.0, -30.0, 0.2, -1.0, 0.4])
        renders = []
        for _ in range(2):
            source = corridor()
            source.twin = coincident_faces(source.p, source.tri_v)[0]
            source.intersector = RayQuery(source.p, source.tri_v)
            source.bounce = PathLight(source, [SUN, SKY], set(), SimpleNamespace(bounces=2, rays=32), 1.0)
            renders.append(np.stack([render_linear(source, job, scene, np.array(pose), 16, 12, 0.6, 2) for pose in poses]))
        self.assertGreater(renders[0].mean(), 0.0, "the poses see lit surfaces")
        self.assertEqual(renders[0].tobytes(), renders[1].tobytes())

    def test_a_set_traces_its_rays_and_bounced_light_on_the_reference_variant(self):
        from tests.test_r3d_path_bake import corridor

        def variant_of(call, at):
            args, kwargs = call
            return kwargs.get("variant", args[at] if len(args) > at else None)

        job = SimpleNamespace(settings=SimpleNamespace(alpha_keep=None, cards=None))
        mocks = render_one_set(job, load_source={"return_value": corridor()}, RayQuery={}, path_light_for={},
                               reference_variant={"return_value": "cuda_ad_rgb"})
        self.assertEqual(variant_of(mocks["RayQuery"].call_args, 2), "cuda_ad_rgb")
        self.assertEqual(variant_of(mocks["path_light_for"].call_args, 3), "cuda_ad_rgb")

    def test_the_reference_variant_is_the_fastest_this_machine_traces_on_else_the_bake_variant(self):
        from unittest.mock import patch
        from r3d import mitsuba_reference, ray_query
        from r3d import reference_render as reference
        for found, expected in (("cuda_ad_rgb", "cuda_ad_rgb"), (None, ray_query.VARIANT)):
            reference.reference_variant.cache_clear()
            self.addCleanup(reference.reference_variant.cache_clear)
            with self.subTest(found=found), \
                    patch.object(mitsuba_reference, "import_mitsuba",
                                 return_value=found and SimpleNamespace(variant=lambda: None)), \
                    patch.object(mitsuba_reference, "default_variant", return_value=found):
                self.assertEqual(reference.reference_variant(), expected)

    def test_the_bounced_light_cache_is_dropped_even_when_a_pose_fails(self):
        import contextlib
        from r3d import mesh_source

        self.addCleanup(mesh_source.PATH_LIGHTS.clear)
        for failure in (None, RuntimeError("pose failed")):
            mesh_source.PATH_LIGHTS["cached"] = object()
            raised = contextlib.nullcontext() if failure is None else self.assertRaisesRegex(RuntimeError, "pose failed")
            with raised:
                render_one_set(None, lit_source={"return_value": object()},
                               render_poses={"side_effect": failure, "return_value": (0, 1)})
            self.assertEqual(mesh_source.PATH_LIGHTS, {}, failure)


@needs_mitsuba
@unittest.skipIf(np is None, "needs NumPy")
class SkyStreamingTests(unittest.TestCase):
    def test_sky_directions_are_streamed_and_match_list(self):
        from unittest.mock import patch
        from r3d import light as lighting
        source = plane_source(square())
        original = lighting.unshadowed_count
        sky = {"type": "sky", "mode": LIGHT_MODE_BAKED, "rays": 7, "color": [.5, .7, .9], "intensity": .8}
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
