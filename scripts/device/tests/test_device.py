import base64
import io
import contextlib
import gzip
import json
import os
import struct
import subprocess
import tempfile
import time
import unittest
import zlib
from argparse import Namespace
from datetime import datetime
from pathlib import Path
import sys
from unittest import mock

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
import device
import device_lock
import device_report


class InterpreterTests(unittest.TestCase):
    """Any interpreter may start device.py - a report script's `python`, a
    person at a prompt - and only ESP-IDF's carries pyserial."""

    MISSING = {"serial": None}
    PRESENT = {"serial": mock.MagicMock()}

    def test_without_pyserial_it_runs_itself_again_under_idf_python(self):
        with mock.patch.dict(sys.modules, self.MISSING), \
                mock.patch.object(device, "idf_python", return_value="C:/idf/python.exe"), \
                mock.patch.object(device.subprocess, "call", return_value=3) as call:
            status = device.rerun_under_idf_python(["status"])
        self.assertEqual(status, 3)
        self.assertEqual(call.call_args.args[0][0], "C:/idf/python.exe")
        self.assertEqual(call.call_args.args[0][-1], "status")

    def test_it_does_not_rerun_when_idf_python_is_this_interpreter(self):
        with mock.patch.dict(sys.modules, self.MISSING), \
                mock.patch.object(device, "idf_python", return_value=sys.executable), \
                mock.patch.object(device.subprocess, "call") as call:
            status = device.rerun_under_idf_python(["status"])
        self.assertIsNone(status)
        call.assert_not_called()

    def test_with_pyserial_it_runs_where_it_is(self):
        with mock.patch.dict(sys.modules, self.PRESENT), \
                mock.patch.object(device.subprocess, "call") as call:
            status = device.rerun_under_idf_python(["status"])
        self.assertIsNone(status)
        call.assert_not_called()


class PortWaitTests(unittest.TestCase):
    """A caller that won the lock must outwait a straggler still holding the
    port, and must say so rather than failing as if the board were flaky."""

    def setUp(self):
        self.clock = [0.0]
        self.slept = []

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.clock[0] += seconds

    def opener(self, failures):
        remaining = [failures]

        def open_port(unused_port):
            if remaining[0] > 0:
                remaining[0] -= 1
                raise OSError(13, "Access is denied")
            return mock.Mock()

        return open_port

    def test_a_port_that_is_free_is_not_waited_for(self):
        device.open_when_free("COM5", 120, self.opener(0), self.sleep, lambda: self.clock[0])
        self.assertEqual(self.slept, [])

    def test_returns_the_connection_without_closing_it(self):
        connection = mock.Mock()
        result = device.open_when_free("COM5", 120, lambda unused_port: connection,
                                       self.sleep, lambda: self.clock[0])
        self.assertIs(result, connection)
        connection.close.assert_not_called()

    def test_a_straggler_is_waited_out(self):
        device.open_when_free("COM5", 120, self.opener(3), self.sleep, lambda: self.clock[0])
        self.assertEqual(self.slept, [1.0, 1.0, 1.0])

    def test_a_port_nobody_frees_reports_at_the_deadline(self):
        with self.assertRaises(RuntimeError) as caught:
            device.open_when_free("COM5", 5, self.opener(99), self.sleep, lambda: self.clock[0])
        self.assertIn("COM5", str(caught.exception))
        self.assertIn("wait ran out", str(caught.exception))

    def test_reset_reenumeration_reason_reaches_the_timeout(self):
        with self.assertRaisesRegex(RuntimeError, "re-enumerating after reset"):
            device.open_when_free("COM5", 0, self.opener(1), self.sleep,
                                  lambda: self.clock[0], "re-enumerating after reset")


class FakeConnection:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []

    def read(self, unused_size):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.writes.append(data)

    def close(self):
        pass

    def flush(self):
        pass

    def reset_input_buffer(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, unused_type, unused_value, unused_traceback):
        pass


class AnswersNoQuery(FakeConnection):
    """A release image: it prints its boot log but answers no console query."""

    def read(self, size):
        return b"" if self.writes else super().read(size)


class InterruptedConnection(FakeConnection):
    def read(self, size):
        if not self.chunks:
            raise KeyboardInterrupt
        return super().read(size)


