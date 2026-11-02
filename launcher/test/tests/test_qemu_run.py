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
COMPLETE = "SELFTEST_COMPLETE failures=0 elapsed_ms=42\n"
BOOT = "ESP-ROM:esp32s3-20210327\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\n"
ABORT = "abort() was called at PC 0x40376f2b on core 0\n"


class IcountDiscoveryTest(unittest.TestCase):
    def test_finds_current_ratchet_suites(self):
        suites = qemu_run.icount_suites(Path(__file__).resolve().parents[3])
        self.assertTrue({"run_sand_icount_suite", "run_span_icount_suite"} <= set(suites))

    def test_floor_cost_suite_is_explicit_only(self):
        root = Path(__file__).resolve().parents[3]
        suites = qemu_run.registered_suites(root)
        self.assertIn("run_boot_floor_cost_suite", suites)
        self.assertTrue(suites["run_boot_floor_cost_suite"])
        self.assertFalse(any(name.startswith("run_boot_floor_") for name in qemu_run.icount_suites(root)))

    def test_ignores_other_registrations_and_build_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ("launcher/main", "launcher/test", "launcher/build"):
                (root / folder).mkdir(parents=True)
            (root / "launcher/main/suite_probe.c").write_text(
                "  SUITE_REGISTER_ON_REQUEST(run_z_icount_suite);\n"
                "SUITE_REGISTER(run_autorun_icount_suite);\n"
                "SUITE_REGISTER_ON_REQUEST(run_other_suite);\n"
                "/*\nSUITE_REGISTER_ON_REQUEST(run_comment_icount_suite);\n*/\n"
                '"SUITE_REGISTER_ON_REQUEST(run_string_icount_suite);";\n'
                "void probe(void) { SUITE_REGISTER_ON_REQUEST(run_inline_icount_suite); }\n")
            (root / "launcher/test/suite_duplicate.c").write_text(
                "SUITE_REGISTER_ON_REQUEST(run_a_icount_suite);\n"
                "SUITE_REGISTER_ON_REQUEST(run_a_icount_suite);\n")
            (root / "launcher/build/suite_stale.c").write_text(
                "SUITE_REGISTER_ON_REQUEST(run_stale_icount_suite);\n")
            self.assertEqual(["run_a_icount_suite", "run_z_icount_suite"], qemu_run.icount_suites(root))

    def test_ratchet_options_append_suites_and_enable_instruction_counting(self):
        with unittest.mock.patch.object(qemu_run, "icount_suites", return_value=["run_a_icount_suite", "run_z_icount_suite"]):
            with unittest.mock.patch.object(qemu_run.subprocess, "Popen") as start:
                args = qemu_run.parse_args(["unused", "--suite", "first", "--suite", "second", "--icount-ratchet"])
                self.assertTrue(args.icount)
                self.assertEqual(["first", "second", "run_a_icount_suite", "run_z_icount_suite"], args.suite)
                command = qemu_run.qemu_command(args, "emulator", 8, "flash", "efuse", 1234)
                self.assertEqual(f"shift={qemu_run.ICOUNT_SHIFT},sleep=off", command[command.index("-icount") + 1])
                start.assert_not_called()

    def test_empty_discovery_fails_before_starting_emulator(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "no instruction ratchet suites"):
                qemu_run.icount_suites(directory)
        with unittest.mock.patch.object(qemu_run, "icount_suites", side_effect=ValueError("no ratchet suites")):
            with self.assertRaises(SystemExit) as error:
                qemu_run.main(["unused", "--icount-ratchet"])
            self.assertEqual(2, error.exception.code)


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
        self.assertEqual(1, self.verdict(PASSING + FAILING +
                                        "SELFTEST_COMPLETE failures=1 elapsed_ms=42\n"))

    def test_sentinel_failures_without_fail_lines_fail_the_run(self):
        self.assertEqual(1, self.verdict(PASSING +
                                        "SELFTEST_COMPLETE failures=2 elapsed_ms=42\n"))

    def test_a_failing_case_is_reported_by_its_full_name_and_message(self):
        fd, path = tempfile.mkstemp(suffix=".log")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(":7:test_profile/scene-a/portrait:FAIL: over budget\n:8:test_profile/scene-a/landscape:FAIL\n"
                     "SELFTEST_COMPLETE failures=2 elapsed_ms=42\n")
        self.addCleanup(os.remove, path)
        with unittest.mock.patch("builtins.print") as shown:
            self.assertEqual((True, 2), qemu_run.summarise(path))
        lines = [call.args[0] for call in shown.call_args_list]
        self.assertIn("  FAIL test_profile/scene-a/portrait: over budget", lines)
        self.assertIn("  FAIL test_profile/scene-a/landscape: ", lines)

    def test_a_name_reported_twice_fails_the_run(self):
        self.assertEqual(1, self.verdict(PASSING + PASSING + COMPLETE))

    def test_suite_overflow_fails_a_driven_run(self):
        self.assertEqual(1, self.verdict(PASSING + "I suites: SUITE OVERFLOW: run_extra_suite\n",
                                        actions=[("suite", "x")]))

    def test_an_autorun_that_never_completes_fails(self):
        self.assertEqual(1, self.verdict(PASSING, finished=False))

    def test_a_failing_suite_fails_a_driven_run(self):
        self.assertEqual(1, self.verdict(FAILING, actions=[("suite", "x")]))

    def test_a_driven_run_needs_no_sentinel(self):
        self.assertEqual(0, self.verdict(PASSING, actions=[("suite", "x")]))

    def test_a_crash_fails_a_run_whose_tests_all_passed(self):
        self.assertEqual(1, self.verdict(BOOT + PASSING + ABORT + COMPLETE))

    def test_a_reboot_fails_a_driven_run(self):
        self.assertEqual(0, self.verdict(BOOT + PASSING, actions=["tap 1 1"]))
        self.assertEqual(1, self.verdict(BOOT + PASSING + BOOT, actions=["tap 1 1"]))


