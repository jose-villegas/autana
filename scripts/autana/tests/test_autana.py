"""Dispatch and console-routing tests for scripts/autana/autana.py. No
hardware and no real device.py process: every device-touching call is
mocked at the subprocess boundary, or at send() - autana's own thin wrapper
around one `device.py send` call - for the commands built on top of it. See
scripts/device/tests/test_device.py and launcher/tools/tests/test_screenshot.py
for device.py's and the wire protocol's own coverage.

    python -m unittest discover -s scripts/autana/tests
"""
import sys
import unittest
from pathlib import Path
from unittest import mock

AUTANA = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AUTANA))
import autana  # noqa: E402


class SendCommandBuildingTests(unittest.TestCase):
    """send() is the one place that actually shells out to `device.py send`
    - every device-verb command (freeze, touch, tune, ...) goes through it,
    so its own command-building is worth pinning once, directly."""

    def setUp(self):
        status = mock.Mock(stdout="", stderr="", returncode=0)
        answered = mock.Mock(stdout="TUNE_OK launcher.ridge_trail=200\n", stderr="", returncode=0)
        self.calls = []

        def fake_run(command, **unused_kwargs):
            self.calls.append(command)
            return status if command[-1] == "status" else answered

        patcher = mock.patch.object(autana.subprocess, "run", side_effect=fake_run)
        patcher.start()
        self.addCleanup(patcher.stop)

    def last_send_command(self):
        return self.calls[-1]

    def test_the_default_reply_adds_no_reply_or_until_flags(self):
        autana.send("TUNE")
        command = self.last_send_command()
        self.assertIn("send", command)
        self.assertIn("TUNE", command)
        self.assertNotIn("--reply", command)
        self.assertNotIn("--optional", command)
        self.assertNotIn("--seconds", command)

    def test_a_non_default_reply_adds_reply_and_until(self):
        autana.send("BUILDID", reply="BUILD_ID", purpose="autana buildid")
        command = self.last_send_command()
        self.assertEqual(command[command.index("--reply") + 1], "BUILD_ID")
        self.assertEqual(command[command.index("--until") + 1], "BUILD_ID")

    def test_an_explicit_until_list_overrides_the_reply_default(self):
        """An app's own command (forward()) ends on either of two lines,
        not one - a multi-line reply's _END, or its _ERR."""
        autana.send("EXAMPLE status", reply="EXAMPLE", until=["EXAMPLE_END", "EXAMPLE_ERR"])
        command = self.last_send_command()
        untils = [command[i + 1] for i, word in enumerate(command) if word == "--until"]
        self.assertEqual(untils, ["EXAMPLE_END", "EXAMPLE_ERR"])

    def test_optional_and_seconds_are_forwarded(self):
        autana.send("TOUCH down 1 2", reply="TOUCH", optional=True, seconds=0.5)
        command = self.last_send_command()
        self.assertIn("--optional", command)
        self.assertEqual(command[command.index("--seconds") + 1], "0.5")

    def test_a_busy_board_sends_nothing(self):
        busy = mock.Mock(stdout="held by someone-else@0001 for 3s\n")
        with mock.patch.object(autana.subprocess, "run", return_value=busy):
            code, replies = autana.send("TUNE")
        self.assertEqual(code, 3)
        self.assertEqual(replies, [])


