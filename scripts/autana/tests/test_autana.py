"""Dispatch and console-routing tests for scripts/autana/autana.py. No
hardware and no real device.py process: every device-touching call is
mocked at the subprocess boundary, or at send() - autana's own thin wrapper
around one `device.py send` call - for the commands built on top of it. See
scripts/device/tests/test_device.py and launcher/tools/tests/test_screenshot.py
for device.py's and the wire protocol's own coverage.

    python -m unittest discover -s scripts/autana/tests
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401  (first: keeps the suite out of real records)
import contextlib
import getpass
import io
import json
import socket
import os
import tempfile
import unittest
from unittest import mock

AUTANA = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AUTANA))
import autana  # noqa: E402

sys.path.insert(0, str(AUTANA.parent / "device"))
import device_lock  # noqa: E402


def busy_status(owner="someone-else@0001", port="COM3"):
    """`device.py status --json` with one board that `owner` holds."""
    return json.dumps({"boards": [{
        "board": "90:70:69:FE:A3:08", "port": port, "state": "held",
        "holder": {"owner": owner, "purpose": "autana screenshot"}, "since": 1000,
        "elapsed_seconds": 3, "estimated_free": None, "stale": None, "waiting": []}]})


class SendCommandBuildingTests(unittest.TestCase):
    """send() is the one place that actually shells out to `device.py send`
    - every device-verb command (freeze, touch, tune, ...) goes through it,
    so its own command-building is worth pinning once, directly."""

    def setUp(self):
        status = mock.Mock(stdout='{"boards": []}', stderr="", returncode=0)
        answered = mock.Mock(stdout="TUNE_OK launcher.ridge_trail=200\n", stderr="", returncode=0)
        self.calls = []

        def fake_run(command, **unused_kwargs):
            self.calls.append(command)
            return status if "status" in command else answered

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
        busy = mock.Mock(stdout=busy_status())
        with mock.patch.object(autana.subprocess, "run", return_value=busy):
            code, replies = autana.send("TUNE")
        self.assertEqual(code, 3)
        self.assertEqual(replies, [])

    def test_the_short_fail_fast_wait_is_used_by_default(self):
        environ = {key: value for key, value in autana.os.environ.items()
                  if key != autana.WAIT_ENV}
        with mock.patch.dict(autana.os.environ, environ, clear=True):
            autana.send("TUNE")
        command = self.last_send_command()
        self.assertEqual(command[command.index("--wait") + 1], str(autana.SEND_WAIT_S))

    def test_the_short_fail_fast_wait_wins_over_the_global_wait(self):
        with mock.patch.dict(autana.os.environ, {autana.WAIT_ENV: "90"}):
            autana.send("TUNE")
        command = self.last_send_command()
        self.assertEqual(command[command.index("--wait") + 1], str(autana.SEND_WAIT_S))


class BoardHolderTests(unittest.TestCase):
    """board_holder() reads `device.py status --json`, pinned here from a
    real store's own status entry."""

    BOARD = "90:70:69:FE:A3:08"

    def status_output(self, holder_pid, owner="killed@0pac", port="COM3"):
        with tempfile.TemporaryDirectory() as root:
            store = device_lock.LockStore(root, is_alive=lambda pid: pid == 1)
            store.write_json(store.lock_path(self.BOARD), {
                "acquired_at": store.now(), "board": self.BOARD, "heartbeat_at": store.now(),
                "expected_build_id": "", "host": socket.gethostname(), "kind": "screenshot",
                "owner": owner, "pid": holder_pid, "purpose": "autana screenshot",
                "token": "old",
            })
            entry = device_lock.status_entry(store, self.BOARD, port, durations={})
        return json.dumps({"boards": [entry]})

    def holder_seen(self, status_text):
        with mock.patch.object(autana.subprocess, "run", return_value=mock.Mock(stdout=status_text)):
            return autana.board_holder()

    def test_a_lock_whose_holder_died_does_not_make_the_board_busy(self):
        self.assertEqual(self.holder_seen(self.status_output(holder_pid=99)), "")

    def test_a_lock_whose_holder_lives_still_makes_the_board_busy(self):
        self.assertEqual(self.holder_seen(self.status_output(holder_pid=1)),
                         "held by killed@0pac for autana screenshot")

    def test_a_lock_this_autana_holds_is_not_somebody_else_s(self):
        self.assertEqual(self.holder_seen(self.status_output(1, owner=autana.owner())), "")

    def test_a_held_board_that_is_not_plugged_in_blocks_nothing(self):
        self.assertEqual(self.holder_seen(self.status_output(holder_pid=1, port=None)), "")

    def test_the_named_board_is_judged_even_off_usb(self):
        named_off_usb = self.status_output(holder_pid=1, port=None)
        with mock.patch.dict(autana.os.environ, {autana.BOARD_ENV: self.BOARD}):
            self.assertEqual(self.holder_seen(named_off_usb),
                             "held by killed@0pac for autana screenshot")

    def test_with_two_boards_plugged_and_none_named_device_py_decides(self):
        held = json.loads(self.status_output(holder_pid=1))["boards"][0]
        free = dict(held, board="90:70:69:FE:B1:22", port="COM7", state="unlocked", holder=None)
        self.assertEqual(self.holder_seen(json.dumps({"boards": [held, free]})), "")


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

    def test_frames_primes_then_freezes_and_steps_between_captures(self):
        events = []
        status = mock.Mock(stdout="")

        def capture(command):
            events.append("capture " + (command[command.index("--out") + 1] if "--out" in command else ""))
            return 0

        def sent(line, **kwargs):
            events.append(line)
            return 0, []

        with mock.patch.object(autana.subprocess, "run", return_value=status), \
             mock.patch.object(autana.subprocess, "call", side_effect=capture), \
             mock.patch.object(autana, "send", side_effect=sent), mock.patch("builtins.print"):
            code = autana.screenshot(["--frames", "3", "-o", "cap"])
        self.assertEqual(code, 0)
        self.assertEqual(events[1:], ["FREEZE", "capture cap-00", "STEP", "capture cap-01", "STEP",
                                      "capture cap-02", "RESUME"])
        self.assertTrue(events[0].startswith("capture "), "one capture before freezing primes band mode")

    def test_frames_needs_a_positive_count_and_a_path(self):
        for args in (["--frames", "0", "-o", "cap"], ["--frames", "x", "-o", "cap"], ["--frames", "3"]):
            with self.assertRaises(SystemExit):
                autana.screenshot(list(args))

    def test_a_busy_board_is_refused_without_calling_device(self):
        busy = mock.Mock(stdout=busy_status())
        with mock.patch.object(autana.subprocess, "run", return_value=busy), \
             mock.patch.object(autana.subprocess, "call") as called:
            code = autana.screenshot([])
        self.assertEqual(code, 3)
        called.assert_not_called()

    def test_an_unknown_flag_is_named_and_rejected(self):
        status = mock.Mock(stdout="")
        with mock.patch.object(autana.subprocess, "run", return_value=status), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called, \
             self.assertRaises(SystemExit) as caught:
            autana.screenshot(["--owner", "delegate-1"])
        self.assertEqual(str(caught.exception), "autana screenshot: unknown flag --owner")
        called.assert_not_called()


