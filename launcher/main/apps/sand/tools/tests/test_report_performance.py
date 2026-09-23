"""Regression tests for this app's frame-budget reporter.

    python -m unittest discover -s launcher/main/apps/sand/tools/tests

A capture that measured nothing must FAIL rather than turn into a clean-
looking table - that is what these pin down, since the device itself cannot
be asked for an empty capture on demand.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
REPORTER = TOOLS / "report_performance.py"
SUITE = TOOLS.parents[0] / "tests" / "suite_sand_perf.c"

BOOT = "ESP-ROM:esp32s3-20210327\nI (31) boot: ESP-IDF v5.5\n"
COMPLETE = "I (3000) selftest: SELFTEST_COMPLETE failures=0 elapsed_ms=380\n"
BUDGETED = "test_a_full_size_step_fits_in_the_frame_budget"
MEASURED = (
    f"I (1200) device_tests: sand_step on 184x224 with 9000 grains: 5210 us per step\n"
    f"suite_sand_perf.c:346:{BUDGETED}:PASS\n"
    f"TEST_TIME name={BUDGETED} elapsed_ms=140\n"
)


class ReportPerformanceTest(unittest.TestCase):
    def run_reporter(self, capture_text):
        handle, capture = tempfile.mkstemp(suffix=".txt")
        os.close(handle)
        pathlib.Path(capture).write_text(capture_text, encoding="utf-8")
        self.addCleanup(os.unlink, capture)
        handle, out = tempfile.mkstemp(suffix=".md")
        os.close(handle)
        self.addCleanup(os.unlink, out)
        done = subprocess.run(
            [sys.executable, str(REPORTER), capture, out, "--source", str(SUITE)],
            capture_output=True, text=True)
        return done, pathlib.Path(out).read_text(encoding="utf-8")

    def test_it_refuses_a_capture_with_no_results(self):
        done, _ = self.run_reporter(BOOT + "I (4500) shell: 30.0 fps\n" + COMPLETE)
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("no test results", done.stderr)

    def test_a_measured_budget_test_becomes_a_row(self):
        done, report = self.run_reporter(BOOT + MEASURED + COMPLETE)
        self.assertEqual(done.returncode, 0, done.stderr)
        # The budget comes from the suite source, never from this test, so
        # retuning it in the source cannot make this fail. Five columns is
        # what picks the budget table out of the wall-time one below it,
        # which names the same test in three.
        rows = [line.split("|") for line in report.splitlines()
                if line.startswith(f"| `{BUDGETED}`")]
        budget_rows = [row for row in rows if len(row) == 7]
        self.assertEqual(len(budget_rows), 1, report)
        self.assertEqual(budget_rows[0][3].strip(), "5210")

    def test_a_budget_test_that_logged_nothing_is_not_a_row(self):
        capture = (BOOT
                   + f"suite_sand_perf.c:346:{BUDGETED}:FAIL: the real grid must fit\n"
                   + COMPLETE)
        done, report = self.run_reporter(capture)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn(f"| `{BUDGETED}` |", report)
        self.assertIn("logged no measurement", report)


if __name__ == "__main__":
    unittest.main()