class DeviceVerbCommandTests(unittest.TestCase):
    """freeze/resume/step/touch/imu: each is a thin line-builder over
    send(), so the contract worth pinning is what line, reply and mode each
    one asks send() for."""

    def test_freeze_sends_the_bare_verb(self):
        with mock.patch.object(autana, "send", return_value=(0, ["FREEZE_STATE frozen=1 steps=0"])) as sent, \
             mock.patch("builtins.print"):
            code = autana.freeze([])
        sent.assert_called_once_with("FREEZE", reply="FREEZE_STATE", purpose="autana freeze")
        self.assertEqual(code, 0)

    def test_freeze_rejects_arguments(self):
        with self.assertRaises(SystemExit):
            autana.freeze(["extra"])

    def test_resume_sends_the_bare_verb(self):
        with mock.patch.object(autana, "send", return_value=(0, [])) as sent, mock.patch("builtins.print"):
            autana.resume([])
        sent.assert_called_once_with("RESUME", reply="FREEZE_STATE", purpose="autana resume")

    def test_step_with_no_count_sends_a_bare_step(self):
        with mock.patch.object(autana, "send", return_value=(0, [])) as sent, mock.patch("builtins.print"):
            autana.step([])
        sent.assert_called_once_with("STEP", reply="FREEZE_STATE", purpose="autana step")

    def test_step_with_a_count_appends_it(self):
        with mock.patch.object(autana, "send", return_value=(0, [])) as sent, mock.patch("builtins.print"):
            autana.step(["5"])
        sent.assert_called_once_with("STEP 5", reply="FREEZE_STATE", purpose="autana step")

    def test_step_rejects_a_non_numeric_count(self):
        with self.assertRaises(SystemExit):
            autana.step(["five"])

    def test_step_rejects_more_than_one_argument(self):
        with self.assertRaises(SystemExit):
            autana.step(["5", "6"])

    def test_touch_passes_its_arguments_through_and_is_optional(self):
        with mock.patch.object(autana, "send", return_value=(0, [])) as sent, mock.patch("builtins.print"):
            autana.touch(["down", "10", "20"])
        sent.assert_called_once_with("TOUCH down 10 20", reply="TOUCH", purpose="autana touch",
                                     optional=True, seconds=0.5)

    def test_touch_rejects_a_state_word_that_is_not_down_or_up(self):
        with self.assertRaises(SystemExit):
            autana.touch(["sideways", "1", "2"])

    def test_touch_rejects_non_integer_coordinates(self):
        with self.assertRaises(SystemExit):
            autana.touch(["down", "x", "2"])

    def test_imu_passes_its_arguments_through_and_is_optional(self):
        with mock.patch.object(autana, "send", return_value=(0, [])) as sent, mock.patch("builtins.print"):
            autana.imu(["100", "-200", "16384"])
        sent.assert_called_once_with("IMU 100 -200 16384", reply="IMU", purpose="autana imu",
                                     optional=True, seconds=0.5)

    def test_imu_rejects_a_non_integer_axis(self):
        with self.assertRaises(SystemExit):
            autana.imu(["1", "two", "3"])

    def test_tap_sends_one_device_side_gesture(self):
        with mock.patch.object(autana, "send", return_value=(0, [])) as sent, mock.patch("builtins.print"):
            autana.tap(["10", "20"])
        sent.assert_called_once_with("TAP 10 20", reply="TAP", until=["TAP_OK"], purpose="autana tap")

    def test_drag_requires_its_duration(self):
        with self.assertRaises(SystemExit):
            autana.drag(["1", "2", "3", "4"])

    def test_button_waits_for_the_board_to_answer(self):
        with mock.patch.object(autana, "send", return_value=(0, ["BUTTON_OK"])) as sent, \
             mock.patch("builtins.print") as printed:
            autana.button(["power", "long"])
        sent.assert_called_once_with("BUTTON power long", reply="BUTTON",
                                     until=["BUTTON_OK", "BUTTON_ERR"], purpose="autana button")
        printed.assert_called_once_with("BUTTON_OK")

    def test_apps_waits_for_the_complete_listing(self):
        with mock.patch.object(autana, "send", return_value=(0, ["APPS name=Star Chart running=0", "APPS_END"])) as sent, \
             mock.patch("builtins.print") as printed:
            autana.apps([])
        sent.assert_called_once_with("APPS", reply="APPS", until=["APPS_END"], purpose="autana apps")
        printed.assert_called_once_with("APPS name=Star Chart running=0")

    def test_open_sends_the_app_name(self):
        with mock.patch.object(autana, "send", return_value=(0, ["OPEN_OK name=Star Chart"])) as sent, \
             mock.patch("builtins.print"):
            autana.open_app(["star"])
        sent.assert_called_once_with("OPEN star", reply="OPEN", purpose="autana open")