class PopValueTests(unittest.TestCase):
    """pop_value() is the one flag-with-a-value stripper every board
    command's own parsing is built on."""

    def test_absent_flag_leaves_args_untouched(self):
        value, rest = autana.pop_value(["a", "b"], "--owner")
        self.assertIsNone(value)
        self.assertEqual(rest, ["a", "b"])

    def test_present_flag_is_removed_with_its_value(self):
        value, rest = autana.pop_value(["suite", "--owner", "sam", "30"], "--owner")
        self.assertEqual(value, "sam")
        self.assertEqual(rest, ["suite", "30"])

    def test_a_flag_with_no_value_is_a_usage_error(self):
        with self.assertRaises(SystemExit):
            autana.pop_value(["suite", "--owner"], "--owner")


class OwnerTests(unittest.TestCase):
    """owner() is the one place a board command's lock identity comes from:
    the global `--owner` first (with this process's own pid still appended,
    so two shells sharing the label don't see each other's lock as
    theirs), else "<user>@<host>:<pid>" - no git, no worktree, so it works
    the same whether or not this process is anywhere near a project."""

    def test_the_global_owner_wins_but_keeps_the_pid(self):
        with mock.patch.dict(autana.os.environ, {autana.OWNER_ENV: "env-owner"}), \
             mock.patch.object(getpass, "getuser", side_effect=AssertionError("unused")), \
             mock.patch.object(autana.os, "getpid", return_value=4242):
            self.assertEqual(autana.owner(), "env-owner:4242")

    def test_the_shape_is_user_at_host_colon_pid(self):
        with mock.patch.dict(autana.os.environ, {}, clear=True), \
             mock.patch.object(getpass, "getuser", return_value="sam"), \
             mock.patch.object(socket, "gethostname", return_value="devbox"), \
             mock.patch.object(autana.os, "getpid", return_value=4242):
            self.assertEqual(autana.owner(), "sam@devbox:4242")

    def test_a_callers_autana_device_owner_is_ignored(self):
        with mock.patch.dict(autana.os.environ, {"AUTANA_DEVICE_OWNER": "ci-7"}, clear=True), \
             mock.patch.object(getpass, "getuser", return_value="sam"), \
             mock.patch.object(socket, "gethostname", return_value="devbox"), \
             mock.patch.object(autana.os, "getpid", return_value=4242):
            self.assertEqual(autana.owner(), "sam@devbox:4242")

    def test_getpass_failure_falls_back_to_user(self):
        with mock.patch.dict(autana.os.environ, {}, clear=True), \
             mock.patch.object(getpass, "getuser", side_effect=OSError("no username")), \
             mock.patch.object(socket, "gethostname", return_value="devbox"), \
             mock.patch.object(autana.os, "getpid", return_value=4242):
            self.assertEqual(autana.owner(), "user@devbox:4242")


class DeviceCommandTests(unittest.TestCase):
    """device_command() is where every board command's `--owner`/`--wait`
    actually reach `device.py`, filled in one place ahead of the subcommand
    word, in place of a flag every command used to parse for itself."""

    def setUp(self):
        patcher = mock.patch.object(autana, "idf_python", return_value="python")
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = mock.patch.object(autana, "device_tool", return_value=Path("device.py"))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_owner_precedes_the_subcommand(self):
        command = autana.device_command("status")
        self.assertEqual(command[command.index("--owner") + 1], autana.owner())
        self.assertLess(command.index("--owner"), command.index("status"))

    def test_no_wait_is_added_by_default(self):
        environ = {key: value for key, value in autana.os.environ.items()
                  if key != autana.WAIT_ENV}
        with mock.patch.dict(autana.os.environ, environ, clear=True):
            command = autana.device_command("status")
        self.assertNotIn("--wait", command)

    def test_the_global_wait_is_forwarded(self):
        with mock.patch.dict(autana.os.environ, {autana.WAIT_ENV: "45"}):
            command = autana.device_command("status")
        self.assertEqual(command[command.index("--wait") + 1], "45")
        self.assertLess(command.index("--wait"), command.index("status"))

    def test_a_callers_own_wait_is_used_when_the_environment_gives_none(self):
        environ = {key: value for key, value in autana.os.environ.items()
                  if key != autana.WAIT_ENV}
        with mock.patch.dict(autana.os.environ, environ, clear=True):
            command = autana.device_command("send", wait=5)
        self.assertEqual(command[command.index("--wait") + 1], "5")

    def test_a_callers_own_wait_wins_over_the_environment(self):
        with mock.patch.dict(autana.os.environ, {autana.WAIT_ENV: "90"}):
            command = autana.device_command("send", wait=5)
        self.assertEqual(command[command.index("--wait") + 1], "5")


