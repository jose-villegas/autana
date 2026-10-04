"""The GPU and board doc stages check what they are given before they publish."""
import importlib.util
import json
import tempfile
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


if __name__ == "__main__":
    unittest.main()


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