class ScreenshotCommandTests(unittest.TestCase):
    """autana screenshot goes straight to `device.py screenshot`, not
    through send() - see device.py's own screenshot() for the capture and
    decode this only triggers."""

    def test_builds_the_device_screenshot_invocation_with_out(self):
        status = mock.Mock(stdout="")
        with mock.patch.object(autana.subprocess, "run", return_value=status), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.screenshot(["-o", "out.png"])
        self.assertEqual(code, 0)
        command = called.call_args[0][0]
        self.assertIn("screenshot", command)
        self.assertEqual(command[command.index("--out") + 1], "out.png")

    def test_no_path_omits_out(self):
        status = mock.Mock(stdout="")
        with mock.patch.object(autana.subprocess, "run", return_value=status), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.screenshot([])
        command = called.call_args[0][0]
        self.assertNotIn("--out", command)

    def test_as_shown_is_forwarded_to_device(self):
        status = mock.Mock(stdout="")
        with mock.patch.object(autana.subprocess, "run", return_value=status), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.screenshot(["--as-shown"])
        self.assertIn("--as-shown", called.call_args[0][0])

    def test_framebuffer_is_forwarded_to_device(self):
        status = mock.Mock(stdout="")
        with mock.patch.object(autana.subprocess, "run", return_value=status), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.screenshot(["--framebuffer"])
        self.assertIn("--framebuffer", called.call_args[0][0])

    def test_screenshot_views_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            autana.screenshot(["--as-shown", "--framebuffer"])

    def test_unrecognised_flags_are_rejected(self):
        with self.assertRaises(SystemExit):
            autana.screenshot(["bogus"])

    def test_a_busy_board_is_refused_without_calling_device(self):
        busy = mock.Mock(stdout="held by someone-else@0001 for 3s\n")
        with mock.patch.object(autana.subprocess, "run", return_value=busy), \
             mock.patch.object(autana.subprocess, "call") as called:
            code = autana.screenshot([])
        self.assertEqual(code, 3)
        called.assert_not_called()


