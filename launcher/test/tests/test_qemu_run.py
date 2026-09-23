"""Tests for launcher/test/qemu_run.py's verdict on a finished console log -
the part of a QEMU run that decides its exit status, with no QEMU needed.

    python -m unittest discover -s launcher/test/tests
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import qemu_run  # noqa: E402

PASSING = (
    "/src/suite_gfx.c:40:test_fill:PASS\n"
    "/src/suite_gfx.c:52:test_blit:PASS\n"
)
FAILING = "/src/suite_gfx.c:61:test_clip:FAIL: Expected 3 Was 4\n"
IGNORED = "/src/suite_sand_perf.c:1953:test_counters:IGNORE: no PMU\n"
COMPLETE = "SELFTEST_COMPLETE tests=4 failures=1\n"


class VerdictTest(unittest.TestCase):
    def verdict(self, log, finished=True, actions=()):
        fd, path = tempfile.mkstemp(suffix=".log")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(log)
        self.addCleanup(os.remove, path)
        return qemu_run.verdict(path, finished, list(actions))

    def test_a_clean_autorun_passes(self):
        self.assertEqual(0, self.verdict(PASSING + IGNORED + COMPLETE))

    def test_a_failing_test_fails_the_run_even_when_it_completes(self):
        self.assertEqual(1, self.verdict(PASSING + FAILING + COMPLETE))

    def test_an_autorun_that_never_completes_fails(self):
        self.assertEqual(1, self.verdict(PASSING, finished=False))

    def test_a_failing_suite_fails_a_driven_run(self):
        self.assertEqual(1, self.verdict(FAILING, actions=[("suite", "x")]))

    def test_a_driven_run_needs_no_sentinel(self):
        self.assertEqual(0, self.verdict(PASSING, actions=[("suite", "x")]))


if __name__ == "__main__":
    unittest.main()
