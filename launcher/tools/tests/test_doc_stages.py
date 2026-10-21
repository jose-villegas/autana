"""The GPU and board doc stages check what they are given before they publish."""
import importlib.util
import hashlib
import json
import tempfile
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


PATH = Path(__file__).resolve().parents[1] / "render/doc_stages.py"
spec = importlib.util.spec_from_file_location("doc_stages", PATH)
stages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stages)
from bake import bake

MEMINFO = Path("/proc/meminfo")
POWERSHELL = Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")


class DocStagesTests(unittest.TestCase):
    def test_capture_intervals_are_required_positive_and_common(self):
        first, second = self.tmp / "a.log", self.tmp / "b.log"
        for text in ("", "pose_every_ms=0", "pose_every_ms=500\npose_every_ms=1000"):
            with self.subTest(text=text):
                first.write_text(text)
                with self.assertRaisesRegex(ValueError, "pose interval"):
                    stages.capture_pose_every_ms([first])
        first.write_text("pose_every_ms=500")
        second.write_text("pose_every_ms=1000")
        with self.assertRaisesRegex(ValueError, "pose interval"):
            stages.capture_pose_every_ms([first, second])

    def test_swapped_pose_rows_keep_their_logged_times(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("CAPTURE pose_every_ms=500\n"
                           "atrium t=500ms clusters=4 | both cores: frame 52000us\n"
                           "atrium t=0ms clusters=4 | both cores: frame 51000us\n")
        expected = [("sponza", 0), ("sponza", 500)]
        self.assertEqual(stages.board_frames([capture], expected, "sponza", "atrium", 500),
                         {("sponza", 0): 51.0, ("sponza", 500): 52.0})

    def test_path_cull_has_its_own_sampling_for_region_visibility(self):
        from types import SimpleNamespace as NS
        import numpy as np
        from r3d import mesh_import, lit_mesh, light, ray_query
        baked = NS(object=NS(name="hall"), renderer=NS(variant=NS(name="full")))
        scene = NS(renderers=[baked])
        job = NS(renderer=NS(visibility=NS(source="camera_region", rounds=2)))
        geometry = NS(positions=np.zeros((3, 3)), tris=np.array([[0, 1, 2]]),
                      tri_double=np.array([False]), rgb=np.zeros((3, 3)), scale={})
        poses = (224, 224, .5, 1, [[0, 0, 0, 0, 0, -1]])
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(mesh_import, "bake_geometry", return_value=geometry), \
                mock.patch.object(mesh_import, "write_baked"), \
                mock.patch.object(mesh_import, "camera_path_poses", return_value=poses) as sample, \
                mock.patch.object(ray_query, "RayQuery"), \
                mock.patch.object(light, "visible_from_path", return_value=np.array([True])) as visible, \
                mock.patch.object(lit_mesh, "write_lit_mesh"):
            stages.bake_worker(scene, job, True, "full", "hall", Path(directory))
        settings = sample.call_args.args[1]
        self.assertEqual((settings.every_ms, settings.size, settings.samples, settings.margin),
                         (100, (184, 224), 3, 8))
        self.assertEqual(visible.call_args.args[4:], (poses[-1], *poses[:4], 3, 8))

    def test_board_and_smoke_sample_the_fit_with_region_visibility(self):
        from types import SimpleNamespace as NS
        from r3d import import_settings, fitted_variant, mesh_import, reference_render
        fit = NS(size=(8, 6), held_out_every_ms=2000)
        job = NS(renderer=NS(fit=fit, visibility=NS(source="camera_region", rounds=2),
                             variant=NS(name="full")), asset_path=Path("start.mesh"), object=NS(name="hall"))
        scene = NS(renderers=[job])
        args = NS(build_commit=None, capture=[], scene=Path("room.scene.toml"), object="hall")
        poses = (8, 6, .5, 1, [[0, 0, 0, 0, 0, -1]])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tables").mkdir()
            capture = root / "capture.txt"
            capture.write_text("CAPTURE pose_every_ms=2000\nFRAME COST rendered 8x6\n")
            args.capture = [capture]
            with mock.patch.object(import_settings, "load_scene", return_value=scene), \
                    mock.patch.object(fitted_variant, "placed_variant", return_value=job), \
                    mock.patch.object(mesh_import, "camera_path_poses", return_value=poses) as sample, \
                    mock.patch.object(stages.subprocess, "check_output", return_value=b"commit"), \
                    mock.patch.object(stages.subprocess, "run", return_value=NS(returncode=0)), \
                    mock.patch.object(stages, "board_means", return_value=dict.fromkeys(("room", *stages.VARIANTS), 1)), \
                    mock.patch.object(stages, "board_frames", side_effect=ValueError("stop before cost fitting")):
                with self.assertRaisesRegex(ValueError, "stop before cost fitting"):
                    stages.board(args, root, root)
            sample.assert_called_once_with(scene, fit, fit.held_out_every_ms, either_way_up=False)
            with mock.patch.object(mesh_import, "camera_path_poses", return_value=poses) as sample, \
                    mock.patch.object(stages.shutil, "copyfile"), \
                    mock.patch.object(reference_render, "main") as render:
                stages.smoke_prepare(args.scene, scene, job, root / "inputs")
            sample.assert_called_once_with(scene, fit, 5000, either_way_up=False)
            self.assertEqual(render.call_count, 3)

    def test_viewer_host_build_receives_the_scene(self):
        from r3d.bake_fidelity import build_host
        with mock.patch("r3d.bake_fidelity.subprocess.run") as run:
            run.return_value.stdout = "built viewer\n"
            build_host(Path("scene_viewer.sh"), Path("out"), Path("other.scene.toml"))
        self.assertEqual(run.call_args.args[0], ["sh", "scene_viewer.sh", Path("other.scene.toml").as_posix(),
                                               "--build-only", "-o", "out"])

    def test_measure_worker_renders_the_job_object_and_packs_only_its_scene(self):
        from types import SimpleNamespace as NS
        from r3d import bake_fidelity, appearance_simplify, lit_mesh, cost_model, poses
        import numpy as np
        scene = Path("gallery.scene.toml")
        job = NS(asset_name="gallery.paint", object=NS(name="painting"),
                 renderer=NS(fit=NS(held_out_every_ms=17)))
        with mock.patch.object(poses, "read_poses", return_value=(1, 1, .5, 1, [0, 1])), \
                mock.patch.object(bake_fidelity, "score", return_value=(1, 2, 3, 4, 5)) as score, \
                mock.patch.object(bake_fidelity, "pack_bytes", return_value={}) as pack, \
                mock.patch.object(bake_fidelity, "write_packs"), \
                mock.patch.object(appearance_simplify, "load_views", return_value=([], 1)), \
                mock.patch.object(appearance_simplify, "normal_error", return_value=0), \
                mock.patch.object(appearance_simplify, "start_mesh"), \
                mock.patch.object(lit_mesh, "read_lit_mesh"), \
                mock.patch.object(lit_mesh, "finest_triangles", return_value=([], [], [0])), \
                mock.patch.object(cost_model, "mesh_rows"), \
                mock.patch.object(cost_model, "predict", return_value=np.array([1])):
            stages.measure_worker("paint", job, Path("scratch.mesh"), self.tmp, self.tmp, Path("host"), [], scene)
        fields = score.call_args.args[0].render_args.split()
        self.assertEqual(fields[fields.index("--scene") + 1], "gallery")
        self.assertEqual(fields[fields.index("--object") + 1], "painting")
        pack.assert_called_once_with([scene], ["gallery.paint=scratch.mesh"])

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tmp = Path(directory.name)

    def test_board_uses_the_callers_scene_and_object(self):
        from types import SimpleNamespace as NS
        from r3d import cost_model, import_settings, mesh_import, fitted_variant
        import numpy as np
        scene_path = Path("gallery.scene.toml")
        job = NS(renderer=NS(fit=NS(held_out_every_ms=1000), visibility=object()))
        scene = NS(renderers=[NS(object=NS(name=name), asset_path=Path(name + ".mesh"))
                              for name in ("painting", "painting_lite")])
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\nFRAME COST rendered 8x6\nCAPTURE pose_every_ms=1000\n" +
            "".join(f"{name} both cores: mean {value}us\n" for name, value in
                    (("painting", 51000), ("painting_lite", 42000), ("painting_flat", 43000),
                     ("painting_fitted", 44000), ("painting_fitted_full", 45000), ("painting_flat_fitted", 46000))) +
            "painting t= 0s clusters=4 | both cores: frame 51000us\n"
            "painting_lite t= 0s clusters=4 | both cores: frame 42000us\n")
        out, work = self.tmp / "out", self.tmp / "work"
        (out / "tables").mkdir(parents=True)
        work.mkdir()
        weights = self.tmp / "weights.txt"
        weights.write_text("")
        features = np.ones((1, len(cost_model.FEATURES)))
        with mock.patch.object(stages.subprocess, "check_output", return_value=b"abcdef123456"), \
                mock.patch.object(stages.subprocess, "run", return_value=NS(returncode=0)), \
                mock.patch.object(import_settings, "load_scene", return_value=scene), \
                mock.patch.object(fitted_variant, "placed_variant", return_value=job) as placed, \
                mock.patch.object(mesh_import, "camera_path_poses", return_value=(8, 6, .62, 6, [[0]*7])), \
                mock.patch.object(fitted_variant, "poses_text", return_value="poses"), \
                mock.patch.object(cost_model, "mesh_rows", return_value=features) as rows, \
                mock.patch.object(cost_model, "fit", return_value=np.ones(len(cost_model.FEATURES))) as fit, \
                mock.patch.object(stages, "current_stamp", return_value="stamp"), \
                mock.patch.object(stages, "apply_tables"), mock.patch.object(stages, "WEIGHTS", weights), \
                mock.patch.object(stages, "ROOT", self.tmp):
            self.assertEqual(stages.board(NS(scene=scene_path, object="painting", capture=[capture],
                                             build_commit=None, check=False), out, work), 0)
        placed.assert_called_once_with(scene, "painting_fitted")
        self.assertEqual([call.args[0] for call in rows.call_args_list],
                         [Path("painting.mesh"), Path("painting_lite.mesh")])
        self.assertEqual(fit.call_args.args[1], [51.0, 42.0])
        self.assertEqual({path.name for path in (out / "tables").iterdir()},
                         {"gallery-board.md", "gallery-board-model.md"})
        self.assertIn("| gallery | 51.000 |", (out / "tables/gallery-board.md").read_text())

    def test_current_stamp_needs_no_numeric_dependencies(self):
        launcher = self.tmp / "launcher"
        launcher.mkdir()
        (launcher / "m.import.toml").write_text('[source]\npath = "m.obj"\ncredit = "c"\n'
                                               '[output]\nname = "mesh"\n')
        (launcher / "m.obj").write_text("geometry")
        (launcher / "m.mtl").write_text("newmtl m\nmap_Kd texture.png\nmap_d alpha.png\n")
        (launcher / "texture.png").write_bytes(b"texture")
        (launcher / "alpha.png").write_bytes(b"alpha")
        code = """
import importlib.util
from pathlib import Path
import sys
from unittest import mock
sys.modules.update(numpy=None, PIL=None)
spec = importlib.util.spec_from_file_location("doc_stages", sys.argv[1])
stages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stages)
with mock.patch.object(stages, "ROOT", Path(sys.argv[2])), mock.patch.object(
        stages.subprocess, "check_output", return_value=b"launcher/m.import.toml\\0"):
    assert len(stages.current_stamp()) == 64
"""
        result = subprocess.run([sys.executable, "-c", code, str(PATH), str(self.tmp)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_capture_requires_matching_commit(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("other-diag\nlite both cores: mean 42000us\n")
        with self.assertRaisesRegex(ValueError, "build"):
            stages.capture_rows(capture, "abcdef123456", "sponza", "atrium")

    def test_capture_uses_existing_perf_parser(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\nlite both cores: mean 42000us\n")
        self.assertEqual(stages.capture_rows(capture, "abcdef123456", "sponza", "atrium"), {"lite": 42.0})

    def test_capture_selects_portrait_and_resolves_variant_names(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\natrium_lite both cores: mean 42000us quarter=portrait\n"
                           "atrium_lite both cores: mean 84000us quarter=landscape\n")
        self.assertEqual(stages.capture_rows(capture, "abcdef123456", "sponza", "atrium"), {"lite": 42.0})

    def test_source_stamp_ignores_doc_refresh_but_not_recipe(self):
        (self.tmp / "launcher").mkdir()
        source = self.tmp / "launcher/recipe.toml"
        source.write_text("steps = 8")
        stamp = stages.source_stamp(self.tmp, [source])
        (self.tmp / "README.md").write_text("refreshed docs")
        self.assertEqual(stages.source_stamp(self.tmp, [source]), stamp)
        source.write_text("steps = 9")
        self.assertNotEqual(stages.source_stamp(self.tmp, [source]), stamp)

    def test_smoke_cannot_publish_or_check(self):
        with self.assertRaisesRegex(ValueError, "smoke"):
            stages.validate_mode(smoke=True, check=True)

    def test_source_stamp_matches_pointer_and_hydrated_content(self):
        source = self.tmp / "source.obj"
        content = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
        source.write_bytes(content)
        hydrated = stages.source_stamp(self.tmp, [source])
        checksum = hashlib.sha256(content).hexdigest()
        source.write_bytes(("version https://git-lfs.github.com/spec/v1\n"
                            f"oid sha256:{checksum}\nsize {len(content)}\n").encode())
        self.assertEqual(stages.source_stamp(self.tmp, [source]), hydrated)

    def test_board_rejects_missing_variants(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\nlite both cores: mean 42000us\n")
        with self.assertRaisesRegex(ValueError, "missing"):
            stages.board_means([capture], "abcdef123456", "sponza", "atrium")

    def test_a_capture_of_all_six_bake_entities_yields_every_board_variant(self):
        names = ("painting", "painting_flat", "painting_lite", "painting_fitted", "painting_fitted_full", "painting_flat_fitted")
        capture = self.tmp / "capture.txt"
        lines = [f"{name} both cores: mean {1000 * (k + 1)}us" for k, name in enumerate(names)]
        capture.write_text("\n".join(["abcdef123456-diag", *lines]) + "\n")
        means = stages.board_means([capture], "abcdef123456", "gallery", "painting")
        self.assertEqual(set(means), {"gallery", *stages.VARIANTS})
        self.assertEqual(means, {"gallery": 1.0, "flat": 2.0, "lite": 3.0, "fitted": 4.0, "fitted-full": 5.0,
                                 "flat-fitted": 6.0})

    def test_a_capture_without_the_flat_fitted_entity_is_refused(self):
        names = ("painting", "painting_flat", "painting_lite", "painting_fitted", "painting_fitted_full")
        capture = self.tmp / "capture.txt"
        lines = [f"{name} both cores: mean 42000us" for name in names]
        capture.write_text("\n".join(["abcdef123456-diag", *lines]) + "\n")
        with self.assertRaisesRegex(ValueError, "flat-fitted"):
            stages.board_means([capture], "abcdef123456", "gallery", "painting")

    def test_board_half_second_pose_rows(self):
        capture = self.tmp / "capture.txt"
        capture.write_text('CAPTURE period_ms=1000 pose_every_ms=500\n'
                           'atrium t=    0s clusters=4 tris=5 | both cores: frame 51000us\n'
                           'atrium t=  500ms clusters=4 tris=5 | both cores: frame 52000us\n'
                           'atrium_lite t=    0s clusters=4 tris=5 | both cores: frame 42000us\n'
                           'atrium_lite t=  500ms clusters=4 tris=5 | both cores: frame 43000us\n')
        expected = [(name, time) for name in ('sponza', 'lite') for time in (0, 500)]
        self.assertEqual(stages.board_frames([capture], expected, 'sponza', 'atrium', 500),
                         {('sponza', 0): 51.0, ('sponza', 500): 52.0,
                          ('lite', 0): 42.0, ('lite', 500): 43.0})

    def test_board_pose_rows_require_complete_unique_samples(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("atrium t=    0s clusters= 4 tris=5 | both cores: frame 51000us\n"
                           "atrium_lite t=    0s clusters= 4 tris=5 | both cores: frame 42000us\n")
        expected = [("sponza", 0), ("lite", 0)]
        self.assertEqual(stages.board_frames([capture], expected, "sponza", "atrium", 500), {("sponza", 0): 51.0, ("lite", 0): 42.0})
        with self.assertRaisesRegex(ValueError, "exactly once"):
            stages.board_frames([capture], [*expected, ("sponza", 5)], "sponza", "atrium", 500)
        capture.write_text(capture.read_text() + capture.read_text().splitlines()[0] + "\n")
        with self.assertRaisesRegex(ValueError, "exactly once"):
            stages.board_frames([capture], expected, "sponza", "atrium", 500)

    def test_gpu_check_requires_saved_full_run(self):
        with self.assertRaisesRegex(ValueError, "no full run"):
            stages.check_gpu(None, self.tmp)

    def test_board_rejects_a_different_render_size(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("=== atrium FRAME COST (10 tris, 20 verts, 1 clusters, rendered 368x448) ===\n")
        with self.assertRaisesRegex(ValueError, "render size"):
            stages.capture_viewport(capture, (184, 224))

    def test_gpu_check_rejects_missing_saved_artifact(self):
        (self.tmp / "source.json").write_text(json.dumps({"source": bake.digest({}),
                                                          "files": {"tables/score.md": "0" * 64}}))
        with mock.patch.object(stages, "gpu_inputs", return_value={}):
            with self.assertRaisesRegex(ValueError, "missing or altered"):
                stages.check_gpu(mock.Mock(), self.tmp)

    def guard_with(self, wsl_kib, host_bytes, error=None):
        """Run memory_guard with WSL's MemAvailable and the host's AvailableBytes faked."""
        read_text, exists = Path.read_text, Path.exists

        def fake_read_text(path, *args, **kwargs):
            if path == MEMINFO:
                return f"MemAvailable: {wsl_kib} kB\n"
            return read_text(path, *args, **kwargs)

        def fake_exists(path):
            return True if path == POWERSHELL else exists(path)

        with mock.patch.object(Path, "read_text", fake_read_text), \
                mock.patch.object(Path, "exists", fake_exists), \
                mock.patch.object(stages.subprocess, "check_output", return_value=f"{host_bytes}\r\n".encode(), side_effect=error):
            stages.memory_guard()

    def test_memory_guard_checks_limiting_windows_host(self):
        with self.assertRaisesRegex(ValueError, "Windows host is short by 1.00 GiB"):
            self.guard_with(wsl_kib=16 * 1024 ** 2, host_bytes=1 * 1024 ** 3)

    def test_memory_guard_checks_limiting_wsl(self):
        with self.assertRaisesRegex(ValueError, "WSL is short by 2.00 GiB"):
            self.guard_with(wsl_kib=4 * 1024 ** 2, host_bytes=16 * 1024 ** 3)

    def test_memory_guard_passes_with_enough_on_both_sides(self):
        self.guard_with(wsl_kib=6 * 1024 ** 2, host_bytes=2 * 1024 ** 3)

    def test_memory_guard_reports_interop_failure(self):
        with self.assertRaisesRegex(ValueError, "Windows host memory query failed.*Invalid argument"):
            self.guard_with(wsl_kib=8 * 1024 ** 2, host_bytes=0, error=OSError("Invalid argument"))

    def test_memory_guard_reports_unparseable_host_memory(self):
        with self.assertRaisesRegex(ValueError, "Windows host memory query failed.*not a byte count"):
            self.guard_with(wsl_kib=8 * 1024 ** 2, host_bytes="Invalid argument")

    def test_smoke_preserves_full_gpu_results(self):
        result = self.tmp / "out/gpu/measurements.json"
        result.parent.mkdir(parents=True)
        result.write_text("full run")
        with mock.patch.object(stages, "RESULTS", self.tmp), mock.patch.object(stages, "gpu", lambda *args: 0):
            self.assertEqual(stages.main(["--stage", "gpu", "--smoke", "--scene", "other.scene.toml", "--object", "hall"]), 0)
        self.assertEqual(result.read_text(), "full run")


class RayTracingPreflightTests(unittest.TestCase):
    def test_a_runner_that_cannot_load_the_llvm_backend_is_refused_with_the_fix(self):
        failed = subprocess.CompletedProcess([], 1, "", "ImportError: libatomic.so.1: cannot open shared object file")
        with mock.patch.object(stages.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(ValueError, r"libatomic\.so\.1.*install libatomic1 and libLLVM"):
                stages.require_ray_tracing()

    def test_a_runner_without_an_llvm_backend_says_so(self):
        absent = subprocess.CompletedProcess([], 3, "", "")
        with mock.patch.object(stages.subprocess, "run", return_value=absent):
            with self.assertRaisesRegex(ValueError, "no LLVM backend"):
                stages.require_ray_tracing()

    def test_a_runner_with_the_backend_passes(self):
        with mock.patch.object(stages.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")):
            self.assertIsNone(stages.require_ray_tracing())

    def test_the_check_asks_drjit_for_the_llvm_backend(self):
        self.assertIn("has_backend(dr.JitBackend.LLVM)", stages.RAY_TRACING_CHECK)


class PrepareSchedulingTests(unittest.TestCase):
    def test_both_prepares_submitted_before_any_variant_is_consumed(self):
        from concurrent.futures import Future
        from r3d.fitted_variant import prepare
        jobs = ['lite-job', 'full-job']
        futures = [Future(), Future()]
        executor = mock.Mock()
        executor.submit.side_effect = futures
        seen = []
        ready = stages.prepare_variants(executor, [(Path('other.scene.toml'), 'scene', job, Path('work') / job) for job in jobs],
                                        lambda index, job: seen.append((index, job)))
        self.assertEqual(executor.submit.call_count, 2)
        self.assertTrue(all(call.args[0].func is stages.recorded_call and call.args[0].args == (prepare,)
                            for call in executor.submit.call_args_list))
        self.assertEqual(seen, [])
        futures[0].set_result(None)
        self.assertEqual(seen, [(0, 'lite-job')])
        self.assertTrue(ready[0].done())
        self.assertFalse(ready[1].done())
        futures[1].set_result(None)
        self.assertEqual(seen, [(0, 'lite-job'), (1, 'full-job')])

    def test_prepare_failure_reaches_ordered_consumer(self):
        from concurrent.futures import Future
        futures = [Future(), Future()]
        executor = mock.Mock()
        executor.submit.side_effect = futures
        callback = mock.Mock()
        ready = stages.prepare_variants(executor, [(Path('other.scene.toml'), None, None, Path('work'))] * 2, callback)
        futures[0].set_exception(RuntimeError('prepare failure'))
        with self.assertRaisesRegex(RuntimeError, 'prepare failure'):
            ready[0].result()
        callback.assert_not_called()

    def test_memory_guard_is_only_called_at_stage_start(self):
        import ast
        tree = ast.parse(PATH.read_text())
        callers = [node.name for node in tree.body if isinstance(node, ast.FunctionDef)
                   for call in ast.walk(node) if isinstance(call, ast.Call)
                   and isinstance(call.func, ast.Name) and call.func.id == 'memory_guard']
        self.assertEqual(callers, ['gpu'])


class LightStudyTests(unittest.TestCase):
    SCENE = stages.ROOT / "launcher/demo/sponza/sponza.scene.toml"

    def test_each_look_scene_loads_and_fits_the_showcase(self):
        from r3d.fitted_variant import placed_variant
        from r3d.import_settings import load_scene
        showcase = load_scene(self.SCENE).renderers[0].object.name
        with tempfile.TemporaryDirectory() as directory:
            for look in ("smooth", "direct", "ao-occluded"):
                with self.subTest(look=look):
                    path = stages.write_look_scene(self.SCENE, look, Path(directory) / look)
                    scene = load_scene(path)
                    self.assertEqual(scene.renderers[0].object.name, showcase)
                    self.assertTrue(placed_variant(scene, showcase).renderer.fit)
                    self.assertEqual(scene.bake.ao is not None, look == "ao-occluded")

    def test_a_fit_is_submitted_only_after_its_prepare_and_its_failure_reaches_the_sheet(self):
        from concurrent.futures import Future
        from r3d.fitted_variant import fit_point, prepare
        submitted = []

        def submit(function, *args, **kwargs):
            submitted.append((getattr(function, "args", (function,))[0], Future()))
            return submitted[-1][1]
        executor = mock.Mock()
        executor.submit.side_effect = submit
        with tempfile.TemporaryDirectory() as directory:
            studies, ready, fits = stages.submit_light_fits(executor, self.SCENE, Path(directory))
        self.assertEqual(len(submitted), len(studies))
        self.assertTrue(all(function is prepare for function, _ in submitted))
        self.assertEqual(fits, {})
        submitted[0][1].set_result((None, []))
        self.assertIs(submitted[-1][0], fit_point)
        self.assertEqual(list(fits), [studies[0][0]])
        submitted[1][1].set_exception(RuntimeError("prepare failure"))
        with self.assertRaisesRegex(RuntimeError, "prepare failure"):
            ready[1].result()
        self.assertNotIn(studies[1][0], fits)


class SmokeAdmissionTests(unittest.TestCase):
    def test_smoke_submits_measured_fit_reservation(self):
        from types import SimpleNamespace
        from r3d import fitted_variant, import_settings
        from r3d.process_budget import FIT_BYTES
        job = SimpleNamespace(renderer=SimpleNamespace(fit=SimpleNamespace(held_out_every_ms=1)))
        executor = mock.Mock()
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(import_settings, 'load_scene', return_value=object()), \
                mock.patch.object(fitted_variant, 'placed_variant', return_value=job), \
                mock.patch.object(stages, 'gpu_inputs', return_value={}):
            stages._gpu(SimpleNamespace(smoke=True, scene=Path("other.scene.toml"), object="hall"), Path(directory), Path(directory), executor)
        fit_call = executor.submit.call_args_list[1]
        self.assertIs(fit_call.args[0], fitted_variant.fit_point)
        self.assertEqual(fit_call.kwargs['estimates'], FIT_BYTES)


class FullGpuSchedulingTests(unittest.TestCase):
    def test_fit_arguments_resume_and_order_with_reverse_completion(self):
        from concurrent.futures import Future
        from functools import partial
        from types import SimpleNamespace as NS
        from r3d import fitted_variant as fitted, import_settings, bake_fidelity, cost_model
        jobs = [NS(renderer=NS(fit=NS(held_out_every_ms=1000, normal_weight=.3, budget=6000),
                               variant=NS(triangles=10000))) for _ in range(3)]
        calls, consumed, completed, prepare_futures, fit_futures = [], [], [], [], []

        class PrepareFuture(Future):
            def add_done_callback(self, callback):
                super().add_done_callback(callback)
                if len(prepare_futures) == 3 and all(f._done_callbacks for f in prepare_futures):
                    for future in reversed(prepare_futures):
                        future.set_result((None, [dict(step="prepare fixture", wall_s=1, triangles_in=4, triangles_out=3, peak_ram_bytes=None, peak_vram_bytes=None)]))
                    for future, directory, target in reversed(fit_futures):
                        completed.append(directory.name)
                        result = {'mesh': str(target)}
                        future.set_result((result, [dict(step='fit fixture', wall_s=2, triangles_in=3, triangles_out=2, peak_ram_bytes=1, peak_vram_bytes=2)])
                                          if directory.name.startswith('fit-') else result)

        class Executor:
            def submit(self, function, *args, **kwargs):
                future = Future()
                owner = function.func if isinstance(function, partial) else function
                if owner is stages.recorded_call:
                    owner = function.args[0]
                from r3d.process_budget import PREPARE_BYTES, FIT_BYTES, BAKE_BYTES, MEASURE_BYTES
                estimates = {fitted.prepare: PREPARE_BYTES, fitted.fit_point: FIT_BYTES,
                             stages.bake_worker: BAKE_BYTES, stages.measure_worker: MEASURE_BYTES}
                if kwargs.get('estimates') != estimates[owner]:
                    raise AssertionError(f"wrong estimate for {owner.__name__}: {kwargs}")
                calls.append((owner, args, kwargs, function))
                if owner is fitted.prepare:
                    future = PrepareFuture()
                    prepare_futures.append(future)
                elif owner is fitted.fit_point:
                    directory = args[1]
                    target = args[7] if len(args) > 7 else directory.parent / f'{directory.name}.mesh'
                    fit_futures.append((future, directory, target))
                elif owner is stages.bake_worker:
                    future.set_result(((Path('baked.mesh'), Path('culled.mesh')), [dict(step='bake fixture', wall_s=3, triangles_in=5, triangles_out=4, peak_ram_bytes=None, peak_vram_bytes=None)]))
                elif owner is stages.measure_worker:
                    label = args[0]
                    consumed.append(label)
                    future.set_result(((label, 1, '1.5', '2.5', '3.5', '4.5', '99.5'), Path(f'{label}.avi'),
                                       {'triangles': 1, 'mean_delta_e': 0., 'predicted_ms': 0.}))
                else:
                    raise AssertionError(owner)
                return future

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out = root / 'out'
            (out / 'tables').mkdir(parents=True)
            (out / 'render/gpu').mkdir(parents=True)
            work = root / 'work/full-' / bake.digest({})
            resume = work / 'budget-4000-cost-0/result.json'
            resume.parent.mkdir(parents=True)
            resume.write_text(json.dumps({'budget': 4000, 'cost_weight': 0., 'triangles': 1,
                                          'mean_delta_e': 0., 'predicted_ms': 0.}))
            before = resume.read_bytes()
            with mock.patch.object(import_settings, 'load_scene', return_value=object()), \
                    mock.patch.object(fitted, 'placed_variant', side_effect=jobs) as placed, \
                    mock.patch.object(bake_fidelity, 'build_host', return_value='host') as build_host, \
                    mock.patch.object(cost_model, 'load', return_value=([],)), \
                    mock.patch.object(stages, 'gpu_inputs', return_value={}), \
                    mock.patch.object(stages, 'run') as run, mock.patch.object(stages, 'apply_tables'), \
                    mock.patch.object(fitted, 'plot_pareto'), mock.patch.object(stages, 'machine_table', return_value='machine fixture'),                     mock.patch.object(stages, 'submit_light_fits', return_value='light fits') as light,                     mock.patch.object(stages, 'light_sheets') as sheets:
                stages._gpu(NS(smoke=False, scene=Path("other.scene.toml"), object="hall"), out, root / 'work', Executor())
            light.assert_called_once_with(mock.ANY, Path('other.scene.toml'), work)
            self.assertEqual(sheets.call_args.args[0], 'light fits')
            build_host.assert_called_once_with(stages.HOST_SCRIPT, work / 'host', Path('other.scene.toml'))
            self.assertEqual(placed.call_args_list, [mock.call(mock.ANY, "hall_fitted"),
                                                    mock.call(mock.ANY, "hall_fitted_full"),
                                                    mock.call(mock.ANY, "hall_flat_fitted")])
            self.assertEqual([args[4] for owner, args, _, _ in calls if owner is stages.bake_worker],
                             ["hall_lite", "hall", "hall_flat"])
            self.assertEqual({path.name for path in (out / "tables").iterdir()},
                             {"other-normal.md", "other-budget.md", "other-gpu.md", "other-flat-fit.md", "bake-machine.md", "bake-steps.md"})
            table = (out / 'tables/bake-steps.md').read_text()
            for prefix in ('lite', 'full', 'flat'):
                self.assertIn(f"| {prefix}-GI-bake | bake fixture | 3.000 | 5 | 4 |", table)
                self.assertIn(f"| {prefix}-fit-start | prepare fixture | 1.000 | 4 | 3 |", table)
                self.assertIn(f"| {prefix}-GI-fit | fit fixture | 2.000 | 3 | 2 |", table)
            measured = [function for owner, args, kwargs, function in calls
                        if owner is fitted.fit_point and args[1].name.startswith('fit-')]
            self.assertEqual(len(measured), 3)
            self.assertTrue(all(isinstance(function, partial) and function.func is stages.recorded_call
                                and function.args == (fitted.fit_point,) for function in measured))
            expected_rows = ['lite-GI-bake', 'lite-GI-fit', 'normal-0', 'normal-0.1', 'normal-0.3',
                             'budget-4000-cost-0.1', 'budget-6000-cost-0', 'budget-6000-cost-0.1',
                             'full-GI-bake', 'full-path-culled', 'full-GI-fit', 'flat-GI-bake', 'flat-GI-fit']
            self.assertEqual(consumed, expected_rows)
            measurements = json.loads((out / 'measurements.json').read_text())
            self.assertEqual([row[0] for row in measurements['rows']], expected_rows)
            self.assertEqual([(row['budget'], row['cost_weight']) for row in measurements['sweep']],
                             [(4000, 0.), (4000, .1), (6000, 0.), (6000, .1), (6000, 0.)])
            self.assertEqual(resume.read_bytes(), before)
            fit_calls = [call for call in calls if call[0] is fitted.fit_point]
            self.assertEqual(completed, [call[1][1].name for call in reversed(fit_calls)])
            arguments = []
            for _, args, kwargs, function in fit_calls:
                point, point_dir = args[:2]
                job = function.keywords['job'] if isinstance(function, partial) and 'job' in function.keywords else args[4]
                target = args[7].name if len(args) > 7 else f'{point_dir.name}.mesh'
                arguments.append((point_dir.name, target, point.get('budget'), point.get('cost_weight', 0.),
                                  job.renderer.fit.normal_weight))
            self.assertCountEqual(arguments, [
                ('fit-lite', 'lite.mesh', None, 0., .3), ('fit-full', 'full.mesh', None, 0., .3),
                ('fit-flat', 'flat.mesh', None, 0., .3),
                ('normal-0', 'normal-0.mesh', None, 0., 0.), ('normal-0.1', 'normal-0.1.mesh', None, 0., .1),
                ('budget-4000-cost-0.1', 'budget-4000-cost-0.1.mesh', 4000, .1, .3),
                ('budget-6000-cost-0.1', 'budget-6000-cost-0.1.mesh', 6000, .1, .3)])
            baked_names = {call[1][3]: call[1][4] for call in calls if call[0] is stages.bake_worker}
            self.assertEqual(baked_names, {'lite': 'hall_lite', 'full': 'hall', 'flat': 'hall_flat'})
            flat_sheet = [call.args[0] for call in run.call_args_list if call.args[1].name == 'sheet-flat.log']
            self.assertEqual(len(flat_sheet), 1)
            command = [str(word) for word in flat_sheet[0]]
            columns = [(command[i + 1], command[i + 2]) for i, word in enumerate(command) if word == '--bake']
            self.assertEqual(columns, [('flat bake', 'flat-GI-bake.avi'), ('lite fit', 'lite-GI-fit.avi'),
                                       ('flat fit', 'flat-GI-fit.avi')])
            table = (out / 'tables/other-flat-fit.md').read_text().splitlines()
            self.assertEqual([line.split(' | ')[0].strip('| ') for line in table[2:5]],
                             ['flat-GI-bake', 'lite-GI-fit', 'flat-GI-fit'], "the sheet's column order")
            self.assertNotIn('Predicted', table[0], "the cost model cannot price flat shading")
            cells = [line.count('|') for line in table[:5]]
            self.assertEqual(len(set(cells)), 1, "a row has more cells than the header")
            self.assertNotIn('99.5', ' '.join(table), "a predicted time reached the flat table")
            self.assertEqual(sum(line.startswith('| ') for line in table), 4, "a header and exactly three rows")
            self.assertTrue(all(kwargs.get('priority') for owner, _, kwargs, _ in calls
                                if owner in (stages.bake_worker, stages.measure_worker)))


@unittest.skipUnless(sys.platform == "linux", "the GPU stage and its fingerprint run on Linux")
class GpuInputsTests(unittest.TestCase):
    def test_the_fingerprint_covers_what_the_stage_runs_without_numeric_dependencies(self):
        """A hosted runner with no numpy or Pillow takes it; it names the scripts the stage starts, the
        files its code names, what the host renderer compiles and the scene's locked bakes."""
        code = """
import importlib.util, json, sys
from pathlib import Path
sys.modules.update(numpy=None, PIL=None)
spec = importlib.util.spec_from_file_location("doc_stages", sys.argv[1])
stages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stages)
from bake import bake
inputs = stages.gpu_inputs(stages.ROOT / "launcher/demo/sponza/sponza.scene.toml", "atrium")
assert bake.digest(inputs) == bake.digest(
    stages.gpu_inputs(stages.ROOT / "launcher/demo/sponza/sponza.scene.toml", "atrium"))
print(json.dumps(inputs))
"""
        result = subprocess.run([sys.executable, "-c", code, str(PATH)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        inputs = json.loads(result.stdout)
        for name in ("code launcher/tools/render/doc_stages.py", "code launcher/tools/render/render_compare.py",
                     "code launcher/tools/r3d/fitted_variant.py", "file launcher/tools/r3d/board_cost_weights.txt",
                     "host launcher/main/render/raster.c", "host launcher/tools/render/scene_viewer.c",
                     "native launcher/main/display/display.h",
                     "bake sponza.atrium.mesh", "bake sponza.atrium_fitted.mesh"):
            self.assertIn(name, inputs)
        self.assertNotIn("code launcher/tools/bake/bake.py", inputs)
        self.assertEqual([name for name in inputs if "/apps/" in name], [])


if __name__ == "__main__":
    unittest.main()
