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

MEMINFO = Path("/proc/meminfo")
POWERSHELL = Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")


class DocStagesTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tmp = Path(directory.name)

    def test_current_stamp_needs_no_numeric_dependencies(self):
        launcher = self.tmp / "launcher"
        launcher.mkdir()
        (launcher / "m.import.toml").write_text('[source]\npath = "m.obj"\ncredit = "c"\n'
                                               '[output]\ndirectory = "."\nname = "mesh"\n')
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
            stages.capture_rows(capture, "abcdef123456")

    def test_capture_uses_existing_perf_parser(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("abcdef123456-diag\nlite both cores: mean 42000us\n")
        self.assertEqual(stages.capture_rows(capture, "abcdef123456"), {"lite": 42.0})

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
            stages.board_means([capture], "abcdef123456")

    def test_board_pose_rows_require_complete_unique_samples(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("atrium t=    0s clusters= 4 tris=5 | both cores: frame 51000us\n"
                           "atrium_lite t=    0s clusters= 4 tris=5 | both cores: frame 42000us\n")
        expected = [("sponza", 0), ("lite", 0)]
        self.assertEqual(stages.board_frames([capture], expected), {("sponza", 0): 51.0, ("lite", 0): 42.0})
        with self.assertRaisesRegex(ValueError, "exactly once"):
            stages.board_frames([capture], [*expected, ("sponza", 5)])
        capture.write_text(capture.read_text() + capture.read_text().splitlines()[0] + "\n")
        with self.assertRaisesRegex(ValueError, "exactly once"):
            stages.board_frames([capture], expected)

    def test_gpu_check_requires_saved_full_run(self):
        with self.assertRaisesRegex(ValueError, "no full run"):
            stages.check_gpu(self.tmp)

    def test_board_rejects_a_different_render_size(self):
        capture = self.tmp / "capture.txt"
        capture.write_text("=== atrium FRAME COST (10 tris, 20 verts, 1 clusters, rendered 368x448) ===\n")
        with self.assertRaisesRegex(ValueError, "render size"):
            stages.capture_viewport(capture, (184, 224))

    def test_gpu_check_rejects_missing_saved_artifact(self):
        (self.tmp / "source.json").write_text(json.dumps({"source": "stamp", "files": {"tables/score.md": "0" * 64}}))
        with mock.patch.object(stages, "current_stamp", lambda: "stamp"):
            with self.assertRaisesRegex(ValueError, "missing or altered"):
                stages.check_gpu(self.tmp)

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
            self.assertEqual(stages.main(["--stage", "gpu", "--smoke"]), 0)
        self.assertEqual(result.read_text(), "full run")


class OcclusionSceneTests(unittest.TestCase):
    def test_the_scratch_scene_raises_the_ambient_adds_occlusion_and_keeps_what_it_reads(self):
        from r3d.import_settings import load_scene
        with tempfile.TemporaryDirectory() as directory:
            path = stages.write_ao_scene(Path(directory) / "ao-scene")
            original, scratch = load_scene(stages.SCENE), load_scene(path)
            ambient = [light for light in scratch.lights if light["type"] == "ambient"]
            self.assertEqual(ambient[0]["intensity"], float(stages.AO_AMBIENT))
            self.assertIsNone(original.bake.ao)
            self.assertEqual((scratch.bake.ao.distance, scratch.bake.ao.rays), (80.0, 32))
            self.assertEqual([job.asset_name for job in scratch.renderers], [job.asset_name for job in original.renderers])
            self.assertTrue(scratch.renderers[0].settings.source["path"].is_absolute())
            self.assertEqual(scratch.renderers[0].settings.source["path"], original.renderers[0].settings.source["path"])
            self.assertTrue((Path(directory) / "ao-scene" / "flythrough_tracks_generated.c").is_file())

    def test_a_long_scene_without_the_ambient_intensity_is_rejected_at_once(self):
        with tempfile.TemporaryDirectory() as directory:
            long = Path(directory) / "meshes"
            long.mkdir()
            (long / "sponza.import.toml").write_text('[source]\npath = "x.obj"\n')
            (long / "sponza.scene.toml").write_text("[ambient]\n" + "color = [1.0, 1.0, 1.0]\n" * 20000)
            with mock.patch.object(stages, "SCENE", long / "sponza.scene.toml"):
                with self.assertRaisesRegex(ValueError, r"no \[ambient\] intensity"):
                    stages.write_ao_scene(Path(directory) / "out")

    def test_a_scene_without_the_lines_to_edit_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            stripped = Path(directory) / "meshes"
            stripped.mkdir()
            (stripped / "sponza.import.toml").write_text('[source]\npath = "x.obj"\n')
            (stripped / "sponza.scene.toml").write_text("tonemap_white = 0.3\n")
            with mock.patch.object(stages, "SCENE", stripped / "sponza.scene.toml"):
                with self.assertRaisesRegex(ValueError, r"no \[ambient\] intensity"):
                    stages.write_ao_scene(Path(directory) / "out")


class PrepareSchedulingTests(unittest.TestCase):
    def test_both_prepares_submitted_before_any_variant_is_consumed(self):
        from concurrent.futures import Future
        from r3d.fitted_variant import prepare
        jobs = ['lite-job', 'full-job']
        futures = [Future(), Future()]
        executor = mock.Mock()
        executor.submit.side_effect = futures
        seen = []
        ready = stages.prepare_variants(executor, 'scene', jobs, Path('work'),
                                        lambda index, job: seen.append((index, job)))
        self.assertEqual(executor.submit.call_count, 2)
        self.assertTrue(all(call.args[0] is prepare for call in executor.submit.call_args_list))
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
        ready = stages.prepare_variants(executor, None, [None, None], Path('work'), callback)
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
                mock.patch.object(stages, 'current_stamp', return_value='smoke-stamp'):
            stages._gpu(SimpleNamespace(smoke=True), Path(directory), Path(directory), executor)
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
                if len(prepare_futures) == 2 and all(f._done_callbacks for f in prepare_futures):
                    for future in reversed(prepare_futures):
                        future.set_result(None)
                    for future, directory, target in reversed(fit_futures):
                        completed.append(directory.name)
                        future.set_result({'mesh': str(target)})

        class Executor:
            def submit(self, function, *args, **kwargs):
                future = Future()
                owner = function.func if isinstance(function, partial) else function
                from r3d.process_budget import PREPARE_BYTES, FIT_BYTES, BAKE_BYTES, MEASURE_BYTES
                estimates = {fitted.prepare: PREPARE_BYTES, fitted.fit_point: FIT_BYTES,
                             stages.bake_worker: BAKE_BYTES, stages.measure_worker: MEASURE_BYTES}
                if kwargs.get('estimates') != estimates[owner]:
                    raise AssertionError(f"wrong estimate for {owner.__name__}: {kwargs}")
                calls.append((owner, args, kwargs, function))
                if owner is fitted.prepare and args[3].name == 'inputs-ao':
                    future.set_result(None)
                elif owner is fitted.prepare:
                    future = PrepareFuture()
                    prepare_futures.append(future)
                elif owner is fitted.fit_point:
                    directory = args[1]
                    target = args[7] if len(args) > 7 else directory.parent / f'{directory.name}.mesh'
                    if directory.name == 'fit-ao':
                        future.set_result({'mesh': str(target)})
                    else:
                        fit_futures.append((future, directory, target))
                elif owner is stages.bake_worker:
                    future.set_result((Path('baked.mesh'), Path('culled.mesh')))
                elif owner is stages.measure_worker:
                    label = args[0]
                    consumed.append(label)
                    future.set_result(((label, 1, '0', '0', '0', '0', '0'), Path('frames.avi'),
                                       {'triangles': 1, 'mean_delta_e': 0., 'predicted_ms': 0.}))
                else:
                    raise AssertionError(owner)
                return future

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out = root / 'out'
            (out / 'tables').mkdir(parents=True)
            (out / 'render/gpu').mkdir(parents=True)
            work = root / 'work/full-/stamp'
            resume = work / 'budget-4000-cost-0/result.json'
            resume.parent.mkdir(parents=True)
            resume.write_text(json.dumps({'budget': 4000, 'cost_weight': 0., 'triangles': 1,
                                          'mean_delta_e': 0., 'predicted_ms': 0.}))
            before = resume.read_bytes()
            with mock.patch.object(import_settings, 'load_scene', return_value=object()), \
                    mock.patch.object(fitted, 'placed_variant', side_effect=jobs), \
                    mock.patch.object(bake_fidelity, 'build_host', return_value='host'), \
                    mock.patch.object(cost_model, 'load', return_value=([],)), \
                    mock.patch.object(stages, 'current_stamp', return_value='stamp'), \
                    mock.patch.object(stages, 'write_ao_scene', return_value=root / 'ao.scene.toml'), \
                    mock.patch.object(stages, 'run'), mock.patch.object(stages, 'apply_tables'), \
                    mock.patch.object(fitted, 'plot_pareto'):
                stages._gpu(NS(smoke=False), out, root / 'work', Executor())
            expected_rows = ['lite-GI-bake', 'lite-GI-fit', 'normal-0', 'normal-0.1', 'normal-0.3',
                             'budget-4000-cost-0.1', 'budget-6000-cost-0', 'budget-6000-cost-0.1',
                             'full-GI-bake', 'full-path-culled', 'full-GI-fit', 'ao-bake', 'ao-fit']
            self.assertEqual(consumed, expected_rows)
            measurements = json.loads((out / 'measurements.json').read_text())
            self.assertEqual([row[0] for row in measurements['rows']], expected_rows)
            self.assertEqual([(row['budget'], row['cost_weight']) for row in measurements['sweep']],
                             [(4000, 0.), (4000, .1), (6000, 0.), (6000, .1), (6000, 0.)])
            self.assertEqual(resume.read_bytes(), before)
            fit_calls = [call for call in calls if call[0] is fitted.fit_point and call[1][1].name != 'fit-ao']
            self.assertEqual(completed, [call[1][1].name for call in reversed(fit_calls)])
            arguments = []
            for _, args, kwargs, function in fit_calls:
                point, point_dir = args[:2]
                job = function.keywords['job'] if isinstance(function, partial) else args[4]
                target = args[7].name if len(args) > 7 else f'{point_dir.name}.mesh'
                arguments.append((point_dir.name, target, point.get('budget'), point.get('cost_weight', 0.),
                                  job.renderer.fit.normal_weight))
            self.assertCountEqual(arguments, [
                ('fit-lite', 'lite.mesh', None, 0., .3), ('fit-full', 'full.mesh', None, 0., .3),
                ('normal-0', 'normal-0.mesh', None, 0., 0.), ('normal-0.1', 'normal-0.1.mesh', None, 0., .1),
                ('budget-4000-cost-0.1', 'budget-4000-cost-0.1.mesh', 4000, .1, .3),
                ('budget-6000-cost-0.1', 'budget-6000-cost-0.1.mesh', 6000, .1, .3)])
            self.assertTrue((out / 'tables/sponza-gpu-ao.md').read_text().startswith('| Mesh |'))
            self.assertTrue(all(kwargs.get('priority') for owner, _, kwargs, _ in calls
                                if owner in (stages.bake_worker, stages.measure_worker)))


if __name__ == "__main__":
    unittest.main()