class FlashCommandTests(unittest.TestCase):
    def test_perf_scope_is_forwarded(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.flash(["diag", "--quiet", "--perf-scope"])
        command = called.call_args[0][0]
        self.assertIn("--perf-scope", command)

    def test_no_perf_scope_flag_is_not_forwarded(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.flash(["diag", "--quiet"])
        command = called.call_args[0][0]
        self.assertNotIn("--perf-scope", command)


class MonitorCommandTests(unittest.TestCase):
    """autana monitor forwards to `device.py listen`, which matches the
    capture's own BUILD_ID to a build directory itself when --elf is not
    given - autana no longer guesses an ELF by file mtime."""

    def test_no_elf_given_omits_the_flag_entirely(self):
        with mock.patch.object(autana.subprocess, "Popen") as called:
            autana.monitor(["--follow"])
        self.assertNotIn("--elf", called.call_args[0][0])

    def test_an_explicit_elf_is_passed_through_as_is(self):
        with mock.patch.object(autana.subprocess, "Popen") as called:
            autana.monitor(["30", "--elf", "mine.elf"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--seconds") + 1], "30.0")
        self.assertEqual(command[command.index("--elf") + 1], "mine.elf")

    def test_elf_with_no_value_is_rejected(self):
        with self.assertRaises(SystemExit):
            autana.monitor(["--elf"])

    def test_echo_only_when_stdout_is_a_terminal(self):
        for tty in (False, True):
            with self.subTest(tty=tty), \
                 mock.patch.object(autana.sys.stdout, "isatty", return_value=tty), \
                 mock.patch.object(autana.subprocess, "Popen") as started:
                autana.monitor(["--follow"])
            self.assertEqual("--echo" in started.call_args.args[0], tty)

    def test_follow_is_forwarded_without_seconds(self):
        with mock.patch.object(autana.subprocess, "Popen") as started:
            autana.monitor(["--follow"])
        command = started.call_args.args[0]
        self.assertIn("--follow", command)
        self.assertNotIn("--seconds", command)

    def test_follow_and_seconds_is_a_usage_error(self):
        with mock.patch.object(autana.subprocess, "Popen") as started, \
             self.assertRaises(SystemExit):
            autana.monitor(["30", "--follow"])
        started.assert_not_called()

    def test_parent_waits_for_child_after_interrupt(self):
        process = mock.Mock()
        process.wait.side_effect = [KeyboardInterrupt, 7]
        with mock.patch.object(autana.subprocess, "Popen", return_value=process):
            self.assertEqual(autana.monitor(["--follow"]), 7)
        self.assertEqual(process.wait.call_count, 2)

    def test_terminal_without_duration_follows(self):
        with mock.patch.object(autana.sys.stdout, "isatty", return_value=True), \
             mock.patch.object(autana.subprocess, "Popen") as started:
            autana.monitor([])
        self.assertIn("--follow", started.call_args.args[0])

    def test_pipe_without_duration_reports_usage_code_two(self):
        with mock.patch.object(autana.sys.stdout, "isatty", return_value=False), \
             mock.patch.object(autana.subprocess, "Popen") as started, \
             self.assertRaises(SystemExit) as caught:
            autana.monitor([])
        self.assertEqual(caught.exception.code, 2)
        started.assert_not_called()

    def test_stream_forces_echo_in_pipe(self):
        with mock.patch.object(autana.sys.stdout, "isatty", return_value=False), \
             mock.patch.object(autana.subprocess, "Popen") as started:
            autana.monitor(["30", "--stream"])
        self.assertIn("--echo", started.call_args.args[0])

    def test_follow_and_elf_are_forwarded(self):
        with mock.patch.object(autana.subprocess, "Popen") as started:
            autana.monitor(["--follow", "--elf", "mine.elf"])
        self.assertIn("--follow", started.call_args.args[0])
        self.assertIn("mine.elf", started.call_args.args[0])

    def test_second_interrupt_stops_waiting(self):
        process = mock.Mock()
        process.wait.side_effect = [KeyboardInterrupt, KeyboardInterrupt]
        with mock.patch.object(autana.subprocess, "Popen", return_value=process):
            self.assertEqual(autana.monitor(["--follow"]), 130)


class ResetCommandTests(unittest.TestCase):
    def test_verbose_reaches_reset_capture(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.reset(["--capture", "--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])

    def test_capture_forwards_its_window_to_device(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.reset(["--capture", "15"])
        self.assertEqual(code, 0)
        command = called.call_args[0][0]
        self.assertIn("reset", command)
        self.assertIn("--capture", command)
        self.assertEqual(command[command.index("--seconds") + 1], "15.0")

    def test_capture_without_a_window_leaves_the_default_to_device(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.reset(["--capture"])
        self.assertNotIn("--seconds", called.call_args[0][0])

    def test_window_without_capture_is_rejected(self):
        with self.assertRaises(SystemExit):
            autana.reset(["30"])


class SelftestCommandTests(unittest.TestCase):
    def test_verbose_reaches_device(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.selftest(["--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])

    def test_builds_the_device_selftest_invocation(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.selftest([])
        self.assertEqual(code, 0)
        command = called.call_args[0][0]
        self.assertIn("selftest", command)
        self.assertEqual(command[command.index("--worktree") + 1], "C:/wt")
        self.assertEqual(command[command.index("--max-seconds") + 1], "3000.0")

    def test_a_seconds_argument_is_forwarded(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.selftest(["120"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--max-seconds") + 1], "120.0")


class BatchCommandTests(unittest.TestCase):
    def test_verbose_reaches_device(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.batch(["run_gfx_suite", "--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])

    def test_one_suite_defaults_runs_and_is_always_diag(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.batch(["run_sand_perf_suite"])
        self.assertEqual(code, 0)
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--suite") + 1], "run_sand_perf_suite")
        self.assertEqual(command[command.index("--runs") + 1], "3")
        self.assertEqual(command[command.index("--variant") + 1], "diag")
        self.assertNotIn("--perf-scope", command)

    def test_several_suites_each_get_their_own_flag(self):
        with mock.patch.object(autana, "engine_worktree", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.batch(["run_sand_perf_suite", "run_gfx_suite", "--runs", "5", "--perf-scope"])
        command = called.call_args[0][0]
        self.assertEqual(command.count("--suite"), 2)
        self.assertEqual(command[command.index("--runs") + 1], "5")
        self.assertIn("--perf-scope", command)

    def test_no_variant_option_exists(self):
        # A suite only exists to run in the diagnostics image - offering a
        # variant choice here would only ever have one real answer.
        with self.assertRaises(SystemExit):
            autana.batch(["run_sand_perf_suite", "--variant", "dev"])

    def test_no_suite_is_rejected(self):
        with self.assertRaises(SystemExit):
            autana.batch(["--runs", "3"])

    def test_an_unknown_flag_is_rejected(self):
        with self.assertRaises(SystemExit):
            autana.batch(["run_sand_perf_suite", "--bogus"])


class SuiteCommandTests(unittest.TestCase):
    def test_verbose_reaches_device(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])


class LockCommandTests(unittest.TestCase):
    """status/release/hand/take-back: thin pass-throughs to device.py's own
    lock-level commands."""

    def test_status_takes_no_arguments_and_calls_device(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.status([])
        self.assertEqual(code, 0)
        self.assertIn("status", called.call_args[0][0])

    def test_status_rejects_arguments(self):
        with self.assertRaises(SystemExit):
            autana.status(["extra"])

    def test_release_forwards_the_token(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.release(["deadbeef"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--token") + 1], "deadbeef")

    def test_release_needs_exactly_a_token(self):
        with self.assertRaises(SystemExit):
            autana.release([])

    def test_hand_joins_its_words_into_one_note(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.hand(["checking", "the", "panel"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--note") + 1], "checking the panel")

    def test_hand_needs_a_note(self):
        with self.assertRaises(SystemExit):
            autana.hand([])

    def test_hand_forwards_wait_and_note(self):
        with mock.patch.object(autana.subprocess, "Popen") as called:
            called.return_value.wait.return_value = 3
            code = autana.hand(["--wait", "10", "enter", "download", "mode"])
        self.assertEqual(code, 3)
        command = called.call_args.args[0]
        self.assertEqual(command[command.index("--wait") + 1], "10")
        self.assertEqual(command[command.index("--note") + 1], "enter download mode")

    def test_hand_interrupt_returns_child_timeout_status(self):
        with mock.patch.object(autana.subprocess, "Popen") as started, \
             mock.patch("builtins.print") as output:
            started.return_value.wait.side_effect = [KeyboardInterrupt, 3]
            code = autana.hand(["--wait", "10", "download mode"])
        self.assertEqual(code, 3)
        started.return_value.terminate.assert_not_called()
        output.assert_not_called()

    def test_hand_rejects_wait_without_seconds(self):
        with self.assertRaises(SystemExit):
            autana.hand(["--wait", "enter mode"])

    def test_take_back_takes_no_arguments(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.take_back([])
        self.assertEqual(code, 0)
        self.assertIn("take-back", called.call_args[0][0])

    def test_take_back_rejects_arguments(self):
        with self.assertRaises(SystemExit):
            autana.take_back(["extra"])


class TuneCommandTests(unittest.TestCase):
    """Tuning is no longer implicit - `tune` and its subforms are the only
    way to a tunable, on the command line and in the console alike."""

    ROWS = [("ridge.trail", "226", "0", "255", "226"),
           ("ridge.glow_radius", "13", "1", "31", "13"),
           ("wave.height", "40", "0", "100", "40")]

    def test_bare_tune_lists_every_row(self):
        with mock.patch.object(autana, "tunables", return_value=self.ROWS), \
             mock.patch("builtins.print") as printed:
            code = autana.tune([])
        self.assertEqual(code, 0)
        self.assertEqual(printed.call_count, 3)

    def test_filter_text_narrows_the_listing(self):
        with mock.patch.object(autana, "tunables", return_value=self.ROWS), \
             mock.patch("builtins.print") as printed:
            autana.tune(["wave"])
        printed.assert_called_once()
        self.assertIn("wave.height", printed.call_args[0][0])

    def test_no_matches_prints_a_message(self):
        with mock.patch.object(autana, "tunables", return_value=self.ROWS), \
             mock.patch("builtins.print") as printed:
            autana.tune(["zzz"])
        printed.assert_called_once_with("no tunables containing 'zzz'")

    def test_an_exact_unambiguous_name_shows_just_that_one(self):
        with mock.patch.object(autana, "tunables", return_value=self.ROWS), \
             mock.patch("builtins.print") as printed:
            autana.tune(["trail"])
        printed.assert_called_once()
        self.assertIn("ridge.trail", printed.call_args[0][0])

    def test_an_ambiguous_name_falls_back_to_the_filtered_listing(self):
        rows = self.ROWS + [("other.trail", "1", "0", "2", "1")]
        with mock.patch.object(autana, "tunables", return_value=rows), \
             mock.patch("builtins.print") as printed:
            autana.tune(["trail"])
        self.assertEqual(printed.call_count, 2)

    def test_two_arguments_set_the_resolved_name(self):
        with mock.patch.object(autana, "send", return_value=(0, ["TUNE_OK ridge.trail=200"])) as sent, \
             mock.patch.object(autana, "tunables", return_value=self.ROWS), mock.patch("builtins.print"):
            code = autana.tune(["trail", "200"])
        self.assertEqual(code, 0)
        sent.assert_called_once_with("SET ridge.trail 200")

    def test_reset_needs_exactly_a_name(self):
        with self.assertRaises(SystemExit):
            autana.tune(["reset"])

    def test_reset_resets_the_resolved_name(self):
        with mock.patch.object(autana, "send", return_value=(0, ["TUNE_OK ridge.trail=226"])) as sent, \
             mock.patch.object(autana, "tunables", return_value=self.ROWS), mock.patch("builtins.print"):
            autana.tune(["reset", "trail"])
        sent.assert_called_once_with("RESET ridge.trail")

    def test_save_takes_no_further_words(self):
        with self.assertRaises(SystemExit):
            autana.tune(["save", "extra"])

    def test_save_calls_the_source_writer(self):
        with mock.patch.object(autana, "save", return_value=0) as saved:
            code = autana.tune(["save"])
        saved.assert_called_once_with()
        self.assertEqual(code, 0)

    def test_reset_as_a_value_is_a_set_not_the_reset_subcommand(self):
        # "reset"/"save" are only ever a subcommand in first position; here
        # "trail" is first, so "reset" is just the value being set.
        with mock.patch.object(autana, "send", return_value=(0, ["TUNE_OK ridge.trail=reset"])) as sent, \
             mock.patch.object(autana, "tunables", return_value=self.ROWS), mock.patch("builtins.print"):
            autana.tune(["trail", "reset"])
        sent.assert_called_once_with("SET ridge.trail reset")

    def test_too_many_arguments_are_rejected(self):
        with self.assertRaises(SystemExit):
            autana.tune(["a", "b", "c"])


class ConsoleRoutingTests(unittest.TestCase):
    """A bare word in the session is only ever an autana command (which
    includes the device console verbs autana now exposes directly) or "not
    understood" - never an implicit tunable lookup."""

    def run_console(self, lines):
        with mock.patch("builtins.input", side_effect=[*lines, EOFError()]):
            autana.console()

    def test_a_known_device_verb_is_dispatched_as_a_command(self):
        fake = mock.Mock(return_value=0)
        with mock.patch.dict(autana.COMMANDS, {"screenshot": fake}):
            self.run_console(["screenshot"])
        fake.assert_called_once_with([])

    def test_a_device_verb_with_arguments_passes_them_through(self):
        fake = mock.Mock(return_value=0)
        with mock.patch.dict(autana.COMMANDS, {"touch": fake}):
            self.run_console(["touch down 10 20"])
        fake.assert_called_once_with(["down", "10", "20"])

    def test_a_line_no_autana_command_recognises_is_forwarded_to_the_device(self):
        """The reply prefix autana asks for is the line's own first word in
        capitals - an app's own reply always starts with its own prefix."""
        with mock.patch.object(autana, "send", return_value=(0, ["EXAMPLE status=ok", "EXAMPLE_END"])) as sent, \
             mock.patch("builtins.print") as printed:
            self.run_console(["example status"])
        sent.assert_called_once_with("example status", reply="EXAMPLE", until=["EXAMPLE_END", "EXAMPLE_ERR"],
                                     purpose="autana console example", optional=True)
        printed.assert_any_call("EXAMPLE status=ok\nEXAMPLE_END")

    def test_a_forwarded_line_with_no_reply_says_sent(self):
        with mock.patch.object(autana, "send", return_value=(0, [])), mock.patch("builtins.print") as printed:
            self.run_console(["trail 200"])
        printed.assert_any_call("sent")

    def test_a_forwarded_lines_several_reply_lines_are_joined(self):
        with mock.patch.object(autana, "send", return_value=(0, ["EXAMPLE alpha=1", "EXAMPLE beta=2", "EXAMPLE_END"])), \
             mock.patch("builtins.print") as printed:
            self.run_console(["example status"])
        printed.assert_any_call("EXAMPLE alpha=1\nEXAMPLE beta=2\nEXAMPLE_END")

    def test_tune_with_a_name_is_still_the_way_to_reach_a_tunable(self):
        fake = mock.Mock(return_value=0)
        with mock.patch.dict(autana.COMMANDS, {"tune": fake}):
            self.run_console(["tune trail"])
        fake.assert_called_once_with(["trail"])

    def test_quit_ends_the_session_without_dispatching_anything(self):
        fake = mock.Mock(return_value=0)
        with mock.patch.dict(autana.COMMANDS, {"tune": fake}), \
             mock.patch("builtins.input", side_effect=["quit"]):
            code = autana.console()
        self.assertEqual(code, 0)
        fake.assert_not_called()


class OneShotForwardingTests(unittest.TestCase):
    """The maintainer's rule (every board operation goes through autana)
    applies to a one-shot invocation the same as a session line - main()
    forwards a first argument no COMMANDS entry recognises rather than
    refusing it."""

    def run_main(self, argv):
        with mock.patch.object(autana.sys, "argv", ["autana", *argv]):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
        return stop.exception.code

    def test_an_unrecognised_first_argument_is_forwarded_to_the_device(self):
        with mock.patch.object(autana, "send", return_value=(0, ["EXAMPLE status=ok", "EXAMPLE_END"])) as sent, \
             mock.patch("builtins.print") as printed:
            code = self.run_main(["example", "status"])
        sent.assert_called_once_with("example status", reply="EXAMPLE", until=["EXAMPLE_END", "EXAMPLE_ERR"],
                                     purpose="autana console example", optional=True)
        printed.assert_any_call("EXAMPLE status=ok\nEXAMPLE_END")
        self.assertEqual(code, 0)

    def test_a_recognised_command_is_still_dispatched_directly(self):
        fake = mock.Mock(return_value=0)
        with mock.patch.dict(autana.COMMANDS, {"buildid": fake}), \
             mock.patch.object(autana, "send") as sent:
            self.run_main(["buildid"])
        fake.assert_called_once_with([])
        sent.assert_not_called()


if __name__ == "__main__":
    unittest.main()
