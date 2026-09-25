"""Regression tests for the capture validator and the shell-owned reporters.

    python -m unittest discover -s launcher/tools/tests

A capture that measured nothing must FAIL rather than turn into a clean-
looking report - that is what these pin down, since the device itself cannot
be asked for an empty capture on demand.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "sweeps"))

import validate_capture  # noqa: E402

BOOT = "ESP-ROM:esp32s3-20210327\nI (31) boot: ESP-IDF v5.5\n"
COMPLETE = "I (3000) selftest: SELFTEST_COMPLETE failures=0 elapsed_ms=380\n"
RESULT = "suite_gfx.c:12:test_band_marks_are_idempotent:PASS\n"


class CaptureFixture(unittest.TestCase):
    def capture(self, text):
        handle, path = tempfile.mkstemp(suffix=".txt")
        os.close(handle)
        pathlib.Path(path).write_text(text, encoding="utf-8")
        self.addCleanup(os.unlink, path)
        return path

    def out_path(self):
        handle, path = tempfile.mkstemp(suffix=".md")
        os.close(handle)
        self.addCleanup(os.unlink, path)
        return path


class ValidateCaptureTest(CaptureFixture):
    def test_a_whole_run_with_results_is_valid(self):
        failures, _ = validate_capture.validate(self.capture(BOOT + RESULT + COMPLETE))
        self.assertEqual(failures, [])

    def test_a_capture_with_no_results_is_rejected(self):
        path = self.capture(BOOT + "I (4500) shell: 30.0 fps\n" + COMPLETE)
        failures, _ = validate_capture.validate(path)
        self.assertTrue(any("no test result lines" in f for f in failures), failures)

    def test_a_declared_sentinel_that_never_appears_is_rejected(self):
        path = self.capture(BOOT + RESULT + COMPLETE)
        failures, _ = validate_capture.validate(path, sentinels=["a line it never printed"])
        self.assertTrue(any("sentinel" in f for f in failures), failures)

    def test_a_declared_sentinel_that_appears_passes(self):
        marker = "device_tests: sand_step on 184x224: 5210 us per step\n"
        path = self.capture(BOOT + marker + RESULT + COMPLETE)
        failures, _ = validate_capture.validate(path, sentinels=[marker.strip()])
        self.assertEqual(failures, [])

    def test_a_runsuite_capture_needs_no_completion_line(self):
        marker = "boot_anim_perf: === BOOT_ANIM PERF curve (now_ms=1, 2 samples) ===\n"
        path = self.capture(BOOT + marker)
        failures, _ = validate_capture.validate(
            path, sentinels=[marker.strip()], require_complete=False)
        self.assertEqual(failures, [])

    def test_a_runsuite_capture_is_not_asked_for_results(self):
        # Its window can close before the suite prints one; the sentinel is
        # what proves the suite ran.
        failures, _ = validate_capture.validate(self.capture(BOOT), require_complete=False)
        self.assertEqual(failures, [])

    def test_a_crash_loop_is_rejected(self):
        failures, _ = validate_capture.validate(self.capture(BOOT + BOOT + RESULT + COMPLETE))
        self.assertTrue(any("boot banners" in f for f in failures), failures)


class ReporterExitCodeTest(CaptureFixture):
    def run_reporter(self, script, capture_text, *extra):
        path = self.capture(capture_text)
        out = self.out_path()
        return subprocess.run(
            [sys.executable, str(TOOLS / script), path, out, *extra],
            capture_output=True, text=True)

    def test_test_results_refuses_a_capture_with_no_results(self):
        done = self.run_reporter("quality/report_test_results.py", BOOT + COMPLETE)
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("no test results", done.stderr)

    def test_test_results_exits_one_for_a_failing_test(self):
        capture = BOOT + "suite_sand.c:70:test_lava_cools:FAIL: expected 5 was 4\n" + COMPLETE
        done = self.run_reporter("quality/report_test_results.py", capture)
        self.assertEqual(done.returncode, 1, done.stderr)

    def test_test_results_exits_zero_when_everything_passed(self):
        done = self.run_reporter("quality/report_test_results.py", BOOT + RESULT + COMPLETE)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_boot_anim_refuses_a_capture_with_no_checkpoint(self):
        done = self.run_reporter("boot_anim/report_boot_anim_perf.py", BOOT + RESULT)
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("BOOT_ANIM PERF", done.stderr)


if __name__ == "__main__":
    unittest.main()