class ProjectResolutionTests(unittest.TestCase):
    """resolve_project() - no git, no worktree, no upward search: this
    invocation's own --project (project_override(), set by run_command())
    used as-is, or the current directory itself, like `make -C`. Either way
    the directory named must carry PROJECT_MARKER."""

    def make_project(self, root):
        marker = Path(root) / autana.PROJECT_MARKER
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.touch()

    def test_no_override_uses_the_current_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_project(directory)
            with mock.patch.object(autana, "_project_arg", None), \
                 mock.patch.object(autana.Path, "cwd", return_value=Path(directory)):
                self.assertEqual(autana.resolve_project(), str(Path(directory).resolve()))

    def test_an_explicit_project_is_used_as_is(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_project(directory)
            with mock.patch.object(autana, "_project_arg", directory):
                self.assertEqual(autana.resolve_project(), str(Path(directory).resolve()))

    def test_a_subfolder_given_explicitly_is_not_searched_upward(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_project(directory)
            sub = Path(directory) / "sub"
            sub.mkdir()
            with mock.patch.object(autana, "_project_arg", str(sub)), \
                 self.assertRaises(SystemExit) as caught:
                autana.resolve_project()
            self.assertIn("--project", str(caught.exception))

    def test_the_current_directory_not_a_project_is_refused_naming_project(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(autana, "_project_arg", None), \
                 mock.patch.object(autana.Path, "cwd", return_value=Path(directory)), \
                 self.assertRaises(SystemExit) as caught:
                autana.resolve_project()
            self.assertIn("--project", str(caught.exception))

    def test_a_cwd_that_is_a_subfolder_of_a_project_is_also_refused(self):
        """No upward search of a subfolder either, whether it got there via
        an explicit --project or because that is where this process runs."""
        with tempfile.TemporaryDirectory() as directory:
            self.make_project(directory)
            sub = Path(directory) / "sub"
            sub.mkdir()
            with mock.patch.object(autana, "_project_arg", None), \
                 mock.patch.object(autana.Path, "cwd", return_value=sub), \
                 self.assertRaises(SystemExit) as caught:
                autana.resolve_project()
            self.assertIn("--project", str(caught.exception))


class ProjectOverrideTests(unittest.TestCase):
    """project_override() - the unvalidated form resolve_project() is built
    on, for a command that wants a path for metadata only (suite without
    --flash) and has no reason to require PROJECT_MARKER there."""

    def test_none_when_no_project_was_given(self):
        with mock.patch.object(autana, "_project_arg", None):
            self.assertIsNone(autana.project_override())

    def test_the_raw_value_when_one_was_given(self):
        with mock.patch.object(autana, "_project_arg", "C:/wherever"):
            self.assertEqual(autana.project_override(), "C:/wherever")


class RunCommandTests(unittest.TestCase):
    """run_command() is the one place `--project` is popped - ahead of any
    command's own parsing, and reset once that command returns."""

    def test_project_is_popped_before_the_handler_sees_its_args(self):
        seen = {}

        def handler(args):
            seen["args"] = args
            seen["override"] = autana.project_override()
            return 0

        autana.run_command(handler, ["dev", "--quiet", "--project", "C:/there"])
        self.assertEqual(seen["args"], ["dev", "--quiet"])
        self.assertEqual(seen["override"], "C:/there")

    def test_no_project_flag_leaves_the_override_unset(self):
        def handler(args):
            self.assertIsNone(autana.project_override())
            return 0

        autana.run_command(handler, ["dev"])

    def test_the_override_does_not_leak_to_a_later_call(self):
        def first(args):
            return 0

        def second(args):
            self.assertIsNone(autana.project_override())
            return 0

        autana.run_command(first, ["dev", "--project", "C:/there"])
        autana.run_command(second, ["dev"])


class FlashCommandTests(unittest.TestCase):
    def test_perf_scope_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.flash(["diag", "--quiet", "--perf-scope"])
        command = called.call_args[0][0]
        self.assertIn("--perf-scope", command)

    def test_no_variant_flashes_dev(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called, \
             mock.patch("builtins.print"):
            autana.flash(["--quiet"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--variant") + 1], "dev")

    def test_no_perf_scope_flag_is_not_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.flash(["diag", "--quiet"])
        command = called.call_args[0][0]
        self.assertNotIn("--perf-scope", command)

    def test_the_owner_is_this_processs_own_name(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.flash(["diag", "--quiet"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--owner") + 1], autana.owner())

    def test_an_unknown_flag_is_named_and_rejected(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             self.assertRaises(SystemExit) as caught:
            autana.flash(["diag", "--owner", "delegate-1"])
        self.assertEqual(str(caught.exception), "autana flash: unknown flag --owner")

    def test_project_is_resolved_and_used_for_both_the_banner_and_the_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / autana.PROJECT_MARKER
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.touch()
            with mock.patch.object(autana, "git", return_value=""), \
                 mock.patch.object(autana.subprocess, "call", return_value=0) as called:
                autana.run_command(autana.flash, ["diag", "--quiet", "--project", directory])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--worktree") + 1], str(Path(directory).resolve()))


class BuildCommandTests(unittest.TestCase):
    """autana build: device.py's build half, in this process, taking no
    board and no lock - LockStore and device.py's own command line both
    refuse here."""

    def build(self, *args, code=0):
        device = autana.device_module()
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "git", return_value=""), \
             mock.patch.object(device, "build_worktree", return_value=code) as built, \
             mock.patch.object(device.device_lock, "LockStore",
                               side_effect=AssertionError("lock")), \
             mock.patch.object(autana.subprocess, "call",
                               side_effect=AssertionError("device.py")), \
             mock.patch("builtins.print"):
            returned = autana.build(list(args))
        return returned, built

    def test_each_variant_word_builds_that_variant_and_dev_when_omitted(self):
        for words, variant in (((), "dev"), (("rel",), "release"), (("release",), "release"),
                               (("dev",), "dev"), (("diag",), "diag")):
            with self.subTest(words=words):
                _, built = self.build(*words)
                built.assert_called_once_with("C:/wt", variant, [])

    def test_perf_scope_is_forwarded(self):
        _, built = self.build("diag", "--perf-scope")
        built.assert_called_once_with("C:/wt", "diag", ["--perf-scope"])

    def test_the_exit_status_is_the_builds(self):
        self.assertEqual(self.build("dev", code=2)[0], 2)
        self.assertEqual(self.build("dev", code=0)[0], 0)

    def test_a_second_variant_word_is_refused(self):
        device = autana.device_module()
        with mock.patch.object(device, "build_worktree") as built, \
                self.assertRaises(SystemExit):
            autana.build(["dev", "diag"])
        built.assert_not_called()

    def test_an_unknown_variant_is_refused(self):
        device = autana.device_module()
        with mock.patch.object(device, "build_worktree") as built, \
                self.assertRaises(SystemExit):
            autana.build(["qemu"])
        built.assert_not_called()

    def test_project_is_resolved_and_built(self):
        device = autana.device_module()
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / autana.PROJECT_MARKER
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.touch()
            with mock.patch.object(autana, "git", return_value=""), \
                 mock.patch.object(device, "build_worktree", return_value=0) as built, \
                 mock.patch("builtins.print"):
                autana.run_command(autana.build, ["dev", "--project", directory])
            built.assert_called_once_with(str(Path(directory).resolve()), "dev", [])

    def test_diag_check_runs_the_diag_check_script(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "build_diag_check", return_value=0) as checked:
            code = autana.build(["diag", "--check"])
        self.assertEqual(code, 0)
        checked.assert_called_once_with("C:/wt")

    def test_check_without_diag_is_refused(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "build_diag_check") as checked, \
                self.assertRaises(SystemExit):
            autana.build(["dev", "--check"])
        checked.assert_not_called()

    def test_check_with_a_second_word_is_refused(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "build_diag_check") as checked, \
                self.assertRaises(SystemExit):
            autana.build(["diag", "rel", "--check"])
        checked.assert_not_called()

    def test_check_with_perf_scope_is_refused(self):
        """build_diag_check.sh has no --perf-scope of its own - dropping the
        flag silently would build something other than what was asked for."""
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana, "build_diag_check") as checked, \
                self.assertRaises(SystemExit):
            autana.build(["diag", "--check", "--perf-scope"])
        checked.assert_not_called()


class BuildDiagCheckTests(unittest.TestCase):
    """build_diag_check() shells out to build_diag_check.sh unchanged -
    the ratchet and the diagnostics build, together, no board."""

    def test_runs_the_script_under_git_bash_in_the_worktree(self):
        device = autana.device_module()
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "launcher" / "tools" / "build"
            script.mkdir(parents=True)
            (script / "build_diag_check.sh").write_text("#!/usr/bin/env bash\n")
            with mock.patch.object(device, "git_bash", return_value="bash.exe"), \
                 mock.patch.object(autana.subprocess, "call", return_value=0) as called:
                code = autana.build_diag_check(directory)
        self.assertEqual(code, 0)
        command = called.call_args[0][0]
        self.assertEqual(command[0], "bash.exe")
        self.assertTrue(command[1].endswith("build_diag_check.sh"))
        self.assertEqual(called.call_args.kwargs["cwd"], directory)

    def test_a_missing_script_is_refused(self):
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(SystemExit):
            autana.build_diag_check(directory)


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

    def test_an_unknown_flag_is_named_and_rejected(self):
        with mock.patch.object(autana.subprocess, "Popen") as started, \
             self.assertRaises(SystemExit) as caught:
            autana.monitor(["--follow", "--purpose", "watching a repro"])
        self.assertEqual(str(caught.exception), "autana monitor: unknown flag --purpose")
        started.assert_not_called()

    def test_out_is_forwarded(self):
        with mock.patch.object(autana.subprocess, "Popen") as started:
            autana.monitor(["--follow", "--out", "capture.log"])
        command = started.call_args.args[0]
        self.assertEqual(command[command.index("--out") + 1], "capture.log")


class ResetCommandTests(unittest.TestCase):
    def test_verbose_reaches_reset_capture(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.reset(["--capture", "--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])

    def test_an_unknown_flag_is_named_and_rejected(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called, \
             self.assertRaises(SystemExit) as caught:
            autana.reset(["--owner", "delegate-1"])
        self.assertEqual(str(caught.exception), "autana reset: unknown flag --owner")
        called.assert_not_called()

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
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.selftest(["--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])

    def test_builds_the_device_selftest_invocation(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.selftest([])
        self.assertEqual(code, 0)
        command = called.call_args[0][0]
        self.assertIn("selftest", command)
        self.assertEqual(command[command.index("--worktree") + 1], "C:/wt")
        self.assertEqual(command[command.index("--max-seconds") + 1], "3000.0")

    def test_a_seconds_argument_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.selftest(["120"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--max-seconds") + 1], "120.0")

    def test_an_unknown_flag_is_named_and_rejected(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called, \
             self.assertRaises(SystemExit) as caught:
            autana.selftest(["--owner", "delegate-1"])
        self.assertEqual(str(caught.exception), "autana selftest: unknown flag --owner")
        called.assert_not_called()

    def test_project_is_resolved_and_used(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / autana.PROJECT_MARKER
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.touch()
            with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
                autana.run_command(autana.selftest, ["--project", directory])
            command = called.call_args[0][0]
            self.assertEqual(command[command.index("--worktree") + 1], str(Path(directory).resolve()))

    def test_out_is_forwarded(self):
        """A report script (launcher/tools/device/device_report.sh) needs its own
        capture path, not the command's default `records/`-rooted one."""
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.selftest(["--out", "C:/report/raw.log"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--out") + 1], "C:/report/raw.log")

    def test_out_is_omitted_when_not_given(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.selftest([])
        self.assertNotIn("--out", called.call_args[0][0])

    def test_perf_scope_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.selftest(["--perf-scope"])
        self.assertIn("--perf-scope", called.call_args[0][0])


class BatchCommandTests(unittest.TestCase):
    def test_verbose_reaches_device(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.batch(["run_gfx_suite", "--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])

    def test_one_suite_defaults_runs_and_is_always_diag(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.batch(["run_sand_perf_suite"])
        self.assertEqual(code, 0)
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--suite") + 1], "run_sand_perf_suite")
        self.assertEqual(command[command.index("--runs") + 1], "3")
        self.assertEqual(command[command.index("--variant") + 1], "diag")
        self.assertNotIn("--perf-scope", command)

    def test_several_suites_each_get_their_own_flag(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
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

    def test_out_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.batch(["run_sand_perf_suite", "--runs", "1", "--out", "capture.log"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--out") + 1], "capture.log")

    def test_expect_build_id_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.batch(["run_sand_perf_suite", "--expect-build-id", "abc123-diag"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--expect-build-id") + 1], "abc123-diag")

    def test_project_is_resolved_and_used(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / autana.PROJECT_MARKER
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.touch()
            with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
                autana.run_command(autana.batch, ["run_sand_perf_suite", "--project", directory])
            command = called.call_args[0][0]
            self.assertEqual(command[command.index("--worktree") + 1], str(Path(directory).resolve()))


class SuiteCommandTests(unittest.TestCase):
    def test_verbose_reaches_device(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "--verbose"])
        self.assertIn("--verbose", called.call_args.args[0])

    def test_an_unknown_flag_is_named_and_rejected(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called, \
             self.assertRaises(SystemExit) as caught:
            autana.suite(["run_gfx_suite", "--owner", "delegate-1"])
        self.assertEqual(str(caught.exception), "autana suite: unknown flag --owner")
        called.assert_not_called()

    def test_out_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "--out", "capture.log"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--out") + 1], "capture.log")

    def test_expect_build_id_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "--expect-build-id", "abc123-diag"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--expect-build-id") + 1], "abc123-diag")


class LockCommandTests(unittest.TestCase):
    """status/release/hand/take-back: thin pass-throughs to device.py's own
    lock-level commands."""

    def test_status_takes_no_arguments_and_calls_device(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.status([])
        self.assertEqual(code, 0)
        self.assertEqual(called.call_args[0][0][-1], "status")

    def test_status_json_is_device_json_passed_through(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            code = autana.status(["--json"])
        self.assertEqual(code, 0)
        self.assertEqual(called.call_args[0][0][-2:], ["status", "--json"])

    def test_status_rejects_arguments(self):
        with self.assertRaises(SystemExit):
            autana.status(["extra"])

    def test_release_forwards_the_token(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.release(["deadbeef"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--token") + 1], "deadbeef")

    def test_release_without_a_token_or_the_variable_is_a_usage_error(self):
        environment = {key: value for key, value in autana.os.environ.items()
                       if key != autana.TOKEN_ENV}
        with mock.patch.dict(autana.os.environ, environment, clear=True), \
                self.assertRaises(SystemExit):
            autana.release([])

    def test_release_without_a_token_uses_the_one_a_running_command_has(self):
        with mock.patch.dict(autana.os.environ, {autana.TOKEN_ENV: "cafe"}), \
                mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.release([])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--token") + 1], "cafe")

    def test_release_takes_no_more_than_one_token(self):
        with self.assertRaises(SystemExit):
            autana.release(["one", "two"])

    def test_hand_joins_its_words_into_one_note(self):
        with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.hand(["checking", "the", "panel"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--note") + 1], "checking the panel")

    def test_hand_needs_a_note(self):
        with self.assertRaises(SystemExit):
            autana.hand([])

    def test_hand_forwards_until_back_and_note(self):
        with mock.patch.object(autana.subprocess, "Popen") as called:
            called.return_value.wait.return_value = 3
            code = autana.hand(["--until-back", "10", "enter", "download", "mode"])
        self.assertEqual(code, 3)
        command = called.call_args.args[0]
        after = command[command.index("hand-to-human"):]
        self.assertEqual(after[after.index("--wait") + 1], "10")
        self.assertEqual(command[command.index("--note") + 1], "enter download mode")

    def test_hand_interrupt_returns_child_timeout_status(self):
        with mock.patch.object(autana.subprocess, "Popen") as started, \
             mock.patch("builtins.print") as output:
            started.return_value.wait.side_effect = [KeyboardInterrupt, 3]
            code = autana.hand(["--until-back", "10", "download mode"])
        self.assertEqual(code, 3)
        started.return_value.terminate.assert_not_called()
        output.assert_not_called()

    def test_hand_rejects_until_back_without_seconds(self):
        with self.assertRaises(SystemExit):
            autana.hand(["--until-back", "enter mode"])

    def test_a_leftover_hand_wait_names_until_back(self):
        with self.assertRaises(SystemExit) as stop:
            autana.hand(["--wait", "10", "download mode"])
        self.assertIn("--until-back", str(stop.exception.code))

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


class VersionCommandTests(unittest.TestCase):
    def run_main(self, argv):
        with mock.patch.object(autana.sys, "argv", ["autana", *argv]):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
        return stop.exception.code

    def test_dash_dash_version_prints_the_version_and_exits_zero(self):
        with mock.patch("builtins.print") as printed:
            code = self.run_main(["--version"])
        printed.assert_called_once_with(autana.__version__)
        self.assertEqual(code, 0)

    def test_dash_v_is_the_same_flag(self):
        with mock.patch("builtins.print") as printed:
            code = self.run_main(["-V"])
        printed.assert_called_once_with(autana.__version__)
        self.assertEqual(code, 0)


class GlobalWaitTests(unittest.TestCase):
    """`autana --wait SECONDS <command>` is the only way to set the wait:
    main() hands it to device_command() and every child process through
    one private variable, and a caller's AUTANA_DEVICE_WAIT is ignored."""

    def run_main(self, argv, environ=None, handler=None):
        handler = handler or mock.Mock(return_value=0)
        with mock.patch.dict(autana.os.environ, environ or {}),              mock.patch.dict(autana.COMMANDS, {"status": handler}),              mock.patch.object(autana.sys, "argv", ["autana", *argv]),              mock.patch.object(autana, "idf_python", return_value="python"),              mock.patch.object(autana, "device_tool", return_value=Path("device.py")):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
        return stop.exception.code, handler

    def wait_seen_by_device_step(self, argv, environ=None):
        seen = []

        def handler(args):
            seen.append(autana.device_command("status"))
            return 0

        self.run_main(argv, environ, handler)
        command = seen[0]
        return command[command.index("--wait") + 1] if "--wait" in command else None

    def test_wait_zero_reaches_the_device_step_as_a_zero_wait(self):
        self.assertEqual(self.wait_seen_by_device_step(["--wait", "0", "status"]), "0")

    def test_wait_overrides_the_environment(self):
        seen = self.wait_seen_by_device_step(
            ["--wait", "7", "status"], {"AUTANA_DEVICE_WAIT": "90"})
        self.assertEqual(seen, "7")

    def test_a_callers_autana_device_wait_no_longer_changes_the_wait(self):
        environ = {key: value for key, value in autana.os.environ.items()
                   if key != autana.WAIT_ENV}
        environ["AUTANA_DEVICE_WAIT"] = "90"
        with mock.patch.dict(autana.os.environ, environ, clear=True):
            seen = self.wait_seen_by_device_step(["status"])
        self.assertIsNone(seen)

    def test_a_nested_autana_keeps_the_wait_it_inherited(self):
        seen = self.wait_seen_by_device_step(["status"], {autana.WAIT_ENV: "5"})
        self.assertEqual(seen, "5")

    def test_the_command_receives_its_own_arguments_only(self):
        _, handler = self.run_main(["--wait", "0", "status", "x"])
        handler.assert_called_once_with(["x"])

    def test_a_nested_process_inherits_the_wait(self):
        seen = []

        def handler(args):
            seen.append(autana.subprocess.check_output(
                [sys.executable, "-c",
                 "import os, sys; print(os.environ[sys.argv[1]])", autana.WAIT_ENV],
                text=True).strip())
            return 0

        self.run_main(["--wait", "0", "status"], {"AUTANA_DEVICE_WAIT": "90"}, handler)
        self.assertEqual(seen, ["0"])

    def test_wait_after_the_command_is_rejected_with_the_hint(self):
        for argv in (["status", "--wait", "0"], ["monitor", "5", "--wait", "0"],
                     ["flash", "--wait=0"], ["hand", "--wait", "5", "note"]):
            with self.subTest(argv=argv):
                code, _ = None, None
                with self.assertRaises(SystemExit) as stop,                      mock.patch.object(autana.sys, "argv", ["autana", *argv]):
                    autana.main()
                self.assertIn("--wait goes before the command: autana --wait 0 monitor 5",
                              str(stop.exception.code))

    def test_an_invalid_value_is_rejected(self):
        for value in ("-1", "abc", "1.5", ""):
            with self.subTest(value=value):
                code, handler = self.run_main(["--wait", value, "status"])
                self.assertIn("non-negative integer number of seconds", str(code))
                handler.assert_not_called()

    def test_a_missing_value_is_rejected(self):
        code, _ = self.run_main(["--wait"])
        self.assertIn("non-negative integer number of seconds", str(code))

    def test_help_lists_the_option_once_as_a_global_option(self):
        text = autana.help_text(["flags"])
        self.assertEqual(text.count("--wait SECONDS"), 1)
        self.assertEqual(autana.help_text([]).count("--wait SECONDS"), 1)


class GlobalOwnerTests(unittest.TestCase):
    """`autana --owner NAME <command>` labels this run in the lock: main()
    hands it to owner() and every child process through one private
    variable, and a caller's AUTANA_DEVICE_OWNER is ignored."""

    def run_main(self, argv, environ=None, handler=None):
        handler = handler or mock.Mock(return_value=0)
        with mock.patch.dict(autana.os.environ, environ or {}), \
             mock.patch.dict(autana.COMMANDS, {"status": handler}), \
             mock.patch.object(autana.sys, "argv", ["autana", *argv]), \
             mock.patch.object(autana, "idf_python", return_value="python"), \
             mock.patch.object(autana, "device_tool", return_value=Path("device.py")), \
             mock.patch.object(autana.os, "getpid", return_value=4242):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
        return stop.exception.code, handler

    def owner_seen_by_device_step(self, argv, environ=None):
        seen = []

        def handler(args):
            command = autana.device_command("status")
            seen.append(command[command.index("--owner") + 1])
            return 0

        self.run_main(argv, environ, handler)
        return seen[0]

    def test_owner_reaches_the_lock_record_with_the_pid(self):
        self.assertEqual(
            self.owner_seen_by_device_step(["--owner", "ci-7", "status"]), "ci-7:4242")

    def test_both_globals_work_in_either_order(self):
        seen = []

        def handler(args):
            command = autana.device_command("status")
            seen.append((command[command.index("--owner") + 1],
                         command[command.index("--wait") + 1]))
            return 0

        self.run_main(["--wait", "0", "--owner", "a b", "status"], handler=handler)
        self.run_main(["--owner", "a b", "--wait", "0", "status"], handler=handler)
        self.assertEqual(seen, [("a b:4242", "0")] * 2)

    def test_a_nested_process_inherits_the_owner(self):
        seen = []

        def handler(args):
            seen.append(autana.subprocess.check_output(
                [sys.executable, "-c",
                 "import os, sys; print(os.environ[sys.argv[1]])", autana.OWNER_ENV],
                text=True).strip())
            return 0

        self.run_main(["--owner", "ci-7", "status"], handler=handler)
        self.assertEqual(seen, ["ci-7"])

    def test_a_callers_autana_device_owner_is_ignored(self):
        environ = {key: value for key, value in autana.os.environ.items()
                   if key != autana.OWNER_ENV}
        environ["AUTANA_DEVICE_OWNER"] = "ci-7"
        with mock.patch.dict(autana.os.environ, environ, clear=True):
            seen = self.owner_seen_by_device_step(["status"])
        self.assertNotIn("ci-7", seen)

    def test_owner_after_the_command_is_rejected_with_the_hint(self):
        for argv in (["status", "--owner", "x"], ["flash", "--owner=x"]):
            with self.subTest(argv=argv):
                with self.assertRaises(SystemExit) as stop, \
                     mock.patch.object(autana.sys, "argv", ["autana", *argv]):
                    autana.main()
                self.assertIn("--owner goes before the command: autana --owner ci-7 flash",
                              str(stop.exception.code))

    def test_an_empty_owner_is_rejected(self):
        for value in ("", "  "):
            with self.subTest(value=value):
                code, handler = self.run_main(["--owner", value, "status"])
                self.assertIn("--owner needs a name", str(code))
                handler.assert_not_called()

    def test_a_missing_owner_value_is_rejected(self):
        code, _ = self.run_main(["--owner"])
        self.assertIn("--owner needs a name", str(code))

    def test_help_lists_both_globals_once_each(self):
        for text in (autana.help_text(["flags"]), autana.help_text([])):
            self.assertEqual(text.count("--owner NAME"), 1)
            self.assertEqual(text.count("--wait SECONDS"), 1)


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


class SuiteFlashAndRunsTests(unittest.TestCase):
    """`suite` merged what `batch` used to do on its own: several suites,
    N runs, one lock, an optional flash first - all through device.py's
    own `batch` subcommand, never `run-suite` directly any more."""

    def test_default_is_one_run_with_no_flash(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite"])
        command = called.call_args[0][0]
        self.assertIn("batch", command)
        self.assertEqual(command[command.index("--runs") + 1], "1")
        self.assertIn("--no-flash", command)

    def test_flash_drops_no_flash_and_needs_no_worktree_error(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "--flash"])
        command = called.call_args[0][0]
        self.assertNotIn("--no-flash", command)

    def test_several_suite_names_each_get_their_own_flag(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "run_sand_perf_suite", "--runs", "5"])
        command = called.call_args[0][0]
        self.assertEqual(command.count("--suite"), 2)
        self.assertEqual(command[command.index("--runs") + 1], "5")

    def test_project_without_flash_is_accepted_but_not_resolved(self):
        """`suite` without `--flash` builds nothing, so its `--project` (if
        any) is metadata only - project_override(), not resolve_project(),
        and never validated against PROJECT_MARKER."""
        with mock.patch.object(autana, "resolve_project",
                               side_effect=AssertionError("should not validate")), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.run_command(autana.suite, ["run_gfx_suite", "--project", "C:/anywhere"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--worktree") + 1], "C:/anywhere")

    def test_project_with_flash_is_resolved_and_used(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / autana.PROJECT_MARKER
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.touch()
            with mock.patch.object(autana.subprocess, "call", return_value=0) as called:
                autana.run_command(autana.suite, ["run_gfx_suite", "--flash", "--project", directory])
            command = called.call_args[0][0]
            self.assertEqual(command[command.index("--worktree") + 1], str(Path(directory).resolve()))

    def test_perf_scope_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "--flash", "--perf-scope"])
        self.assertIn("--perf-scope", called.call_args[0][0])

    def test_no_suite_name_is_rejected(self):
        with self.assertRaises(SystemExit):
            autana.suite(["--runs", "3"])

    def test_an_unknown_flag_is_rejected(self):
        with self.assertRaises(SystemExit):
            autana.suite(["run_gfx_suite", "--bogus"])


class SuiteSecondsTests(unittest.TestCase):
    """`suite`'s own per-call timeout, restored - the old `suite <name>
    [seconds]` (600 s default), forwarded as device.py batch's --max-seconds
    bound, the same idea as `selftest [seconds]`."""

    def test_default_max_seconds_is_600(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--max-seconds") + 1], "600.0")

    def test_a_trailing_seconds_argument_is_forwarded(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "120"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--max-seconds") + 1], "120.0")
        self.assertEqual(command.count("--suite"), 1)

    def test_seconds_after_several_suite_names(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "run_sand_perf_suite", "45"])
        command = called.call_args[0][0]
        self.assertEqual(command.count("--suite"), 2)
        self.assertEqual(command[command.index("--max-seconds") + 1], "45.0")

    def test_seconds_survives_alongside_flash_and_runs(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called:
            autana.suite(["run_gfx_suite", "90", "--flash", "--runs", "2"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--max-seconds") + 1], "90.0")
        self.assertEqual(command[command.index("--runs") + 1], "2")


class BatchAliasTests(unittest.TestCase):
    """`autana batch` is the old spelling of `suite ... --flash`; it must
    keep working and say so once, to stderr, before running the new form."""

    def test_prints_the_new_spelling_once_to_stderr(self):
        stream = io.StringIO()
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0), \
             contextlib.redirect_stderr(stream):
            autana.batch(["run_gfx_suite"])
        self.assertIn("autana batch: use `autana suite", stream.getvalue())

    def test_defaults_to_three_runs_and_flashes(self):
        with mock.patch.object(autana, "resolve_project", return_value="C:/wt"), \
             mock.patch.object(autana.subprocess, "call", return_value=0) as called, \
             mock.patch("builtins.print"):
            autana.batch(["run_gfx_suite"])
        command = called.call_args[0][0]
        self.assertEqual(command[command.index("--runs") + 1], "3")
        self.assertNotIn("--no-flash", command)


class LockAndDebugDispatchTests(unittest.TestCase):
    """`lock` and `debug` are thin verb routers; each verb's own behaviour is
    covered where it is defined (LockCommandTests, DeviceVerbCommandTests)."""

    def test_lock_routes_to_its_verb(self):
        fake = mock.Mock(return_value=0)
        with mock.patch.dict(autana.LOCK_VERBS, {"id": fake}):
            code = autana.lock(["id", "--json"])
        fake.assert_called_once_with(["--json"])
        self.assertEqual(code, 0)

    def test_lock_with_no_or_unknown_verb_is_a_usage_error(self):
        with self.assertRaises(SystemExit):
            autana.lock([])
        with self.assertRaises(SystemExit):
            autana.lock(["nope"])

    def test_debug_routes_to_its_verb(self):
        fake = mock.Mock(return_value=0)
        with mock.patch.dict(autana.DEBUG_VERBS, {"freeze": fake}):
            code = autana.debug(["freeze"])
        fake.assert_called_once_with([])
        self.assertEqual(code, 0)

    def test_debug_with_no_or_unknown_verb_is_a_usage_error(self):
        with self.assertRaises(SystemExit):
            autana.debug([])
        with self.assertRaises(SystemExit):
            autana.debug(["nope"])


class RenamedVerbAliasTests(unittest.TestCase):
    """Every bare old spelling in RENAMED_VERBS still works, printing the new
    one to stderr before calling straight through to the same handler."""

    def test_every_alias_forwards_and_announces_the_new_spelling(self):
        for old, (new, handler) in autana.RENAMED_VERBS.items():
            fake = mock.Mock(return_value=0)
            stream = io.StringIO()
            with mock.patch.dict(autana.COMMANDS, {old: autana.alias(old, new, fake)}), \
                    contextlib.redirect_stderr(stream):
                code = autana.COMMANDS[old](["x"])
            fake.assert_called_once_with(["x"])
            self.assertEqual(code, 0)
            self.assertIn(f"autana {old}: use `autana {new}`", stream.getvalue())

    def test_the_real_handlers_are_wired_up(self):
        expected = {"id": autana.identify, "release": autana.release, "hand": autana.hand,
                   "take-back": autana.take_back, "freeze": autana.freeze,
                   "resume": autana.resume, "step": autana.step, "touch": autana.touch,
                   "imu": autana.imu, "framewatch": autana.framewatch}
        self.assertEqual({old: handler for old, (_, handler) in autana.RENAMED_VERBS.items()},
                         expected)


class BoardOnlyCommandsTests(unittest.TestCase):
    """Every command that only talks to the board, never a project, must
    never call resolve_project() - a fresh clone with no launcher/ folder
    can still `autana tap 1 2` or `autana suite <name>` against whatever is
    already on the board."""

    def run_board_only(self, handler, args):
        status = mock.Mock(stdout='{"boards": []}', stderr="", returncode=0)
        popen = mock.Mock()
        popen.wait.return_value = 0
        with mock.patch.object(autana, "resolve_project",
                               side_effect=AssertionError("must not resolve a project")), \
             mock.patch.object(autana, "send", return_value=(0, ["OK"])), \
             mock.patch.object(autana.subprocess, "call", return_value=0), \
             mock.patch.object(autana.subprocess, "run", return_value=status), \
             mock.patch.object(autana.subprocess, "Popen", return_value=popen), \
             mock.patch.object(autana.sys.stdout, "isatty", return_value=False), \
             mock.patch("builtins.print"):
            return autana.run_command(handler, args)

    def test_board_only_commands_never_resolve_a_project(self):
        cases = [
            (autana.monitor, ["--follow"]),
            (autana.reset, []),
            (autana.status, []),
            (autana.buildid, []),
            (autana.tune, ["ridge_trail"]),
            (autana.tap, ["1", "2"]),
            (autana.press, ["1", "2"]),
            (autana.drag, ["1", "2", "3", "4", "100"]),
            (autana.button, ["boot"]),
            (autana.screenshot, []),
            (autana.freeze, []),
            (autana.step, []),
            (autana.resume, []),
            (autana.apps, []),
            (autana.open_app, ["star"]),
            (autana.home, []),
            (autana.lock, ["id"]),
            (autana.debug, ["freeze"]),
            (autana.suite, ["run_gfx_suite"]),
        ]
        for handler, args in cases:
            with self.subTest(command=handler.__name__, args=args):
                self.run_board_only(handler, args)


class GlobalBoardTests(unittest.TestCase):
    """`autana --board SERIAL <command>` names the board, through one private
    variable every child reads; a caller's AUTANA_BOARD is ignored."""

    def run_main(self, argv, handler=None, environ=None):
        handler = handler or mock.Mock(return_value=0)
        with mock.patch.dict(autana.os.environ, environ or {}), \
             mock.patch.dict(autana.COMMANDS, {"status": handler}), \
             mock.patch.object(autana.sys, "argv", ["autana", *argv]):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
        return stop.exception.code, handler

    def test_a_nested_process_inherits_the_board(self):
        seen = []

        def handler(args):
            seen.append(autana.subprocess.check_output(
                [sys.executable, "-c",
                 "import os, sys; print(os.environ[sys.argv[1]])", autana.BOARD_ENV],
                text=True).strip())
            return 0

        self.run_main(["--board", "90:70:69:FE:A3:08", "status"], handler)
        self.assertEqual(seen, ["90:70:69:FE:A3:08"])

    def test_a_callers_autana_board_is_ignored(self):
        seen = []
        environ = {key: value for key, value in autana.os.environ.items()
                   if key != autana.BOARD_ENV}
        environ["AUTANA_BOARD"] = "90:70:69:FE:A3:08"
        with mock.patch.dict(autana.os.environ, environ, clear=True):
            self.run_main(["status"], lambda args: seen.append(
                autana.os.environ.get(autana.BOARD_ENV)) or 0)
        self.assertEqual(seen, [None])

    def test_board_after_the_command_is_rejected_with_the_hint(self):
        with self.assertRaises(SystemExit) as stop, \
             mock.patch.object(autana.sys, "argv", ["autana", "status", "--board", "x"]):
            autana.main()
        self.assertIn("--board goes before the command", str(stop.exception.code))

    def test_an_empty_board_is_rejected(self):
        code, handler = self.run_main(["--board", " ", "status"])
        self.assertIn("--board needs a board's USB serial number", str(code))
        handler.assert_not_called()


class ProjectSettingsTests(unittest.TestCase):
    """Every command reads the settings file of the project it acts on, and
    refuses to run on one it cannot trust."""

    def run_main(self, argv, handler):
        with mock.patch.dict(autana.COMMANDS, {"status": handler}), \
             mock.patch.object(autana.sys, "argv", ["autana", *argv]):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
        return stop.exception.code

    def test_children_are_told_which_project_the_command_acts_on(self):
        seen = []
        with isolation.project() as project:
            self.run_main(["status", "--project", str(project)], lambda args: seen.append(
                autana.os.environ[autana.PROJECT_ENV]) or 0)
        self.assertEqual(seen, [str(project.resolve())])

    def test_an_unknown_key_stops_the_command_naming_the_file_and_the_key(self):
        handler = mock.Mock(return_value=0)
        with isolation.project() as project:
            (project / "autana.local.toml").write_text("recods = 'x'\n")
            code = self.run_main(["status", "--project", str(project)], handler)
        self.assertIn("autana.local.toml", str(code))
        self.assertIn("recods", str(code))
        handler.assert_not_called()

    def test_help_config_lists_every_key(self):
        text = autana.help_text(["config"])
        for key in autana.autana_config.KEYS:
            self.assertIn(key, text)

    def test_help_config_works_when_the_project_file_is_broken(self):
        with isolation.project() as project:
            (project / "autana.local.toml").write_text("recods = 'x'\n")
            with mock.patch.object(autana.sys, "argv", ["autana", "help", "config"]), \
                 contextlib.redirect_stdout(io.StringIO()) as printed, \
                 self.assertRaises(SystemExit) as stop:
                autana.main()
        self.assertEqual(stop.exception.code, 0)
        self.assertIn("records", printed.getvalue())


if __name__ == "__main__":
    unittest.main()