class DeviceTests(unittest.TestCase):
    def test_suite_output_shows_failure_messages_and_caps_the_list(self):
        data = (b"boot detail\n" + b":1:good:PASS\n" +
                b"".join(f"file.c:{line}:bad_{line}:FAIL: wrong {line}\n".encode()
                         for line in range(1, 13)))
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "suite", "complete", False)
        lines = [call.args[0] for call in printed.call_args_list]
        self.assertIn("suite results: 1 PASS, 12 FAIL", lines)
        self.assertIn("bad_1: wrong 1", lines)
        self.assertIn("2 more in the capture; by suite:", lines)
        self.assertIn("  file: 12 FAIL", lines)
        self.assertNotIn("boot detail", "\n".join(lines))

    def test_failures_past_the_cap_are_counted_per_suite(self):
        data = b"".join(
            [f"/p/tests/suite_sand_scenes.c:{n}:a_{n}:FAIL: x\n".encode() for n in range(9)] +
            [f"C:\\w\\suite_gfx.c:{n}:b_{n}:FAIL: y\n".encode() for n in range(3)] +
            [b"/p/suite_gfx.c:99:ok:PASS\n"])
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "suite", "complete", False)
        lines = [call.args[0] for call in printed.call_args_list]
        by_suite = lines[lines.index("2 more in the capture; by suite:") + 1:][:2]
        self.assertEqual(by_suite, ["  suite_sand_scenes: 9 FAIL", "  suite_gfx: 3 FAIL"])

    def test_under_the_cap_there_is_no_per_suite_block(self):
        data = b"/p/suite_gfx.c:1:b:FAIL: y\n"
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "suite", "complete", False)
        self.assertFalse(any("by suite" in call.args[0] for call in printed.call_args_list))

    def test_a_passing_run_still_names_its_capture(self):
        data = b"boot detail\n:1:good:PASS\n"
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "suite", "complete", False)
        lines = [call.args[0] for call in printed.call_args_list]
        self.assertIn("suite capture: record.log", lines)
        self.assertNotIn("boot detail", "\n".join(lines))

    def test_suite_output_pass_and_verbose_capture(self):
        data = b"boot detail\n:1:good:PASS\n"
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "selftest", "complete", True)
        lines = [call.args[0] for call in printed.call_args_list]
        self.assertIn("boot detail\n:1:good:PASS\n", lines)
        self.assertIn("selftest results: 1 PASS, 0 FAIL", lines)

    def test_reset_output_shows_only_error_lines_by_default(self):
        data = b"boot detail\nE (4) boot: failed to mount\npanic: halted\n"
        with mock.patch("builtins.print") as printed:
            device.print_reset_output(data, False)
        lines = [call.args[0] for call in printed.call_args_list]
        self.assertEqual(lines, ["E (4) boot: failed to mount", "panic: halted"])

    def test_reset_output_verbose_shows_full_capture(self):
        data = b"boot detail\nabort() was called\n"
        with mock.patch("builtins.print") as printed:
            device.print_reset_output(data, True)
        printed.assert_called_once_with(data.decode(), end="")

    def test_count_suite_results_accepts_unity_failure_messages(self):
        data = b""":601:test_one:PASS
:602:test_two:PASS
:603:test_three:PASS
:604:test_four:PASS
:605:test_five:PASS
:606:test_six:PASS
:607:test_seven:PASS
:623:test_a_timed_out_job_falls_back_inline:FAIL: Expected 1 Was 0
"""
        self.assertEqual(device.count_suite_results(data), (7, 1))

    def test_capture_rejects_build_id_seen_after_expected_id(self):
        connection = FakeConnection([
            b"BUILD_ID=expected\n",
            b"rebooting\nBUILD_ID=different\n",
        ])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            with self.assertRaisesRegex(RuntimeError, "got different"):
                device.capture(connection, output, 1, None, "expected")

    def test_capture_stops_at_single_suite_completion_and_counts_results(self):
        connection = FakeConnection([
            b":1:test_one:PASS\nSUITE_DONE sand\n",
            b"late output\n",
        ])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            data, reason = device.capture(connection, output, 1, None, suite_name="sand")
        self.assertEqual(reason, "complete")
        self.assertEqual(device.count_suite_results(data), (1, 0))
        self.assertNotIn(b"late output", data)

    def test_capture_stops_at_the_shells_own_completion_line(self):
        connection = FakeConnection([
            b":1:test_one:PASS\n\nRUNSUITE_COMPLETE name=sand found=1\n",
            b"I (1) shell: 12.0 fps\n",
        ])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            data, reason = device.capture(connection, output, 1, None, suite_name="sand")
        self.assertEqual(reason, "complete")
        self.assertNotIn(b"12.0 fps", data)

    def test_capture_ignores_another_suites_completion_line(self):
        connection = FakeConnection([b"RUNSUITE_COMPLETE name=other found=1\n"])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            unused_data, reason = device.capture(connection, output, 0.2, None,
                                                 suite_name="sand")
        self.assertEqual(reason, "timeout")

    def test_capture_keeps_what_it_read_when_the_port_goes_away(self):
        class VanishingConnection(FakeConnection):
            def read(self, size):
                if not self.chunks:
                    raise OSError(22, "ClearCommError failed")
                return super().read(size)

        connection = VanishingConnection([b":1:test_one:PASS\n"])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            data, reason = device.capture(connection, output, 1, None, suite_name="sand")
            self.assertEqual(output.read_bytes(), b":1:test_one:PASS\n")
        self.assertEqual(reason, "port lost")
        self.assertEqual(device.count_suite_results(data), (1, 0))

    def test_capture_echoes_every_chunk_byte_for_byte(self):
        chunks = [b"ordinary\n", b"\xfferror: broken\n"]
        echo = io.BytesIO()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            device.capture(FakeConnection(chunks), output, 0.2, None, echo=echo)
            self.assertEqual(output.read_bytes(), b"".join(chunks))
        self.assertEqual(echo.getvalue(), b"".join(chunks))

    def test_capture_flushes_bytes_before_interrupt(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            with self.assertRaises(KeyboardInterrupt):
                device.capture(InterruptedConnection([b"before\n"]), output, None, None)
            self.assertEqual(output.read_bytes(), b"before\n")

    def test_regular_capture_still_raises_on_interrupt(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            with self.assertRaises(KeyboardInterrupt):
                device.capture(InterruptedConnection([b"before\n"]), output, 1, None)
            self.assertEqual(output.read_bytes(), b"before\n")

    def test_monitor_capture_ignores_test_completion(self):
        chunks = [b"TESTS_DONE\n", b"ordinary after tests\n"]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            with self.assertRaises(KeyboardInterrupt):
                device.capture(InterruptedConnection(chunks), output,
                               None, None, until_tests_done=False)
            self.assertEqual(output.read_bytes(), b"".join(chunks))

    def test_capture_rejects_run_suite_when_build_has_no_suites(self):
        connection = FakeConnection([b"shell: ignoring line: 'RUNSUITE sand'\n"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "no test suites.*flash --variant diag"):
                device.capture(connection, Path(directory) / "capture.log", 1, None,
                               suite_name="sand")

    def test_capture_rejects_unknown_suite(self):
        connection = FakeConnection([b"no suite named 'sand'\n"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "no suite named sand"):
                device.capture(connection, Path(directory) / "capture.log", 1, None,
                               suite_name="sand")

    def test_run_suite_prints_result_counts(self):
        connection = FakeConnection([b":1:test_one:PASS\nSUITE_DONE sand\n"])
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand",
                         out=None, max_seconds=1, idle_seconds=None, expect_build_id=None)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch.object(device, "records_root", return_value=Path(directory)), \
             mock.patch("builtins.print") as output:
            args.out = str(Path(directory) / "capture.log")
            self.assertEqual(device.run_suite(args, store, "COM5"), 0)
        output.assert_any_call("suite results: 1 PASS, 0 FAIL")

    def test_run_suite_returns_failure_status(self):
        connection = FakeConnection([
            b":623:test_a_timed_out_job_falls_back_inline:FAIL: Expected 1 Was 0\n"
            b"SUITE_DONE sand\n"
        ])
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand",
                         out=None, max_seconds=1, idle_seconds=None, expect_build_id=None)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch.object(device, "records_root", return_value=Path(directory)):
            args.out = str(Path(directory) / "capture.log")
            self.assertEqual(device.run_suite(args, store, "COM5"), 1)

    def test_boot_build_id_queries_console_when_boot_has_no_id(self):
        connection = FakeConnection([b"boot\nTESTS_DONE\n", b"BUILD_ID=reply\n"])
        with mock.patch.object(device, "open_serial", return_value=connection):
            actual, unused_reason = device.boot_build_id("COM5", seconds=1)
        self.assertEqual(actual, "reply")
        self.assertEqual(connection.writes, [b"BUILDID\n"])

    def test_replies_are_found_behind_log_prefixes_and_end_at_a_terminator(self):
        data = (b"I (812) shell: frame 16 ms\n"
                b"TUNE launcher.ridge_trail=226 min=0 max=255\n"
                b"I (813) screenshot: TUNE launcher.glow_radius=13 min=1 max=31\r\n"
                b"TUNE_END count=2\n"
                b"TUNE_OK later=1\n")
        found, complete = device.replies_to(data, "TUNE", ["TUNE_OK", "TUNE_ERR", "TUNE_END"])
        self.assertTrue(complete)
        self.assertEqual(found, ["TUNE launcher.ridge_trail=226 min=0 max=255",
                                 "TUNE launcher.glow_radius=13 min=1 max=31",
                                 "TUNE_END count=2"])

    def test_replies_are_incomplete_until_the_terminator_arrives(self):
        found, complete = device.replies_to(b"TUNE a=1 min=0 max=2\nTUNE_EN", "TUNE",
                                            ["TUNE_OK", "TUNE_ERR", "TUNE_END"])
        self.assertFalse(complete)
        self.assertEqual(found, ["TUNE a=1 min=0 max=2", "TUNE_EN"])

    def send_args(self, line):
        return Namespace(owner="agent", purpose="send", wait=0, line=line, reply="TUNE",
                         until=["TUNE_OK", "TUNE_ERR", "TUNE_END"], seconds=1, optional=False)

    def test_send_writes_the_line_and_prints_the_reply(self):
        connection = FakeConnection([b"I (5) shell: x\nTUNE_OK launcher.ridge_trail=200\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print") as printed:
            status = device.send(self.send_args("SET launcher.ridge_trail 200"), store, "COM5")
        self.assertEqual(status, 0)
        self.assertEqual(connection.writes, [b"\nSET launcher.ridge_trail 200\n"])
        printed.assert_called_once_with("TUNE_OK launcher.ridge_trail=200")

    def test_send_fails_on_an_error_reply(self):
        connection = FakeConnection([b"TUNE_ERR range launcher.ridge_trail takes 0..255\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print"):
            self.assertEqual(device.send(self.send_args("SET launcher.ridge_trail 999"), store, "COM5"), 1)

    def test_send_says_so_when_the_build_has_no_such_command(self):
        connection = FakeConnection([b"I (9) screenshot: ignoring line: 'TUNE'\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with mock.patch.object(device, "open_when_free", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "needs a development build"):
                device.send(self.send_args("TUNE"), store, "COM5")

    def test_send_forwards_an_app_command_and_completes_on_its_own_end_line(self):
        """autana console's own case (autana.py's console()): an app's
        reply always starts with its own prefix in capitals, so send()
        completes as soon as that prefix's own _END arrives rather than
        waiting out the window."""
        connection = FakeConnection([b"EXAMPLE status=ok\n", b"EXAMPLE_END\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="example status",
                         reply="EXAMPLE", until=["EXAMPLE_END", "EXAMPLE_ERR"], seconds=1, optional=True)
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print") as printed:
            status = device.send(args, store, "COM5")
        self.assertEqual(status, 0)
        printed.assert_called_once_with("EXAMPLE status=ok\nEXAMPLE_END")

    def test_send_forwards_an_app_command_and_fails_on_its_own_err_line(self):
        connection = FakeConnection([b"EXAMPLE_ERR not running\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="example status",
                         reply="EXAMPLE", until=["EXAMPLE_END", "EXAMPLE_ERR"], seconds=1, optional=True)
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print"):
            status = device.send(args, store, "COM5")
        self.assertEqual(status, 1)

    def test_send_optional_treats_silence_as_success(self):
        """TOUCH/IMU answer only when something is wrong - a timeout with
        nothing seen is that verb's normal happy path, not a failure."""
        connection = FakeConnection([b"I (1) shell: unrelated log line\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="TOUCH down 1 2",
                         reply="TOUCH", until=["TOUCH"], seconds=0.05, optional=True)
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print") as printed:
            status = device.send(args, store, "COM5")
        self.assertEqual(status, 0)
        printed.assert_called_once_with("")

    def test_send_optional_still_prints_a_device_side_warning(self):
        connection = FakeConnection([b"W (2) console: TOUCH wants <down|up> <x> <y>: 'bad'\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="TOUCH bad",
                         reply="TOUCH", until=["TOUCH"], seconds=0.5, optional=True)
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print") as printed:
            status = device.send(args, store, "COM5")
        self.assertEqual(status, 0)
        printed.assert_called_once_with("TOUCH wants <down|up> <x> <y>: 'bad'")

    def test_send_without_optional_still_raises_on_silence(self):
        connection = FakeConnection([b"I (1) shell: unrelated log line\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="TUNE", reply="TUNE",
                         until=["TUNE_OK", "TUNE_ERR", "TUNE_END"], seconds=0.05, optional=False)
        with mock.patch.object(device, "open_when_free", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "no reply"):
                device.send(args, store, "COM5")

    def test_status_reports_human_note_and_age(self):
        status = {"human": {"owner": "maintainer", "note": "panel", "since_at": 1000},
                  "lock": None, "queue": []}
        with mock.patch.object(device_lock.time, "time", return_value=1065), \
             mock.patch("builtins.print") as output:
            device_lock.print_status(status)
        output.assert_called_once_with("human reservation: maintainer: panel (65s ago)")

    def test_take_back_clears_human_reservation_and_prints_status(self):
        store = mock.Mock()
        store.status.return_value = {"human": None, "lock": None, "queue": []}
        with mock.patch.object(device.device_lock, "LockStore", return_value=store), \
             mock.patch("builtins.print") as output:
            self.assertEqual(device.main(["--port", "COM5", "take-back"]), 0)
        store.clear_human.assert_called_once_with("COM5")
        output.assert_called_once_with("unlocked")


class SlugTests(unittest.TestCase):
    def test_replaces_unsafe_characters_with_a_single_dash(self):
        self.assertEqual(device.slug("agent a/b:c"), "agent-a-b-c")

    def test_collapses_runs_and_strips_leading_and_trailing_dashes(self):
        self.assertEqual(device.slug("  --weird!!name--  "), "weird-name")

    def test_falls_back_to_unknown_for_input_with_no_safe_characters(self):
        self.assertEqual(device.slug("   "), "unknown")


class RecordsRootTests(unittest.TestCase):
    def setUp(self):
        self.named = os.environ.pop("AUTANA_RECORDS", None)

    def tearDown(self):
        if self.named is not None:
            os.environ["AUTANA_RECORDS"] = self.named
        else:
            os.environ.pop("AUTANA_RECORDS", None)

    def test_defaults_into_the_checkout_regardless_of_cwd(self):
        expected = Path(device.__file__).resolve().parents[2] / ".records" / "device"
        self.assertEqual(device.records_root(), expected)

    def test_autana_records_names_it_instead(self):
        os.environ["AUTANA_RECORDS"] = os.path.join("somewhere", "else")
        self.assertEqual(device.records_root(), Path("somewhere") / "else")

    def test_an_empty_autana_records_is_no_setting_at_all(self):
        """An exported-but-empty variable is what a shell leaves behind after
        a failed assignment - a records root of "" would otherwise resolve to
        the working directory."""
        os.environ["AUTANA_RECORDS"] = ""
        expected = Path(device.__file__).resolve().parents[2] / ".records" / "device"
        self.assertEqual(device.records_root(), expected)


class ResolveCapturePathTests(unittest.TestCase):
    def test_an_explicit_out_is_returned_unchanged_and_not_created(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "nested" / "capture.log"
            path, managed = device.resolve_capture_path(
                str(out), "listen", "agent", datetime(2026, 9, 16, 12, 30, 45))
        self.assertEqual(path, out)
        self.assertFalse(managed)
        self.assertFalse(out.parent.is_dir())

    def test_the_default_path_is_built_from_kind_and_owner_and_its_directory_is_made(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            path, managed = device.resolve_capture_path(
                None, "runsuite-sand", "agent a", datetime(2026, 9, 16, 12, 30, 45), root=root)
            self.assertTrue(managed)
            self.assertEqual(path, root / "20260916" / "123045_runsuite-sand_agent-a.log")
            self.assertTrue(path.parent.is_dir())


class GitCommitTests(unittest.TestCase):
    def test_returns_none_when_the_directory_is_not_a_git_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(device.git_commit(directory))


class RecordCaptureTests(unittest.TestCase):
    def test_appends_an_index_line_and_leaves_an_explicit_out_uncompressed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            out = Path(directory) / "capture.log"
            out.write_bytes(b"x" * (device.COMPRESS_ABOVE_BYTES + 1))
            started_at = datetime(2026, 9, 16, 12, 30, 45)
            path = device.record_capture(
                out, False, started_at=started_at, port="COM5", owner="agent",
                purpose="listen", command="listen", commit="deadbeef", reason="complete",
                error=None, root=root)
            self.assertEqual(path, out)
            self.assertTrue(out.is_file())
            lines = (root / "index.jsonl").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            self.assertEqual(json.loads(lines[0]), {
                "started_at": started_at.isoformat(),
                "port": "COM5",
                "owner": "agent",
                "purpose": "listen",
                "command": "listen",
                "suite": None,
                "build_id": None,
                "worktree": None,
                "commit": "deadbeef",
                "reason": "complete",
                "error": None,
                "capture_path": str(out),
                "capture_bytes": device.COMPRESS_ABOVE_BYTES + 1,
            })

    def test_compresses_a_managed_capture_above_the_threshold(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            root.mkdir()
            log = root / "capture.log"
            payload = b"x" * (device.COMPRESS_ABOVE_BYTES + 1)
            log.write_bytes(payload)
            path = device.record_capture(
                log, True, started_at=datetime(2026, 9, 16, 12, 30, 45), port="COM5",
                owner="agent", purpose="run suite", command="run-suite", suite="sand",
                commit=None, reason="complete", error=None, root=root)
            self.assertEqual(path, log.with_name("capture.log.gz"))
            self.assertFalse(log.exists())
            with gzip.open(path, "rb") as stream:
                self.assertEqual(stream.read(), payload)
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            self.assertEqual(entry["capture_path"], str(path))
            self.assertEqual(entry["capture_bytes"], path.stat().st_size)

    def test_leaves_a_managed_capture_at_the_threshold_uncompressed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            root.mkdir()
            log = root / "capture.log"
            log.write_bytes(b"x" * device.COMPRESS_ABOVE_BYTES)
            path = device.record_capture(
                log, True, started_at=datetime(2026, 9, 16, 12, 30, 45), port="COM5",
                owner="agent", purpose="run suite", command="run-suite", suite="sand",
                commit=None, reason="complete", error=None, root=root)
            self.assertEqual(path, log)
            self.assertTrue(log.is_file())


class RunSuiteDefaultPathTests(unittest.TestCase):
    def test_without_out_uses_the_default_path_and_writes_an_index_line(self):
        connection = FakeConnection([b":1:test_one:PASS\nSUITE_DONE sand\n"])
        args = Namespace(owner="agent a", purpose="test", wait=0, suite="sand", out=None,
                         max_seconds=1, idle_seconds=None, expect_build_id=None)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            fixed_now = datetime(2026, 9, 16, 12, 30, 45)
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "now", return_value=fixed_now), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                self.assertEqual(device.run_suite(args, store, "COM5"), 0)
            expected_log = root / "20260916" / "123045_runsuite-sand_agent-a.log"
            self.assertTrue(expected_log.is_file())
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            self.assertEqual(entry["capture_path"], str(expected_log))
            self.assertEqual(entry["command"], "run-suite")
            self.assertEqual(entry["suite"], "sand")
            self.assertEqual(entry["reason"], "complete")
            self.assertIsNone(entry["error"])
            self.assertEqual(entry["commit"], "deadbeef")

    def test_a_mid_capture_runtime_error_is_still_recorded_before_it_propagates(self):
        connection = FakeConnection([
            b"BUILD_ID=expected\n",
            b"rebooting\nBUILD_ID=different\n",
        ])
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand", out=None,
                         max_seconds=1, idle_seconds=None, expect_build_id="expected")
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value=None):
                with self.assertRaisesRegex(RuntimeError, "got different"):
                    device.run_suite(args, store, "COM5")
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            self.assertIsNotNone(entry["error"])
            self.assertIn("got different", entry["error"])
            self.assertTrue(Path(entry["capture_path"]).is_file())


def build_prints(build_id):
    """A subprocess.run standing in for build_flash.sh, which prints the id
    of the image it built into the flash log."""
    def run(*unused, **keywords):
        keywords["stdout"].write(("BUILD_ID=" + build_id + "\n").encode("ascii"))
    return run


class FlashVerificationTests(unittest.TestCase):
    """flash() restarts the board, then checks the image that boots against
    the id the build printed - not a build directory it would have to guess."""

    def flash(self, build_output, board_output, events=None, after_watchdog=None):
        """board_output is what the board says after the RTS reset - b"" for
        a chip still in download mode - and after_watchdog what it says once
        the watchdog has restarted it."""
        events = [] if events is None else events
        said = {"hard_reset": board_output, "watchdog_reset": after_watchdog}
        last_reset = ["hard_reset"]

        def run(*unused, **keywords):
            events.append("build")
            keywords["stdout"].write(build_output)

        def reset(port, after="hard_reset"):
            events.append("reset " + port + " " + after)
            last_reset[0] = after

        def opened(*unused, **unused_keywords):
            events.append("open")
            output = said[last_reset[0]]
            return AnswersNoQuery([output] if output else [])

        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "engine"
            (worktree / "launcher" / "tools").mkdir(parents=True)
            (worktree / "launcher" / "tools" / "build_flash.sh").write_text("")
            args = Namespace(owner="agent", purpose="flash", wait=0, variant="release",
                             worktree=str(worktree), out=None)
            store = mock.Mock()
            store.acquire.return_value = {"log": "", "token": "token"}
            with mock.patch.object(device, "records_root", return_value=Path(directory)), \
                 mock.patch.object(device.subprocess, "run", side_effect=run), \
                 mock.patch.object(device, "reset", side_effect=reset), \
                 mock.patch.object(device, "open_when_free", side_effect=opened), \
                 mock.patch.object(device, "open_serial", side_effect=opened), \
                 mock.patch.object(device, "RESET_FIRST_BYTE_SECONDS", 0.05), \
                 mock.patch.object(device, "RESET_REOPEN_SECONDS", 0.2), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                return device.flash(args, store, "COM5"), store

    def test_restarts_the_board_after_the_build_and_before_reading_it(self):
        events = []
        self.flash(b"BUILD_ID=built\n", b"BUILD_ID=built\nTESTS_DONE\n", events)
        self.assertEqual(events[events.index("build"):][:3],
                         ["build", "reset COM5 hard_reset", "open"])

    def test_a_board_heard_after_the_rts_reset_is_not_restarted_again(self):
        events = []
        self.flash(b"BUILD_ID=built\n", b"BUILD_ID=built\nTESTS_DONE\n", events)
        self.assertNotIn("reset COM5 watchdog_reset", events)

    def test_a_board_silent_after_the_flash_is_restarted_through_the_watchdog(self):
        events = []
        verified, unused_store = self.flash(b"BUILD_ID=built\n", b"", events,
                                            after_watchdog=b"BUILD_ID=built\nTESTS_DONE\n")
        self.assertIn("reset COM5 watchdog_reset", events)
        self.assertEqual(verified, "built")

    def test_verifies_against_the_id_the_build_printed(self):
        verified, store = self.flash(b"noise\nBUILD_ID=built\n", b"BUILD_ID=built\nTESTS_DONE\n")
        self.assertEqual(verified, "built")
        store.set_expected_build_id.assert_called_once_with("COM5", "token", "built")

    def test_a_different_image_on_the_board_is_a_mismatch(self):
        with self.assertRaisesRegex(RuntimeError, "expected built, got other"):
            self.flash(b"BUILD_ID=built\n", b"BUILD_ID=other\nTESTS_DONE\n")

    def test_a_build_that_printed_no_id_is_unverified_not_verified(self):
        verified, store = self.flash(b"no id here\n", b"BUILD_ID=anything\nTESTS_DONE\n")
        self.assertIsNone(verified)
        store.set_expected_build_id.assert_not_called()


class FlashDefaultPathTests(unittest.TestCase):
    def test_uses_the_default_path_and_records_the_manifest_when_out_is_omitted(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "engine"
            (worktree / "launcher" / "tools").mkdir(parents=True)
            (worktree / "launcher" / "tools" / "build_flash.sh").write_text("")
            root = Path(directory) / "records"
            connection = FakeConnection([b"BUILD_ID=expected\nTESTS_DONE\n"])
            args = Namespace(owner="agent", purpose="flash", wait=0, variant="dev",
                             worktree=str(worktree), out=None)
            store = mock.Mock()
            store.acquire.return_value = {"log": "", "token": "token"}
            fixed_now = datetime(2026, 9, 16, 12, 30, 45)
            with mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "now", return_value=fixed_now), \
                 mock.patch.object(device.subprocess, "run", side_effect=build_prints("expected")), \
                 mock.patch.object(device, "reset"), \
                 mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                device.flash(args, store, "COM5")
            expected_log = root / "20260916" / "123045_flash-dev_agent.log"
            self.assertTrue(expected_log.is_file())
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            self.assertEqual(entry["capture_path"], str(expected_log))
            self.assertEqual(entry["command"], "flash")
            self.assertEqual(entry["build_id"], "expected")
            self.assertIsNone(entry["error"])
            self.assertEqual(entry["worktree"], str(worktree.resolve()))
            self.assertEqual(entry["commit"], "deadbeef")

    def test_passes_the_lock_token_to_build_flash_sh(self):
        # build_flash.sh refuses to flash without AUTANA_DEVICE_LOCK_TOKEN -
        # flash() is the one place that has the token to give it.
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "engine"
            (worktree / "launcher" / "tools").mkdir(parents=True)
            (worktree / "launcher" / "tools" / "build_flash.sh").write_text("")
            root = Path(directory) / "records"
            connection = FakeConnection([b"BUILD_ID=expected\nTESTS_DONE\n"])
            args = Namespace(owner="agent", purpose="flash", wait=0, variant="dev",
                             worktree=str(worktree), out=None)
            store = mock.Mock()
            store.acquire.return_value = {"log": "", "token": "sekrit-token"}
            run = mock.Mock(side_effect=build_prints("expected"))
            with mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device.subprocess, "run", run), \
                 mock.patch.object(device, "reset"), \
                 mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                device.flash(args, store, "COM5")
            passed_env = run.call_args.kwargs["env"]
            self.assertEqual(passed_env["AUTANA_DEVICE_LOCK_TOKEN"], "sekrit-token")


class FlashCommandLineTests(unittest.TestCase):
    """main()'s own `flash` dispatch: --perf-scope becomes an extra_flags
    entry, the same way batch() and selftest() already build theirs."""

    def run_main(self, argv):
        calls = []

        def fake_flash(args, store, port, held_lock=None, extra_flags=()):
            calls.append(list(extra_flags))
            return 0

        with mock.patch.object(device, "find_port", return_value="COM5"), \
             mock.patch.object(device, "flash", fake_flash), \
             mock.patch.object(device, "device_lock") as fake_lock_module:
            fake_lock_module.LockStore.return_value = mock.Mock()
            device.main(argv)
        return calls

    def test_perf_scope_flag_becomes_an_extra_flag(self):
        calls = self.run_main(["--owner", "a", "flash", "--variant", "diag",
                               "--worktree", "C:/wt", "--perf-scope"])
        self.assertEqual(calls, [["--perf-scope"]])

    def test_no_perf_scope_flag_passes_nothing_extra(self):
        calls = self.run_main(["--owner", "a", "flash", "--variant", "dev", "--worktree", "C:/wt"])
        self.assertEqual(calls, [[]])


class ReportCommandTests(unittest.TestCase):
    """The `report` subcommand must work over a file already on disk without
    ever touching the board - no port lookup, no lock, no serial."""

    def test_never_looks_for_a_port_and_writes_the_report_file(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_text(":1:test_one:PASS\n:2:test_two:FAIL: boom\n")
            with mock.patch.object(device, "find_port",
                                   side_effect=AssertionError("must not look for a port")):
                exit_code = device.main(["report", str(capture), "--index",
                                         str(Path(directory) / "index.jsonl")])
            self.assertEqual(exit_code, 0)
            report_path = capture.with_name("capture.md")
            self.assertTrue(report_path.is_file())
            self.assertIn("PASS: 1  FAIL: 1", report_path.read_text(encoding="utf-8"))

    def test_a_missing_capture_fails_without_raising(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "nope.log"
            with mock.patch.object(device, "find_port",
                                   side_effect=AssertionError("must not look for a port")):
                exit_code = device.main(["report", str(missing)])
        self.assertEqual(exit_code, 1)


class RunSuiteReportGenerationTests(unittest.TestCase):
    """run_suite() must hand its finished capture to device_report so a
    result is readable without opening the raw log - see device_report.py."""

    def test_writes_a_report_beside_the_default_path_capture(self):
        connection = FakeConnection([
            b":1:test_one:PASS\n:2:test_two:FAIL: Expected 1 Was 0\nSUITE_DONE sand\n"
        ])
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand", out=None,
                         max_seconds=1, idle_seconds=None, expect_build_id=None)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            fixed_now = datetime(2026, 9, 16, 12, 30, 45)
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "now", return_value=fixed_now), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                self.assertEqual(device.run_suite(args, store, "COM5"), 1)
            report_path = root / "20260916" / "123045_runsuite-sand_agent.md"
            self.assertTrue(report_path.is_file())
            text = report_path.read_text(encoding="utf-8")
            self.assertIn("PASS: 1  FAIL: 1", text)
            self.assertIn("test_two", text)

    def test_a_broken_reporter_step_does_not_fail_the_suite_command(self):
        connection = FakeConnection([b":1:test_one:PASS\nSUITE_DONE sand\n"])
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand", out=None,
                         max_seconds=1, idle_seconds=None, expect_build_id=None)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch.object(device_report, "write_report_for_capture",
                                   side_effect=RuntimeError("disk full")):
                self.assertEqual(device.run_suite(args, store, "COM5"), 0)


class RunSuiteRecordsWorktreeTests(unittest.TestCase):
    """The manifest documents 'the worktree and commit involved' for every
    invocation (Device-Lock.md); run-suite used to leave worktree null."""

    def test_run_suite_records_the_ambient_cwd_as_worktree(self):
        connection = FakeConnection([b":1:test_one:PASS\nSUITE_DONE sand\n"])
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand", out=None,
                         max_seconds=1, idle_seconds=None, expect_build_id=None)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch.object(device.Path, "cwd", return_value=Path("C:/some/worktree")):
                device.run_suite(args, store, "COM5")
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        self.assertEqual(entry["worktree"], str(Path("C:/some/worktree")))


class BatchTests(unittest.TestCase):
    """A batch flashes once and captures every suite N times under ONE lock -
    the property that stops another agent flashing between two captures of
    the same image. No serial port, lock file or build is touched here."""

    def run_batch(self, suites=("run_sand_perf_suite",), runs=3, fail_run=None,
                  perf_scope=False, script_text="--diag --dev --perf-scope", out=False):
        calls = {"locks": 0, "flash": [], "run_suite": []}

        class FakeLock:
            def __init__(self, *unused):
                calls["locks"] += 1

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                return False

        def fake_flash(args, store, port, held_lock=None, extra_flags=()):
            calls["flash"].append((held_lock, list(extra_flags)))
            return "abc123-diag"

        def fake_run_suite(args, store, port, held_lock=None, worktree=None, commit=None):
            calls["run_suite"].append((args.suite, args.out, held_lock, args.expect_build_id,
                                       worktree, commit))
            Path(args.out).write_text(":1:test_one:PASS\n", encoding="utf-8")
            if fail_run is not None and len(calls["run_suite"]) == fail_run:
                raise RuntimeError("capture timed out")
            return 0

        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "wt"
            (worktree / "launcher" / "tools").mkdir(parents=True)
            (worktree / "launcher" / "tools" / "build_flash.sh").write_text(script_text)
            calls["worktree"] = str(worktree.resolve())
            out_path = str(Path(directory) / "raw.txt") if out else None
            args = Namespace(owner="agent", purpose="p", wait=0, worktree=str(worktree),
                             variant="diag", suite=list(suites), runs=runs, perf_scope=perf_scope,
                             max_seconds=1, idle_seconds=None, out=out_path)
            with mock.patch.object(device, "HeldLock", FakeLock), \
                 mock.patch.object(device, "flash", fake_flash), \
                 mock.patch.object(device, "run_suite", fake_run_suite), \
                 mock.patch.object(device, "records_root", return_value=Path(directory) / "rec"), \
                 mock.patch.object(device, "git_commit", return_value="c0ffee"), \
                 mock.patch("builtins.print"):
                code = device.batch(args, mock.Mock(), "COM5")
                summaries = list((Path(directory) / "rec").rglob("*_batch_*.md"))
                summary = summaries[0].read_text(encoding="utf-8") if summaries else ""
        return code, calls, summary

    def test_one_lock_one_flash_for_every_capture(self):
        code, calls, _ = self.run_batch(suites=("run_sand_perf_suite", "run_gfx_suite"), runs=3)
        self.assertEqual(code, 0)
        self.assertEqual(calls["locks"], 1)
        self.assertEqual(len(calls["flash"]), 1)
        self.assertEqual(len(calls["run_suite"]), 6)

    def test_every_capture_runs_inside_the_batch_lock_on_the_flashed_build(self):
        _, calls, _ = self.run_batch(runs=2)
        batch_lock = calls["flash"][0][0]
        self.assertIsNotNone(batch_lock)
        for suite, out, held_lock, expected, unused_worktree, unused_commit in calls["run_suite"]:
            self.assertIs(held_lock, batch_lock)
            self.assertEqual(expected, "abc123-diag")

    def test_run_suite_entries_carry_the_batch_worktree_and_its_commit(self):
        """Each run-suite call must be told the batch's own --worktree and
        that worktree's HEAD, not the ambient cwd - see device.py's
        run_suite() docstring and the manifest bug this replaced."""
        _, calls, _ = self.run_batch(runs=1)
        for suite, out, unused_held_lock, unused_expected, worktree, commit in calls["run_suite"]:
            self.assertEqual(worktree, calls["worktree"])
            self.assertEqual(commit, "c0ffee")

    def test_a_capture_error_is_recorded_and_the_batch_continues(self):
        code, calls, summary = self.run_batch(runs=3, fail_run=2)
        self.assertEqual(code, 1)
        self.assertEqual(len(calls["run_suite"]), 3)
        self.assertIn("- run 2: capture timed out", summary)

    def test_writes_one_summary_for_the_batch(self):
        _, _, summary = self.run_batch(runs=2)
        self.assertIn("# Device Batch Report", summary)
        self.assertIn("- Build Id: `abc123-diag`", summary)

    def test_perf_scope_is_passed_to_the_build(self):
        _, calls, _ = self.run_batch(perf_scope=True)
        self.assertEqual(calls["flash"][0][1], ["--perf-scope"])

    def test_out_is_used_for_one_suite_one_run(self):
        _, calls, _ = self.run_batch(suites=("run_sand_perf_suite",), runs=1, out=True)
        self.assertTrue(calls["run_suite"][0][1].endswith("raw.txt"))

    def test_out_with_more_than_one_run_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "--out only makes sense"):
            self.run_batch(suites=("run_sand_perf_suite",), runs=2, out=True)

    def test_out_with_more_than_one_suite_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "--out only makes sense"):
            self.run_batch(suites=("run_sand_perf_suite", "run_gfx_suite"), runs=1, out=True)


class ToolchainAddr2LineTests(unittest.TestCase):
    """toolchain_addr2line() looks under IDF_TOOLS_PATH when set - the same
    override ESP-IDF's own install script honours - and ~/.espressif
    otherwise."""

    def make_toolchain(self, root):
        bin_dir = root / "tools" / "xtensa-esp-elf" / "14.2.0" / "esp-14.2.0_20241119" / "bin"
        bin_dir.mkdir(parents=True)
        addr2line = bin_dir / "xtensa-esp32s3-elf-addr2line"
        addr2line.write_bytes(b"")
        return addr2line

    def test_honours_idf_tools_path_when_set(self):
        with tempfile.TemporaryDirectory() as directory:
            custom_root = Path(directory) / "custom-tools"
            addr2line = self.make_toolchain(custom_root)
            with mock.patch.dict(os.environ, {"IDF_TOOLS_PATH": str(custom_root)}):
                found = device.toolchain_addr2line()
        self.assertEqual(found, addr2line)

    def test_falls_back_to_home_espressif_when_unset(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            addr2line = self.make_toolchain(home / ".espressif")
            with mock.patch.dict(os.environ, {}, clear=False):
                os.environ.pop("IDF_TOOLS_PATH", None)
                with mock.patch.object(device.Path, "home", return_value=home):
                    found = device.toolchain_addr2line()
        self.assertEqual(found, addr2line)


class DecodeCrashAddressesTests(unittest.TestCase):
    """decode_crash_addresses() turns a Backtrace line into addr2line's
    output, the same symbolication idf_monitor gets for free when handed
    a .elf."""

    def test_no_crash_line_needs_no_addr2line(self):
        with mock.patch.object(device, "toolchain_addr2line",
                               side_effect=AssertionError("must not be called")):
            self.assertEqual(device.decode_crash_addresses(b"ordinary log output\n", Path("x.elf")), [])

    def test_a_missing_toolchain_yields_nothing(self):
        data = b"Backtrace:0x400d1234:0x3ffb1f80\n"
        with mock.patch.object(device, "toolchain_addr2line", return_value=None):
            self.assertEqual(device.decode_crash_addresses(data, Path("x.elf")), [])

    def test_addresses_on_a_backtrace_line_are_decoded_once_each(self):
        data = b"Backtrace:0x400d1234:0x3ffb1f80 0x400d1234:0x3ffb1fa0\n"
        result = mock.Mock(stdout="main.c:42\n")
        with mock.patch.object(device, "toolchain_addr2line", return_value=Path("addr2line")), \
             mock.patch.object(device.subprocess, "run", return_value=result) as run:
            decoded = device.decode_crash_addresses(data, Path("x.elf"))
        self.assertEqual(decoded, ["main.c:42"])
        command = run.call_args[0][0]
        self.assertEqual(command.count("0x400d1234"), 1)


class FindElfForBuildIdTests(unittest.TestCase):
    """find_elf_for_build_id() picks the build actually on the board - the
    one whose own build_id.txt matches - never merely the newest on disk."""

    def test_no_build_id_finds_nothing(self):
        self.assertIsNone(device.find_elf_for_build_id(Path("C:/wt"), ""))

    def test_matches_the_build_whose_build_id_txt_agrees(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory)
            newer = worktree / "launcher" / "build.dev"
            older = worktree / "launcher" / "build.diag"
            newer.mkdir(parents=True)
            older.mkdir(parents=True)
            (newer / "build_id.txt").write_text("newer-id\n", encoding="ascii")
            (newer / "launcher.elf").write_bytes(b"")
            (older / "build_id.txt").write_text("older-id\n", encoding="ascii")
            (older / "launcher.elf").write_bytes(b"")
            found = device.find_elf_for_build_id(worktree, "older-id")
        self.assertEqual(found, older / "launcher.elf")

    def test_no_matching_build_id_finds_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory)
            build = worktree / "launcher" / "build.dev"
            build.mkdir(parents=True)
            (build / "build_id.txt").write_text("some-id\n", encoding="ascii")
            (build / "launcher.elf").write_bytes(b"")
            found = device.find_elf_for_build_id(worktree, "different-id")
        self.assertIsNone(found)

    def test_a_matching_build_id_with_no_elf_file_is_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory)
            build = worktree / "launcher" / "build.dev"
            build.mkdir(parents=True)
            (build / "build_id.txt").write_text("some-id\n", encoding="ascii")
            found = device.find_elf_for_build_id(worktree, "some-id")
        self.assertIsNone(found)


class CaptureAfterResetTests(unittest.TestCase):
    """A watchdog reset can hand the first open the pre-reset handle, which
    reads nothing and raises nothing. Only a short window after the reset
    treats that silence as a stale handle; the caller's idle cutoff owns the
    rest, so a silent board never holds the shared lock to the deadline."""

    def capture(self, connections, seconds, idle_seconds):
        connections = iter(connections)
        opened = lambda *unused, **unused_keywords: next(connections)
        started = time.monotonic()
        with mock.patch.object(device, "open_when_free", side_effect=opened), \
             mock.patch.object(device, "RESET_FIRST_BYTE_SECONDS", 0.05), \
             mock.patch.object(device, "RESET_REOPEN_SECONDS", 0.3):
            data, reason = device.capture_after_reset("COM5", os.devnull, seconds, idle_seconds)
        return data, reason, time.monotonic() - started

    def test_a_handle_silent_since_the_reset_is_reopened_despite_a_long_idle_cutoff(self):
        data, reason, elapsed = self.capture(
            [FakeConnection([]), FakeConnection([b"SELFTEST_COMPLETE failures=0\n"])],
            seconds=30, idle_seconds=300)
        self.assertEqual(reason, "complete")
        self.assertIn(b"SELFTEST_COMPLETE", data)
        self.assertLess(elapsed, 5)

    def test_a_handle_silent_since_the_reset_is_reopened_with_no_idle_cutoff(self):
        data, reason, unused_elapsed = self.capture(
            [FakeConnection([]), FakeConnection([b"SELFTEST_COMPLETE failures=0\n"])],
            seconds=30, idle_seconds=None)
        self.assertEqual(reason, "complete")

    def test_a_board_silent_for_good_ends_at_its_idle_cutoff_not_the_deadline(self):
        forever_silent = (FakeConnection([]) for unused in iter(int, 1))
        data, reason, elapsed = self.capture(forever_silent, seconds=30, idle_seconds=0.5)
        self.assertEqual((data, reason), (b"", "silent"))
        self.assertLess(elapsed, 5)

    def test_output_that_goes_idle_after_bytes_is_not_reopened(self):
        opens = []

        def opened(*unused, **unused_keywords):
            opens.append(1)
            return FakeConnection([b"I (640) boot: some output\n"])

        started = time.monotonic()
        with mock.patch.object(device, "open_when_free", side_effect=opened), \
             mock.patch.object(device, "RESET_FIRST_BYTE_SECONDS", 0.05):
            unused_data, reason = device.capture_after_reset("COM5", os.devnull, 30, 0.2)
        self.assertEqual((reason, len(opens)), ("idle", 1))
        self.assertLess(time.monotonic() - started, 5)

    def test_silence_after_output_on_an_earlier_handle_is_not_reopened(self):
        class LostAfterOutput(FakeConnection):
            def read(self, size):
                if self.chunks:
                    return super().read(size)
                raise OSError("device re-enumerated")

        opens = []
        connections = iter([LostAfterOutput([b"I (640) boot: some output\n"])]
                           + [FakeConnection([]) for unused in range(20)])

        def opened(*unused, **unused_keywords):
            opens.append(1)
            return next(connections)

        with mock.patch.object(device, "open_when_free", side_effect=opened), \
             mock.patch.object(device, "RESET_FIRST_BYTE_SECONDS", 0.05), \
             mock.patch.object(device, "RESET_REOPEN_SECONDS", 5):
            unused_data, reason = device.capture_after_reset("COM5", os.devnull, 30, 0.5)
        self.assertEqual((reason, len(opens)), ("idle", 2))

    def boot_build_id(self, connections):
        connections = iter(connections)
        opened = lambda *unused, **unused_keywords: next(connections)
        with mock.patch.object(device, "open_serial", side_effect=opened), \
             mock.patch.object(device, "open_when_free", side_effect=opened), \
             mock.patch.object(device, "RESET_FIRST_BYTE_SECONDS", 0.05), \
             mock.patch.object(device, "RESET_REOPEN_SECONDS", 0.3):
            return device.boot_build_id("COM5", seconds=5)

    def test_boot_build_id_reads_the_id_through_a_stale_first_handle(self):
        actual, unused_reason = self.boot_build_id(
            [AnswersNoQuery([]), AnswersNoQuery([b"BUILD_ID=after-reset\n"])])
        self.assertEqual(actual, "after-reset")

    def test_boot_build_id_reads_the_id_through_a_lost_first_handle(self):
        class LostConnection(FakeConnection):
            def read(self, unused_size):
                raise OSError("device re-enumerated")

        actual, unused_reason = self.boot_build_id(
            [LostConnection([]), AnswersNoQuery([b"BUILD_ID=after-reset\n"])])
        self.assertEqual(actual, "after-reset")

    def test_a_board_silent_throughout_still_gets_the_query_in_bounded_time(self):
        silent = [AnswersNoQuery([]) for unused in range(50)]
        started = time.monotonic()
        actual, reason = self.boot_build_id(silent)
        self.assertIsNone(actual)
        self.assertEqual(reason, "silent")
        queried =[connection for connection in silent if connection.writes]
        self.assertEqual([connection.writes for connection in queried], [[b"BUILDID\n"]])
        self.assertLess(time.monotonic() - started, 5 + 3)



class ResetTests(unittest.TestCase):
    def after_argument(self, *args, **keywords):
        with mock.patch.object(device, "python_with_pyserial", return_value="python"), \
             mock.patch.object(device.subprocess, "run") as run:
            device.reset("COM5", *args, **keywords)
        command = run.call_args[0][0]
        return command[command.index("--after") + 1]

    def test_restarts_through_rts_by_default(self):
        self.assertEqual(self.after_argument(), "hard_reset")

    def test_restarts_through_the_watchdog_when_asked(self):
        self.assertEqual(self.after_argument(after="watchdog_reset"), "watchdog_reset")


class ListenElfResolutionTests(unittest.TestCase):
    """listen() decodes against --elf when given, and otherwise against
    whatever find_elf_for_build_id() resolves from the capture itself."""

    def run_listen(self, elf_arg, data_lines, matched_elf):
        connection = FakeConnection([b"\n".join(data_lines) + b"\n"])
        args = Namespace(owner="agent", purpose="autana monitor", wait=0,
                         seconds=1, out=None, elf=elf_arg)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch.object(device, "find_elf_for_build_id", return_value=matched_elf) as finder, \
                 mock.patch.object(device, "decode_crash_addresses", return_value=[]) as decode:
                device.listen(args, store, "COM5")
        return finder, decode

    def test_an_explicit_elf_skips_build_id_matching(self):
        finder, decode = self.run_listen("mine.elf", [b"ordinary log line"], None)
        finder.assert_not_called()
        decode.assert_called_once()
        self.assertEqual(decode.call_args[0][1], Path("mine.elf"))

    def test_no_elf_resolves_by_build_id(self):
        finder, decode = self.run_listen(
            None, [b"BUILD_ID=abc123-dev", b"Backtrace:0x400d1234:0x3ffb1f80"],
            Path("C:/wt/launcher/build.dev/launcher.elf"))
        finder.assert_called_once()
        self.assertEqual(finder.call_args[0][1], "abc123-dev")
        decode.assert_called_once_with(mock.ANY, Path("C:/wt/launcher/build.dev/launcher.elf"))

    def test_no_match_decodes_nothing(self):
        finder, decode = self.run_listen(None, [b"no build id here"], None)
        finder.assert_called_once()
        decode.assert_not_called()

    def test_quiet_listen_prints_record_path_and_errors(self):
        connection = FakeConnection([b"ordinary line\nerror: failed\n"])
        args = Namespace(owner="agent", purpose="autana monitor", wait=0,
                         seconds=0.1, follow=False, echo=False, out=None, elf=None)
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        printed = io.StringIO()
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch.object(device, "records_root", return_value=Path(directory)), \
             mock.patch.object(device, "git_commit", return_value="deadbeef"), \
             mock.patch.object(device, "find_elf_for_build_id", return_value=None), \
             contextlib.redirect_stdout(printed):
            device.listen(args, store, "COM5")
        self.assertIn("listen capture: ", printed.getvalue())
        self.assertIn("error: failed", printed.getvalue())
        self.assertNotIn("ordinary line", printed.getvalue())


class ListenLifecycleTests(unittest.TestCase):
    def run_listen(self, connection, flags, output=None):
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = output or io.StringIO()
            with mock.patch.object(device.device_lock, "LockStore", return_value=store), \
                 mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch.object(device, "find_elf_for_build_id", return_value=None), \
                 contextlib.redirect_stdout(output):
                code = device.main(["--port", "COM5", "listen", *flags])
            entries = [json.loads(line) for line in (root / "index.jsonl").read_text().splitlines()]
            payload = Path(entries[-1]["capture_path"]).read_bytes()
            return code, output.getvalue(), entries[-1], payload, store

    def test_read_interrupt_records_stopped_capture(self):
        for flags in (["--follow"], ["--seconds", "1"]):
            with self.subTest(flags=flags):
                code, output, entry, payload, store = self.run_listen(
                    InterruptedConnection([b"BUILD_ID=abc123\nerror: broken\n"]), flags)
                self.assertEqual(code, 0)
                self.assertEqual(entry["reason"], "stopped")
                self.assertEqual(entry["build_id"], "abc123")
                self.assertEqual(payload, b"BUILD_ID=abc123\nerror: broken\n")
                self.assertIn("listen capture: " + entry["capture_path"], output)
                self.assertIn("listen capture ended: stopped", output)
                store.release.assert_called_once()

    def test_listen_requires_exactly_one_duration_mode(self):
        for flags in ([], ["--seconds", "1", "--follow"]):
            with self.subTest(flags=flags), self.assertRaises(SystemExit) as caught:
                device.main(["--port", "COM5", "listen", *flags])
            self.assertEqual(caught.exception.code, 2)

    def test_quiet_errors_arrive_before_capture_ends(self):
        class Connection(FakeConnection):
            def read(self, size):
                if not self.chunks:
                    self.assert_output()
                    raise KeyboardInterrupt
                return super().read(size)

        connection = Connection([b"ordinary\nerror: broken\n"])
        connection.assert_output = lambda: self.assertIn("error: broken", current_stdout.getvalue())
        current_stdout = io.StringIO()
        code, output, entry, _, _ = self.run_listen(connection, ["--follow"], current_stdout)
        self.assertEqual(code, 0)
        self.assertEqual(output.count("error: broken"), 1)
        self.assertNotIn("ordinary", output)

    def test_echo_interrupt_records_flushed_bytes(self):
        class Output(io.StringIO):
            def __init__(self):
                super().__init__()
                self.buffer = mock.Mock()
                self.buffer.write.side_effect = KeyboardInterrupt

        output = Output()
        code, text, entry, payload, store = self.run_listen(
            FakeConnection([b"BUILD_ID=abc123\nerror: broken\n"]),
            ["--follow", "--echo"], output)
        self.assertEqual(code, 0)
        self.assertEqual(entry["reason"], "stopped")
        self.assertEqual(entry["build_id"], "abc123")
        self.assertEqual(payload, b"BUILD_ID=abc123\nerror: broken\n")
        self.assertIn(entry["capture_path"], text)
        store.release.assert_called_once()

    def test_echo_writes_raw_bytes_without_reprinting_errors(self):
        class Output(io.StringIO):
            def __init__(self):
                super().__init__()
                self.buffer = io.BytesIO()

        output = Output()
        raw = b"ordinary\nerror: broken\n"
        code, text, _, _, _ = self.run_listen(
            InterruptedConnection([raw]), ["--follow", "--echo"], output)
        self.assertEqual(code, 0)
        self.assertEqual(output.buffer.getvalue(), raw)
        self.assertNotIn("error: broken", text)

    def test_timed_listen_continues_past_test_completion(self):
        connection = InterruptedConnection([b"TESTS_DONE\n", b"after tests\n"])
        code, _, entry, payload, _ = self.run_listen(connection, ["--seconds", "1"])
        self.assertEqual(code, 0)
        self.assertEqual(entry["reason"], "stopped")
        self.assertEqual(payload, b"TESTS_DONE\nafter tests\n")

    def test_partial_echo_gets_newline_before_path(self):
        class Output(io.StringIO):
            def __init__(self):
                super().__init__()
                self.buffer = io.BytesIO()

        output = Output()
        code, text, entry, _, _ = self.run_listen(
            InterruptedConnection([b"partial"]), ["--follow", "--echo"], output)
        self.assertEqual(code, 0)
        self.assertTrue(text.startswith("\nlisten capture: "))
        self.assertEqual(output.buffer.getvalue(), b"partial")

    def test_compressed_path_is_printed(self):
        code, text, entry, _, _ = self.run_listen(
            InterruptedConnection([b"x" * (device.COMPRESS_ABOVE_BYTES + 1)]),
            ["--follow"])
        self.assertEqual(code, 0)
        self.assertTrue(entry["capture_path"].endswith(".gz"))
        self.assertIn("listen capture: " + entry["capture_path"], text)

    def test_interrupt_during_lock_wait_records_stopped(self):
        store = mock.Mock()
        store.acquire.side_effect = KeyboardInterrupt
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device.device_lock, "LockStore", return_value=store), \
             mock.patch.object(device, "records_root", return_value=Path(directory)), \
             mock.patch.object(device, "git_commit", return_value="deadbeef"), \
             mock.patch.object(device, "find_elf_for_build_id", return_value=None), \
             contextlib.redirect_stdout(io.StringIO()):
            code = device.main(["--port", "COM5", "listen", "--follow"])
            entry = json.loads((Path(directory) / "index.jsonl").read_text().strip())
        self.assertEqual(code, 0)
        self.assertEqual(entry["reason"], "stopped")

    def test_interrupt_during_port_open_releases_lock(self):
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device.device_lock, "LockStore", return_value=store), \
             mock.patch.object(device, "open_when_free", side_effect=KeyboardInterrupt), \
             mock.patch.object(device, "records_root", return_value=Path(directory)), \
             mock.patch.object(device, "git_commit", return_value="deadbeef"), \
             mock.patch.object(device, "find_elf_for_build_id", return_value=None), \
             contextlib.redirect_stdout(io.StringIO()):
            code = device.main(["--port", "COM5", "listen", "--follow"])
            entry = json.loads((Path(directory) / "index.jsonl").read_text().strip())
        self.assertEqual(code, 0)
        self.assertEqual(entry["reason"], "stopped")
        store.release.assert_called_once()


class WaiterNoticeTests(unittest.TestCase):
    def test_holder_reports_each_waiter_once(self):
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        store.tickets.return_value = [{"ticket": "one", "owner": "sam", "purpose": "test"}]
        lock = device.HeldLock(store, "COM5", "agent", "monitor", 0, announce_waiters=True)
        lock.stop.wait = mock.Mock(side_effect=[False, False, True])
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            lock.keep_alive()
        self.assertEqual(stderr.getvalue().count("sam is waiting"), 1)

    def test_a_flash_or_suite_holder_never_invites_ctrl_c(self):
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        store.tickets.return_value = [{"ticket": "one", "owner": "sam", "purpose": "test"}]
        lock = device.HeldLock(store, "COM5", "agent", "flash", 0)
        lock.stop.wait = mock.Mock(side_effect=[False, False, True])
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            lock.keep_alive()
        self.assertEqual(stderr.getvalue(), "")


class ResetCommandTests(unittest.TestCase):
    def test_parser_forwards_capture_options(self):
        store = mock.Mock()
        with mock.patch.object(device.device_lock, "LockStore", return_value=store), \
             mock.patch.object(device, "reset_device", return_value=0) as reset_device:
            code = device.main(["--port", "COM5", "reset", "--capture", "--seconds", "15"])
        self.assertEqual(code, 0)
        args = reset_device.call_args[0][0]
        self.assertTrue(args.capture)
        self.assertEqual(args.seconds, 15.0)

    def test_capture_resets_waits_reopens_after_a_lost_handle_and_records(self):
        class VanishingConnection(FakeConnection):
            def read(self, unused_size):
                raise OSError("USB device disappeared")

        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=1.0, out=None)
        first = VanishingConnection([])
        second = FakeConnection([b"cpu_start: Multicore app\nSELFTEST_COMPLETE\n"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "reset") as reset, \
                 mock.patch.object(device, "open_when_free",
                                   side_effect=[FakeConnection([]), first, second]) as open_when_free, \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                code = device.reset_device(args, store, "COM5")
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        self.assertEqual(code, 0)
        reset.assert_called_once_with("COM5")
        store.release.assert_called_once_with("COM5", "token")
        self.assertEqual(open_when_free.call_count, 3)
        open_when_free.assert_called_with("COM5", mock.ANY, reason="re-enumerating after reset")
        self.assertEqual(entry["command"], "reset")
        self.assertEqual(entry["reason"], "complete")

    def test_capture_keeps_data_when_a_later_reopen_runs_out_of_time(self):
        class VanishingConnection(FakeConnection):
            def read(self, unused_size):
                if self.chunks:
                    return super().read(unused_size)
                raise OSError("USB device disappeared")

        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=20.0, out=None)
        connection = VanishingConnection([b"boot output\n"])
        timeout = RuntimeError("board did not come back after reset before capture ended")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "reset"), \
                 mock.patch.object(device, "open_when_free",
                                   side_effect=[FakeConnection([]), connection, timeout]), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch("builtins.print") as printed:
                code = device.reset_device(args, store, "COM5")
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            capture = Path(entry["capture_path"]).read_bytes()
        self.assertEqual(code, 0)
        self.assertEqual(capture, b"boot output\n")
        self.assertEqual(entry["reason"], "port lost")
        self.assertIsNone(entry["error"])
        self.assertNotIn(mock.call("boot output\n", end=""), printed.call_args_list)
        printed.assert_any_call("reset capture: " + entry["capture_path"])

    def test_reset_releases_the_lock_when_esptool_fails(self):
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=1.0, out=None)
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "reset",
                               side_effect=subprocess.CalledProcessError(1, "esptool")), \
             mock.patch.object(device, "open_when_free", return_value=FakeConnection([])), \
             mock.patch.object(device, "records_root", return_value=Path(directory)):
            with self.assertRaises(subprocess.CalledProcessError):
                device.reset_device(args, store, "COM5")
        store.release.assert_called_once_with("COM5", "token")

    def test_reset_records_an_os_error(self):
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=1.0, out=None)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "reset", side_effect=OSError("port vanished")), \
                 mock.patch.object(device, "open_when_free", return_value=FakeConnection([])), \
                 mock.patch.object(device, "records_root", return_value=root):
                with self.assertRaisesRegex(OSError, "port vanished"):
                    device.reset_device(args, store, "COM5")
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        self.assertEqual(entry["error"], "port vanished")
        self.assertIsNone(entry["reason"])

    def test_reset_without_capture_waits_before_and_after_reset(self):
        calls = []
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=False)

        def waited(*unused, **unused_keywords):
            calls.append("wait")
            return FakeConnection([])

        with mock.patch.object(device, "open_when_free", side_effect=waited), \
             mock.patch.object(device, "reset", side_effect=lambda unused: calls.append("reset")):
            self.assertEqual(device.reset_device(args, store, "COM5"), 0)
        self.assertEqual(calls, ["wait", "reset", "wait"])


