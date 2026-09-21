"""Dispatch and console-routing tests for scripts/autana/autana.py. No
hardware and no real device.py process: every device-touching call is
mocked at the subprocess boundary, or at send() - autana's own thin wrapper
around one `device.py send` call - for the commands built on top of it. See
scripts/device/test_device.py and launcher/tools/tests/test_screenshot.py
for device.py's and the wire protocol's own coverage.

    python -m unittest discover -s scripts/autana
"""
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
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

    def test_a_bare_tunable_name_is_not_understood(self):
        with mock.patch("builtins.print") as printed:
            self.run_console(["trail"])
        printed.assert_any_call("not understood - 'help' lists what is")

    def test_a_bare_tunable_name_with_a_value_is_also_not_understood(self):
        with mock.patch("builtins.print") as printed:
            self.run_console(["trail 200"])
        printed.assert_any_call("not understood - 'help' lists what is")

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


if __name__ == "__main__":
    unittest.main()
