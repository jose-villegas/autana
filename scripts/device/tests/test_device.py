import base64
import gzip
import json
import os
import struct
import tempfile
import unittest
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
        device.wait_for_port("COM5", 120, self.opener(0), self.sleep, lambda: self.clock[0])
        self.assertEqual(self.slept, [])

    def test_a_straggler_is_waited_out(self):
        device.wait_for_port("COM5", 120, self.opener(3), self.sleep, lambda: self.clock[0])
        self.assertEqual(self.slept, [1.0, 1.0, 1.0])

    def test_a_port_nobody_frees_reports_at_the_deadline(self):
        with self.assertRaises(RuntimeError) as caught:
            device.wait_for_port("COM5", 5, self.opener(99), self.sleep, lambda: self.clock[0])
        self.assertIn("COM5", str(caught.exception))
        self.assertIn("won the lock", str(caught.exception))


class FakeConnection:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []

    def read(self, unused_size):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.writes.append(data)

    def close(self):
        """wait_for_port() probes the port by opening and closing it."""

    def flush(self):
        pass

    def reset_input_buffer(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, unused_type, unused_value, unused_traceback):
        pass


class DeviceTests(unittest.TestCase):
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
             mock.patch.object(device, "open_serial", return_value=connection), \
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
             mock.patch.object(device, "open_serial", return_value=connection), \
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

    def test_replies_to_an_empty_reply_captures_every_line_and_never_completes(self):
        """autana console's own forwarding case: an app's reply prefix
        (COUNTS, ...) is not known here, so every line counts and nothing
        ends the answer early - the caller reads out its own window."""
        data = b"I (1) shell: frame 16 ms\nCOUNTS Sand=12\nCOUNTS_END\n"
        found, complete = device.replies_to(data, "", [])
        self.assertFalse(complete)
        self.assertEqual(found, ["I (1) shell: frame 16 ms", "COUNTS Sand=12", "COUNTS_END"])

    def test_replies_to_an_empty_reply_skips_blank_lines(self):
        found, unused_complete = device.replies_to(b"\nCOUNTS Sand=12\n\n", "", [])
        self.assertEqual(found, ["COUNTS Sand=12"])

    def send_args(self, line):
        return Namespace(owner="agent", purpose="send", wait=0, line=line, reply="TUNE",
                         until=["TUNE_OK", "TUNE_ERR", "TUNE_END"], seconds=1, optional=False)

    def test_send_writes_the_line_and_prints_the_reply(self):
        connection = FakeConnection([b"I (5) shell: x\nTUNE_OK launcher.ridge_trail=200\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"), \
             mock.patch("builtins.print") as printed:
            status = device.send(self.send_args("SET launcher.ridge_trail 200"), store, "COM5")
        self.assertEqual(status, 0)
        self.assertEqual(connection.writes, [b"\nSET launcher.ridge_trail 200\n"])
        printed.assert_called_once_with("TUNE_OK launcher.ridge_trail=200")

    def test_send_fails_on_an_error_reply(self):
        connection = FakeConnection([b"TUNE_ERR range launcher.ridge_trail takes 0..255\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"), mock.patch("builtins.print"):
            self.assertEqual(device.send(self.send_args("SET launcher.ridge_trail 999"), store, "COM5"), 1)

    def test_send_says_so_when_the_build_has_no_such_command(self):
        connection = FakeConnection([b"I (9) screenshot: ignoring line: 'TUNE'\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"):
            with self.assertRaisesRegex(RuntimeError, "needs a development build"):
                device.send(self.send_args("TUNE"), store, "COM5")

    def test_send_forwards_a_console_line_and_prints_everything_seen(self):
        """autana console's own case (autana.py's console()): the reply
        prefix an app chooses is not known here, so this reads out the
        whole window rather than matching one prefix."""
        connection = FakeConnection([b"COUNTS Sand=12\n", b"COUNTS_END\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="sand counts",
                         reply="", until=[], seconds=0.05, optional=True)
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"), mock.patch("builtins.print") as printed:
            status = device.send(args, store, "COM5")
        self.assertEqual(status, 0)
        printed.assert_called_once_with("COUNTS Sand=12\nCOUNTS_END")

    def test_send_optional_treats_silence_as_success(self):
        """TOUCH/IMU answer only when something is wrong - a timeout with
        nothing seen is that verb's normal happy path, not a failure."""
        connection = FakeConnection([b"I (1) shell: unrelated log line\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="TOUCH down 1 2",
                         reply="TOUCH", until=["TOUCH"], seconds=0.05, optional=True)
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"), mock.patch("builtins.print") as printed:
            status = device.send(args, store, "COM5")
        self.assertEqual(status, 0)
        printed.assert_called_once_with("")

    def test_send_optional_still_prints_a_device_side_warning(self):
        connection = FakeConnection([b"W (2) console: TOUCH wants <down|up> <x> <y>: 'bad'\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="TOUCH bad",
                         reply="TOUCH", until=["TOUCH"], seconds=0.5, optional=True)
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"), mock.patch("builtins.print") as printed:
            status = device.send(args, store, "COM5")
        self.assertEqual(status, 0)
        printed.assert_called_once_with("TOUCH wants <down|up> <x> <y>: 'bad'")

    def test_send_without_optional_still_raises_on_silence(self):
        connection = FakeConnection([b"I (1) shell: unrelated log line\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="send", wait=0, line="TUNE", reply="TUNE",
                         until=["TUNE_OK", "TUNE_ERR", "TUNE_END"], seconds=0.05, optional=False)
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"):
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
                 mock.patch.object(device.subprocess, "run"), \
                 mock.patch.object(device, "reset"), \
                 mock.patch.object(device, "read_expected_build_id", return_value="expected"), \
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
                  perf_scope=False, script_text="--diag --dev --perf-scope"):
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
            calls["run_suite"].append((args.suite, held_lock, args.expect_build_id,
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
            args = Namespace(owner="agent", purpose="p", wait=0, worktree=str(worktree),
                             variant="diag", suite=list(suites), runs=runs, perf_scope=perf_scope,
                             max_seconds=1, idle_seconds=None)
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
        for suite, held_lock, expected, unused_worktree, unused_commit in calls["run_suite"]:
            self.assertIs(held_lock, batch_lock)
            self.assertEqual(expected, "abc123-diag")

    def test_run_suite_entries_carry_the_batch_worktree_and_its_commit(self):
        """Each run-suite call must be told the batch's own --worktree and
        that worktree's HEAD, not the ambient cwd - see device.py's
        run_suite() docstring and the manifest bug this replaced."""
        _, calls, _ = self.run_batch(runs=1)
        for suite, unused_held_lock, unused_expected, worktree, commit in calls["run_suite"]:
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

    def test_perf_scope_is_refused_when_the_build_script_cannot_do_it(self):
        with self.assertRaisesRegex(RuntimeError, "no --perf-scope option"):
            self.run_batch(perf_scope=True, script_text="--diag --dev")


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
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "wait_for_port"):
                code = device.screenshot(args, store, "COM5")
            self.assertEqual(code, 0)
            self.assertTrue((Path(directory) / "shot.png").is_file())
            self.assertEqual(json.loads((Path(directory) / "shot.json").read_text()), {"heap": 1})
        store.acquire.assert_called_once()

    def test_defaults_its_out_path_to_a_timestamped_name_in_the_cwd(self):
        connection = FakeConnection([self.wire_lines(self.minimal_bmp())])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        fixed_now = datetime(2026, 9, 16, 12, 30, 45)
        with tempfile.TemporaryDirectory() as directory:
            args = Namespace(owner="agent", purpose="autana screenshot", wait=0, out=None, timeout=1.0)
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "wait_for_port"), \
                 mock.patch.object(device, "now", return_value=fixed_now), \
                 mock.patch.object(device.Path, "cwd", return_value=Path(directory)):
                device.screenshot(args, store, "COM5")
            self.assertTrue((Path(directory) / "screenshot_20260916_123045.png").is_file())

    def test_a_refusal_propagates_as_a_runtime_error(self):
        connection = FakeConnection([b"SCREENSHOT_REFUSED: no room in PSRAM\n"])
        store = mock.Mock()
        store.acquire.return_value = {"log": "", "token": "token"}
        args = Namespace(owner="agent", purpose="p", wait=0, out=None, timeout=1.0)
        with mock.patch.object(device, "open_serial", return_value=connection), \
             mock.patch.object(device, "wait_for_port"):
            with self.assertRaisesRegex(RuntimeError, "no room in PSRAM"):
                device.screenshot(args, store, "COM5")


if __name__ == "__main__":
    unittest.main()