class SelftestTests(unittest.TestCase):
    """selftest() flashes diag+autorun under one held lock, then resets,
    waits for USB serial, and captures until SELFTEST_COMPLETE."""

    def run_selftest(self, perf_scope=False):
        calls = {"flash_extra_flags": None, "held_lock": None, "events": []}

        class FakeLock:
            def __enter__(self):
                return self

            def __exit__(self, *unused):
                return False

        def fake_flash(args, store, port, held_lock=None, extra_flags=()):
            calls["flash_extra_flags"] = list(extra_flags)
            calls["held_lock"] = held_lock
            return "abc123-diag"

        connection = FakeConnection([b"free heap after framebuffer: 123456 bytes\n",
                                     b"SELFTEST_COMPLETE failures=0 elapsed_ms=42\n"])

        def fake_reset(unused_port):
            calls["events"].append("reset")

        def fake_wait(*unused, **unused_keywords):
            if calls["events"]:
                calls["events"].append("reopen")
            return connection

        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "wt"
            (worktree / "launcher" / "tools").mkdir(parents=True)
            root = Path(directory) / "records"
            args = Namespace(owner="agent", purpose="autana selftest", wait=0,
                             worktree=str(worktree), out=None, perf_scope=perf_scope,
                             max_seconds=5, idle_seconds=None)
            store = mock.Mock()
            store.acquire.return_value = {"log": "", "token": "token"}
            with mock.patch.object(device, "flash", fake_flash), \
                 mock.patch.object(device, "reset", fake_reset), \
                 mock.patch.object(device, "open_when_free", side_effect=fake_wait), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                code = device.selftest(args, store, "COM5")
                entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        return code, calls, entry

    def test_flashes_diag_with_autorun_under_the_batch_lock(self):
        code, calls, entry = self.run_selftest()
        self.assertEqual(code, 0)
        self.assertEqual(calls["flash_extra_flags"], ["--autorun"])
        self.assertIsNotNone(calls["held_lock"])

    def test_perf_scope_is_passed_to_the_build(self):
        _, calls, _ = self.run_selftest(perf_scope=True)
        self.assertEqual(sorted(calls["flash_extra_flags"]), ["--autorun", "--perf-scope"])

    def test_resets_before_reopening_for_capture(self):
        _, calls, _ = self.run_selftest()
        self.assertEqual(calls["events"], ["reset", "reopen"])

    def test_records_the_selftest_command_and_build_id(self):
        _, _, entry = self.run_selftest()
        self.assertEqual(entry["command"], "selftest")
        self.assertEqual(entry["build_id"], "abc123-diag")
        self.assertIsNone(entry["error"])

    def test_a_failing_run_is_reported_but_still_records_cleanly(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "wt"
            (worktree / "launcher" / "tools").mkdir(parents=True)
            root = Path(directory) / "records"
            connection = FakeConnection([
                b":1:test_one:FAIL: boom\nSELFTEST_COMPLETE failures=1 elapsed_ms=10\n",
            ])
            args = Namespace(owner="agent", purpose="autana selftest", wait=0,
                             worktree=str(worktree), out=None, perf_scope=False,
                             max_seconds=5, idle_seconds=None)
            store = mock.Mock()
            store.acquire.return_value = {"log": "", "token": "token"}
            with mock.patch.object(device, "flash", return_value="abc123-diag"), \
                 mock.patch.object(device, "reset"), \
                 mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                code = device.selftest(args, store, "COM5")
        self.assertEqual(code, 1)


class ScreenshotCommandTests(unittest.TestCase):
    """device.screenshot() under a faked serial port, driving the real
    launcher/tools/screenshot.py decode - see that module's own tests
    (launcher/tools/tests/test_screenshot.py) for the decode in isolation."""

    def minimal_bmp(self):
        pixel_offset = 14 + 40
        pixel = bytes((7, 8, 9)) + b"\x00"  # one BGR pixel, padded to 4 bytes
        total = pixel_offset + len(pixel)
        header = b"BM" + struct.pack("<IHHI", total, 0, 0, pixel_offset)
        info = struct.pack("<IiiHHIIiiII", 40, 1, 1, 1, 24, 0, len(pixel), 0, 0, 0, 0)
        return header + info + pixel

    def marked_bmp(self):
        pixel_offset = 14 + 40
        width, height = 2, 3
        rows = [bytes((1, 2, 3, 4, 5, 6, 0, 0)), bytes((7, 8, 9, 10, 11, 12, 0, 0)),
                bytes((13, 14, 15, 16, 17, 18, 0, 0))]
        pixel_data = b"".join(rows)
        total = pixel_offset + len(pixel_data)
        header = b"BM" + struct.pack("<IHHI", total, 0, 0, pixel_offset)
        info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                           len(pixel_data), 0, 0, 0, 0)
        return header + info + pixel_data

    def image_shape(self, path):
        data = Path(path).read_bytes()
        return struct.unpack_from(">II", data, 16)

    def first_pixel(self, path):
        data = Path(path).read_bytes()
        pos = 8
        compressed = bytearray()
        while pos < len(data):
            length, = struct.unpack_from(">I", data, pos)
            if data[pos + 4:pos + 8] == b"IDAT":
                compressed += data[pos + 8:pos + 8 + length]
            pos += length + 12
        return zlib.decompress(compressed)[1:4]

    def wire_lines(self, bmp, state_json=None):
        encoded = base64.b64encode(bmp).decode("ascii")
        lines = [f"SCREENSHOT_BEGIN size={len(bmp)}", f"SCREENSHOT_DATA:{encoded}"]
        if state_json is not None:
            lines.append(f"SCREENSHOT_STATE:{state_json}")
        lines.append("SCREENSHOT_END")
        return ("\n".join(lines) + "\n").encode("ascii")

    def test_takes_the_lock_and_writes_the_decoded_files(self):
        connection = FakeConnection([self.wire_lines(self.minimal_bmp(), '{"heap": 1}')])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / "shot.bmp")
            args = Namespace(owner="agent", purpose="autana screenshot", wait=0, out=out, timeout=1.0)
            with mock.patch.object(device, "open_when_free", return_value=connection):
                code = device.screenshot(args, store, "COM5")
            self.assertEqual(code, 0)
            self.assertTrue((Path(directory) / "shot.png").is_file())
            self.assertEqual(json.loads((Path(directory) / "shot.json").read_text()),
                             {"heap": 1, "image_turn_quarter": 3})
        store.acquire.assert_called_once()

    def test_defaults_its_out_path_to_a_timestamped_name_in_the_cwd(self):
        connection = FakeConnection([self.wire_lines(self.minimal_bmp())])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        fixed_now = datetime(2026, 9, 16, 12, 30, 45)
        with tempfile.TemporaryDirectory() as directory:
            args = Namespace(owner="agent", purpose="autana screenshot", wait=0, out=None, timeout=1.0)
            with mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "now", return_value=fixed_now), \
                 mock.patch.object(device.Path, "cwd", return_value=Path(directory)):
                device.screenshot(args, store, "COM5")
            self.assertTrue((Path(directory) / "screenshot_20260916_123045.png").is_file())

    def test_default_turns_the_framebuffer_to_the_board_shape_and_records_it(self):
        connection = FakeConnection([self.wire_lines(self.marked_bmp(), '{"orientation_quarter": 2}')])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / "shot.png")
            args = Namespace(owner="agent", purpose="p", wait=0, out=out, timeout=1.0)
            with mock.patch.object(device, "open_when_free", return_value=connection):
                device.screenshot(args, store, "COM5")
            self.assertEqual(self.image_shape(out), (3, 2))
            self.assertEqual(self.first_pixel(out), bytes((18, 17, 16)))
            self.assertEqual(json.loads(Path(directory, "shot.json").read_text())["image_turn_quarter"], 3)

    def test_framebuffer_mode_keeps_the_bytes_orientation_and_records_zero_turn(self):
        connection = FakeConnection([self.wire_lines(self.marked_bmp(), '{"orientation_quarter": 1}')])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / "shot.png")
            args = Namespace(owner="agent", purpose="p", wait=0, out=out, timeout=1.0,
                             framebuffer=True)
            with mock.patch.object(device, "open_when_free", return_value=connection):
                device.screenshot(args, store, "COM5")
            self.assertEqual(self.image_shape(out), (2, 3))
            self.assertEqual(self.first_pixel(out), bytes((15, 14, 13)))
            self.assertEqual(json.loads(Path(directory, "shot.json").read_text())["image_turn_quarter"], 0)

    def test_as_shown_uses_the_captured_orientation_quarter(self):
        connection = FakeConnection([self.wire_lines(self.marked_bmp(), '{"orientation_quarter": 2}')])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / "shot.png")
            args = Namespace(owner="agent", purpose="p", wait=0, out=out, timeout=1.0,
                             as_shown=True)
            with mock.patch.object(device, "open_when_free", return_value=connection):
                device.screenshot(args, store, "COM5")
            self.assertEqual(self.image_shape(out), (2, 3))
            self.assertEqual(self.first_pixel(out), bytes((6, 5, 4)))
            self.assertEqual(json.loads(Path(directory, "shot.json").read_text())["image_turn_quarter"], 2)

    def test_screenshot_views_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            device.main(["screenshot", "--as-shown", "--framebuffer"])

    def test_a_refusal_propagates_as_a_runtime_error(self):
        connection = FakeConnection([b"SCREENSHOT_REFUSED: no room in PSRAM\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="p", wait=0, out=None, timeout=1.0)
        with mock.patch.object(device, "open_when_free", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "no room in PSRAM"):
                device.screenshot(args, store, "COM5")


if __name__ == "__main__":
    unittest.main()
