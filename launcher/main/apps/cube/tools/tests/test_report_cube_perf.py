"""Regression tests for this app's perf reporter.

    python -m unittest discover -s launcher/main/apps/cube/tools/tests

A capture with no run in it must FAIL rather than turn into a report that is
a timestamp over an empty table.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPORTER = pathlib.Path(__file__).resolve().parents[1] / "report_cube_perf.py"

BOOT = "ESP-ROM:esp32s3-20210327\nI (31) boot: ESP-IDF v5.5\n"
RUN = (
    "I (11692) cube_perf: === CUBE PERF hud_on_partial_on_interlace_off "
    "(274 frames over 10s) ===\n"
    "I (11694) cube_perf: Total:   min=30000us max=40000us avg=36000us "
    "med=35800us p95=39000us (27.8/27.9/25.6 fps)\n"
)


class ReportCubePerfTest(unittest.TestCase):
    def run_reporter(self, capture_text):
        handle, capture = tempfile.mkstemp(suffix=".txt")
        os.close(handle)
        pathlib.Path(capture).write_text(capture_text, encoding="utf-8")
        self.addCleanup(os.unlink, capture)
        handle, out = tempfile.mkstemp(suffix=".md")
        os.close(handle)
        self.addCleanup(os.unlink, out)
        done = subprocess.run([sys.executable, str(REPORTER), capture, out],
                              capture_output=True, text=True)
        return done, pathlib.Path(out).read_text(encoding="utf-8")

    def test_it_refuses_a_capture_with_no_run(self):
        done, _ = self.run_reporter(BOOT + "I (4500) shell: 30.0 fps\n")
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("CUBE PERF", done.stderr)

    def test_a_run_becomes_a_table(self):
        done, report = self.run_reporter(BOOT + RUN)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("hud_on_partial_on_interlace_off", report)
        self.assertIn("36000", report)


if __name__ == "__main__":
    unittest.main()
