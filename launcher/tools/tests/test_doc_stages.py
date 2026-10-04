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

    def guard_with(self, wsl_kib, host_bytes):
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
                mock.patch.object(stages.subprocess, "check_output", return_value=f"{host_bytes}\r\n".encode()):
            stages.memory_guard()

    def test_memory_guard_checks_limiting_windows_host(self):
        with self.assertRaisesRegex(ValueError, "Windows host is short by 3.00 GiB"):
            self.guard_with(wsl_kib=16 * 1024 ** 2, host_bytes=3 * 1024 ** 3)

    def test_memory_guard_checks_limiting_wsl(self):
        with self.assertRaisesRegex(ValueError, "WSL is short by 2.00 GiB"):
            self.guard_with(wsl_kib=4 * 1024 ** 2, host_bytes=16 * 1024 ** 3)

    def test_memory_guard_passes_with_enough_on_both_sides(self):
        self.guard_with(wsl_kib=8 * 1024 ** 2, host_bytes=8 * 1024 ** 3)

    def test_smoke_preserves_full_gpu_results(self):
        result = self.tmp / "out/gpu/measurements.json"
        result.parent.mkdir(parents=True)
        result.write_text("full run")
        with mock.patch.object(stages, "RESULTS", self.tmp), mock.patch.object(stages, "gpu", lambda *args: 0):
            self.assertEqual(stages.main(["--stage", "gpu", "--smoke"]), 0)
        self.assertEqual(result.read_text(), "full run")


if __name__ == "__main__":
    unittest.main()
