"""Regression tests for the capture validator and the shell-owned reporters.

    python -m unittest discover -s launcher/tools/tests

A capture that measured nothing must FAIL rather than turn into a clean-
looking report; that is what these pin down, since the device itself cannot
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
sys.path.insert(0, str(TOOLS.parents[1] / "scripts" / "device" / "tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)

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
        for body in ("I (4500) shell: 30.0 fps\n", ":1:skipped:IGNORE\n"):
            with self.subTest(body=body):
                failures, _ = validate_capture.validate(self.capture(BOOT + body + COMPLETE))
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

    def test_a_runsuite_capture_starts_after_the_boot_so_it_has_no_banner(self):
        # autana suite --flash waits for the console, then sends RUNSUITE.
        failures, _ = validate_capture.validate(self.capture(RESULT), require_complete=False)
        self.assertEqual(failures, [])

    def test_a_whole_run_without_a_banner_is_rejected(self):
        failures, _ = validate_capture.validate(self.capture(RESULT + COMPLETE))
        self.assertTrue(any("no boot banner" in f for f in failures), failures)

    def test_a_crash_loop_is_rejected(self):
        failures, _ = validate_capture.validate(self.capture(BOOT + BOOT + RESULT + COMPLETE))
        self.assertTrue(any(f.startswith("rebooted:") for f in failures), failures)


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

    def test_boot_anim_reports_layouts_and_contention_separately(self):
        script = TOOLS / "boot_anim/report_boot_anim_perf.py"
        directory = str(script.parent)
        sys.path.insert(0, directory)
        self.addCleanup(sys.path.remove, directory)
        import report_boot_anim_perf as report
        capture = ""
        for mode, draw_us in (("FULL_FB", 100), ("DOUBLE_FB", 110), ("DOUBLE_FB_CONTENDED", 125)):
            capture += (f"boot_anim_perf: === BOOT_ANIM PERF curve (now_ms=1, 2 samples, mode={mode}) ===\n"
                        f"boot_anim_perf: Draw: min={draw_us}us max={draw_us}us avg={draw_us}us med={draw_us}us p95={draw_us}us\n"
                        "boot_anim_perf: Wait: min=1us max=2us avg=1us med=1us p95=2us\n"
                        "boot_anim_perf: Send: min=10us max=20us avg=15us med=15us p95=20us\n")
        runs, order, duplicates = report.parse_capture(self.capture(capture))
        self.assertEqual(len(runs), 3)
        self.assertEqual(duplicates, [])
        self.assertEqual([runs[key]["mode"] for key in order],
                         ["FULL_FB", "DOUBLE_FB", "DOUBLE_FB_CONTENDED"])
        self.assertEqual(runs[order[-1]]["phases"]["Draw"]["avg"], 125)
        out = self.out_path()
        done = subprocess.run([sys.executable, str(script),
                               self.capture(capture), out], capture_output=True, text=True)
        self.assertNotEqual(done.returncode, 0, done.stderr)
        self.assertIn("| `curve` | 25 |", pathlib.Path(out).read_text())
        self.assertIn("Total", done.stderr)
        self.assertNotIn("missing Draw", done.stderr)

    def boot_capture(self, mode=None, missing=None):
        suffix = f", mode={mode}" if mode else ""
        text = f"boot_anim_perf: === BOOT_ANIM PERF curve (now_ms=1, 2 samples{suffix}) ===\n"
        for phase in ("Total", "Image", "Present", "Draw", "Wait", "Send"):
            if phase == missing or (mode is None and phase in ("Draw", "Wait", "Send")):
                continue
            text += f"boot_anim_perf: {phase}: min=1us max=2us avg=1us med=1us p95=2us\n"
        for phase in ("Clear", "Floor", "Axes", "Curve", "Zeros", "Title"):
            text += f"boot_anim_perf: {phase}: avg=1us\n"
        return text

    def test_boot_anim_legacy_capture_needs_no_layout_phases(self):
        done = self.run_reporter("boot_anim/report_boot_anim_perf.py", self.boot_capture())
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("missing", done.stderr)

    def test_boot_anim_moded_capture_requires_wait_and_send(self):
        for phase in ("Wait", "Send"):
            with self.subTest(phase=phase):
                done = self.run_reporter("boot_anim/report_boot_anim_perf.py",
                                         self.boot_capture("LAYOUT_B", missing=phase))
                self.assertNotEqual(done.returncode, 0)
                self.assertIn(f"missing {phase}", done.stderr)

    def test_boot_anim_duplicate_label_and_mode_is_flagged(self):
        capture = self.boot_capture("LAYOUT_B")
        done = self.run_reporter("boot_anim/report_boot_anim_perf.py", capture + capture)
        self.assertIn("more than one BOOT_ANIM PERF header", done.stderr)

    def test_boot_anim_refuses_a_capture_with_no_checkpoint(self):
        done = self.run_reporter("boot_anim/report_boot_anim_perf.py", BOOT + RESULT)
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("BOOT_ANIM PERF", done.stderr)


class DeviceReportArgumentsTest(unittest.TestCase):
    """device_report.sh as a report script sources it, with `autana` (not
    device.py, #454 removed --owner/--worktree/--purpose from it, and
    device_report.sh now goes through `autana` like every other command,
    docs/tools/Device-Lock.md) stubbed to record the call instead of
    reaching a board."""

    def setUp(self):
        sys.path.insert(0, str(TOOLS.parents[1] / "scripts" / "device"))
        self.addCleanup(sys.path.remove, str(TOOLS.parents[1] / "scripts" / "device"))
        import device
        try:
            self.bash = device.git_bash()
        except RuntimeError as error:
            self.skipTest(str(error))
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.dir = pathlib.Path(temp.name)
        self.calls = self.dir / "calls.txt"

    def run_report(self, *arguments, board="", suite=""):
        script = (
            'report_name=t; report_dir="$1"; report_timeout=1; report_suite="$4"\n'
            'report_generate() { :; }\n'
            '. "$2"\n'
            'calls="$3"\n'
            'autana() { echo "$*" >> "$calls"; return 1; }\n'
            'shift 4\n'
            'device_report_run "$@"\n')
        return subprocess.run(
            [self.bash, "-c", script, str(TOOLS / "quality" / "report_test_results.sh"),
             self.dir.as_posix(), (TOOLS / "device" / "device_report.sh").as_posix(),
             self.calls.as_posix(), suite, "--no-restore",
             *(("--board", board) if board else ()), *arguments],
            cwd=self.dir, stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=60)

    def device_calls(self):
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def run_failed_capture_report(self, capture_text):
        capture = self.dir / "capture-source.txt"
        capture.write_text(capture_text, encoding="utf-8")
        out = self.dir / "out.md"
        out.unlink(missing_ok=True)
        script = (
            'report_name=t; report_dir="$1"; report_timeout=1; report_suite=run_perf\n'
            'report_sentinel="timing row"; report_capture_failures_ok=1\n'
            'report_generate() { cp "$1" "$2"; }\n'
            '. "$2"\n'
            'capture_source="$3"\n'
            'autana() { while [ "$#" -gt 0 ]; do '
            'if [ "$1" = --out ]; then cp "$capture_source" "$2"; '
            'case "$(cat "$capture_source")" in *"timing row"*) ended=complete ;; *) ended=max_seconds ;; esac; '
            'printf "%s\\n" "- Ended: $ended" > "${2%.*}.md"; break; fi; shift; done; return 1; }\n'
            'shift 3\n'
            'device_report_run "$@"\n')
        done = subprocess.run(
            [self.bash, "-c", script, str(TOOLS / "quality" / "report_test_results.sh"),
             self.dir.as_posix(), (TOOLS / "device" / "device_report.sh").as_posix(),
             capture.as_posix(), "--no-restore", out.as_posix()],
            cwd=self.dir, stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=60)
        return done, out

    def test_the_board_is_the_global_board_option_as_for_every_command(self):
        self.run_report("out.md", board="SERIAL1")
        [call] = self.device_calls()
        self.assertTrue(call.startswith("--board SERIAL1 --owner "), call)

    def test_no_board_option_leaves_the_board_to_autana(self):
        self.run_report("out.md")
        [call] = self.device_calls()
        self.assertNotIn("--board", call)

    def test_a_boot_time_report_calls_autana_selftest_with_project_and_out(self):
        self.run_report("out.md")
        [call] = self.device_calls()
        argv = call.split()
        self.assertEqual(argv[:2], ["--owner", "device_report-t"])
        self.assertEqual(argv[2:5], ["--wait", "3600", "selftest"])
        self.assertIn("--out", argv)
        self.assertIn("--project", argv)
        # #454 removed these from every command; a leftover here means
        # device_report.sh is passing a flag autana would refuse outright.
        self.assertNotIn("--worktree", argv)
        self.assertNotIn("--purpose", argv)

    def test_a_runsuite_report_calls_autana_suite_with_the_name_runs_and_flash(self):
        self.run_report("out.md", suite="run_gfx_suite")
        [call] = self.device_calls()
        argv = call.split()
        self.assertEqual(argv[:2], ["--owner", "device_report-t"])
        self.assertEqual(argv[2:5], ["--wait", "3600", "suite"])
        self.assertIn("run_gfx_suite", argv)
        self.assertIn("--runs", argv)
        self.assertIn("--flash", argv)
        self.assertIn("--project", argv)
        self.assertNotIn("--worktree", argv)
        self.assertNotIn("--purpose", argv)

    def test_a_budget_failure_is_reported_but_a_cut_short_capture_is_not(self):
        done, out = self.run_failed_capture_report("timing row\nsuite.c:1:test_budget:FAIL\n")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(out.read_text(encoding="utf-8"),
                         "timing row\nsuite.c:1:test_budget:FAIL\n")

        done, out = self.run_failed_capture_report("suite.c:1:test_budget:FAIL\n")
        self.assertNotEqual(done.returncode, 0)
        self.assertFalse(out.exists())

    def test_the_lock_owner_names_this_report_without_an_owner_flag(self):
        """Every autana call device_report_run makes starts with the global
        `--owner device_report-<name>`."""
        env = dict(os.environ)
        script = (
            'report_name=owner-probe; report_dir="$1"; report_timeout=1; report_suite=""\n'
            'report_generate() { :; }\n'
            '. "$2"\n'
            # A function called with arguments gets its own $1/$2/$3 for the
            # duration of that call; "$3" inside autana() would name one of
            # ITS OWN arguments, not this script's. Capture the real path
            # into a named variable first, same as $calls above.
            'owner_out="$3"\n'
            'autana() { echo "$*" > "$owner_out"; return 1; }\n'
            'shift 3\n'
            'device_report_run "$@"\n')
        with tempfile.TemporaryDirectory() as directory:
            owner_file = pathlib.Path(directory) / "owner.txt"
            subprocess.run(
                [self.bash, "-c", script, str(TOOLS / "quality" / "report_test_results.sh"),
                 directory, (TOOLS / "device" / "device_report.sh").as_posix(),
                 owner_file.as_posix(), "--no-restore", "out.md"],
                cwd=directory, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                timeout=60, env=env)
            self.assertTrue(owner_file.read_text().startswith(
                "--owner device_report-owner-probe "))

    def test_anything_but_one_report_path_is_refused_before_any_device_call(self):
        for arguments in (("SERIAL1", "out.md"), ("SERIAL1",), ("--board",),
                          ("one.md", "two.md")):
            with self.subTest(arguments=arguments):
                done = self.run_report(*arguments)
                self.assertNotEqual(done.returncode, 0)
                self.assertIn("--board", done.stderr)
                self.assertEqual(self.device_calls(), [])


if __name__ == "__main__":
    unittest.main()
