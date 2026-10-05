"""Tests for launcher/test/qemu_run.py's verdict on a finished console log:
the part of a QEMU run that decides its exit status, with no QEMU needed.

    python -m unittest discover -s launcher/test/tests
"""
import base64
import json
import os
import struct
import sys
import tempfile
import unittest
import unittest.mock
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


class FakeConsole:
    def __init__(self, lines):
        self.sent, self._lines = [], lines

    def send(self, line):
        self.sent.append(line)

    def lines(self):
        return iter(self._lines)


class RunSuiteTest(unittest.TestCase):
    """The completion line is read field by field, the way the board tooling
    reads it: what follows found= says nothing about whether it was found."""

    def run_suite(self, *lines):
        return qemu_run.run_suite(FakeConsole(list(lines)), "run_sand_suite")

    def test_a_suite_that_ran_passes(self):
        self.assertTrue(self.run_suite(
            "RUNSUITE_COMPLETE name=run_sand_suite found=1 selected=12 unmatched=0"))

    def test_an_image_that_prints_no_selection_still_passes(self):
        self.assertTrue(self.run_suite("RUNSUITE_COMPLETE name=run_sand_suite found=1"))

    def test_a_suite_not_registered_fails(self):
        self.assertFalse(self.run_suite(
            "RUNSUITE_COMPLETE name=run_sand_suite found=0 selected=0 unmatched=0"))

    def test_a_suite_that_selected_nothing_fails(self):
        self.assertFalse(self.run_suite(
            "RUNSUITE_COMPLETE name=run_sand_suite found=1 selected=0 unmatched=1"))

    def test_another_suites_completion_is_not_this_ones(self):
        self.assertFalse(self.run_suite(
            "RUNSUITE_COMPLETE name=run_sand_suite_perf found=1 selected=3 unmatched=0"))


if __name__ == "__main__":
    unittest.main()


class DriveFrameTest(unittest.TestCase):
    """A tap or swipe takes pixels of the default screenshot, as `autana tap`
    does; a touch stays the raw panel-frame level."""

    def run_action(self, action):
        console = FakeConsole([])
        with unittest.mock.patch.object(qemu_run.time, "sleep"):
            ok = qemu_run.run_action(console, action)
        return ok, console.sent

    def test_a_tap_goes_in_at_the_panel_point_of_the_screenshot_pixel(self):
        ok, sent = self.run_action("tap 10 20")
        self.assertTrue(ok)
        self.assertEqual(sent, ["TOUCH down 347 10", "TOUCH up 347 10"])

    def test_a_swipe_turns_both_ends(self):
        ok, sent = self.run_action("swipe 0 0 447 367")
        self.assertTrue(ok)
        self.assertEqual(sent[0], "TOUCH down 367 0")
        self.assertEqual(sent[-1], "TOUCH up 0 447")

    def test_a_touch_stays_in_panel_pixels(self):
        ok, sent = self.run_action("touch down 10 20")
        self.assertTrue(ok)
        self.assertEqual(sent, ["TOUCH down 10 20"])

    def test_a_tap_outside_the_screenshot_is_not_an_action(self):
        with unittest.mock.patch("builtins.print"):
            ok, sent = self.run_action("tap 448 0")
        self.assertFalse(ok)
        self.assertEqual(sent, [])


class ScreenshotTest(unittest.TestCase):
    def test_writes_the_default_view_and_records_its_turn(self):
        # A 2 x 3 frame, BGR bottom-up rows padded to 4 bytes.
        rows = [bytes((1, 2, 3, 4, 5, 6, 0, 0)), bytes((7, 8, 9, 10, 11, 12, 0, 0)),
                bytes((13, 14, 15, 16, 17, 18, 0, 0))]
        pixels = b"".join(rows)
        bmp = (b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54)
               + struct.pack("<IiiHHIIiiII", 40, 2, 3, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels)
        lines = ["SCREENSHOT_BEGIN size=%d" % len(bmp),
                 "SCREENSHOT_DATA:" + base64.b64encode(bmp).decode("ascii"),
                 'SCREENSHOT_STATE:{"orientation_quarter": 2}', "SCREENSHOT_END"]
        with tempfile.TemporaryDirectory() as directory, unittest.mock.patch("builtins.print"):
            out = os.path.join(directory, "shot.png")
            self.assertTrue(qemu_run.take_screenshot(FakeConsole(lines), out))
            with open(out, "rb") as fh:
                self.assertEqual(struct.unpack_from(">II", fh.read(), 16), (3, 2))
            with open(os.path.join(directory, "shot.json")) as fh:
                state = json.load(fh)
        self.assertEqual(state, {"orientation_quarter": 2, "image_turn_quarter": 3})