class FakeConsole:
    def __init__(self, lines):
        self.sent, self._lines = [], lines

    def send(self, line):
        self.sent.append(line)

    def lines(self, seconds=None):
        return iter(self._lines)


class LiveConsole(FakeConsole):
    """Answers BUILDID for the first `answers` heartbeats, then never again."""

    def __init__(self, answers):
        super().__init__([])
        self.answers = answers

    def wait_for(self, needle):
        return needle == qemu_run.LISTENING

    def send(self, line):
        super().send(line)
        if line == qemu_run.BUILD_ID_REQUEST:
            self._lines = ["BUILD_ID=abc"] if self.answers else []
            self.answers -= 1


class HeartbeatTest(unittest.TestCase):
    def drive(self, console, steps):
        with unittest.mock.patch.object(qemu_run.time, "sleep"), \
                unittest.mock.patch("builtins.print"):
            return qemu_run.drive_shell(console, steps)

    def test_a_tour_that_answers_after_every_step_passes(self):
        console = LiveConsole(answers=2)
        self.assertTrue(self.drive(console, ["wait 1", "tilt 0 0 4096"]))
        self.assertEqual(console.sent.count(qemu_run.BUILD_ID_REQUEST), 2)

    def test_a_step_with_no_heartbeat_after_it_ends_the_tour(self):
        console = LiveConsole(answers=1)
        self.assertFalse(self.drive(console, ["wait 1", "tilt 0 0 4096", "wait 1"]))
        self.assertEqual(console.sent.count(qemu_run.BUILD_ID_REQUEST), 2)


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

    def test_a_send_writes_the_rest_of_the_step_as_typed(self):
        ok, sent = self.run_action("send render scene  sponza-lite")
        self.assertTrue(ok)
        self.assertEqual(sent, ["render scene  sponza-lite"])

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


if __name__ == "__main__":
    unittest.main()
