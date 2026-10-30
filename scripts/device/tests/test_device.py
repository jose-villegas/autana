"""device.py: interpreter choice, serial open and port guards, waits, records and captures."""
import isolation  # noqa: F401  (first: keeps the suite out of real records)
import port_guard
import ast
import base64
import io
import contextlib
import errno
import gzip
import importlib.util
import itertools
import json
import os
import re
import struct
import subprocess
import tempfile
import time
import types
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
from fake_serial import FakeConnection, FakeUsbBoard
import fake_flash  # noqa: E402
import device_lock
import device_hook
import device_report
import autana_config

# A board is named by its USB serial number; COM5 is where it enumerates.
BOARD = "90:70:69:FE:A3:08"
EXAMPLE_PREFIX = "EXAMPLE"


def mock_store(token="token"):
    store = mock.Mock()
    store.root = Path(os.environ["_AUTANA_DEVICE_LOCK_ROOT"])
    store.key_root.side_effect = lambda board: device_lock.heavy_root() if board == device_lock.HEAVY else store.root
    store.acquire.return_value = device_lock.Held({"token": token, "acquired_at": 1000.0})
    store.tickets.return_value = []
    return store


def suite_args(owner="agent"):
    return Namespace(owner=owner, purpose="test", wait=0, suite="sand", out=None,
                     max_seconds=1, idle_seconds=None, expect_build_id=None)


@contextlib.contextmanager
def recorded_serial(connection, root):
    with mock.patch.object(device, "open_serial", return_value=connection), \
         mock.patch.object(device, "records_root", return_value=root), \
         mock.patch.object(device, "git_commit", return_value="deadbeef"):
        yield


class InterpreterTests(unittest.TestCase):
    """Any interpreter may start device.py: a report script's `python`, a
    person at a prompt, and only ESP-IDF's carries pyserial."""

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

    def test_a_tool_reruns_its_own_script_rather_than_device_py(self):
        with mock.patch.dict(sys.modules, self.MISSING),                 mock.patch.object(device, "idf_python", return_value="C:/idf/python.exe"),                 mock.patch.object(device.subprocess, "call", return_value=0) as call:
            device.rerun_under_idf_python(["--plan"], "tools/perf_compare.py")
        self.assertEqual(call.call_args.args[0][1], str(Path("tools/perf_compare.py").resolve()))
        self.assertEqual(call.call_args.args[0][2:], ["--plan"])

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


class SerialTimeoutException(OSError):
    """pyserial's, an IOError like every port error it raises."""


class StalledSerial(FakeConnection):
    """pyserial's Serial on a port the board stopped draining: a write times
    out, and what was queued never leaves."""

    out_waiting = 1

    def open(self):
        pass

    def write(self, unused_data):
        raise SerialTimeoutException("Write timeout")


class OpenSerialTests(unittest.TestCase):
    """A board that stops reading its console (a half-written image, a
    wedged app) must fail the command, not hold the lock forever: pyserial
    with no write timeout blocks in the OS write with no limit."""

    @contextlib.contextmanager
    def stalled(self):
        serial = types.ModuleType("serial")
        serial.Serial = StalledSerial
        serial.SerialTimeoutException = SerialTimeoutException
        with mock.patch.dict(sys.modules, {"serial": serial}), \
             mock.patch.object(device, "locked_port", return_value="COM5"):
            yield

    def test_a_write_waits_no_longer_than_a_boot(self):
        with self.stalled():
            connection = device.open_serial()
        self.assertEqual(connection.write_timeout, device.BOOT_WAIT_SECONDS)

    def test_modem_policy_wraps_the_open(self):
        events = []
        with self.stalled(), \
                mock.patch.object(device.board_serial, "before_open", side_effect=lambda port: events.append(("before", port))), \
                mock.patch.object(StalledSerial, "open", side_effect=lambda: events.append(("open", None))), \
                mock.patch.object(device.board_serial, "after_open", side_effect=lambda port: events.append(("after", port))):
            connection = device.open_serial()
        self.assertEqual(events, [("before", connection), ("open", None), ("after", connection)])

    def test_a_write_the_board_never_takes_is_an_error(self):
        with self.stalled():
            connection = device.open_serial()
        with self.assertRaisesRegex(device.BoardNotReading,
                                    f"stopped reading its console: a write did not drain "
                                    f"within {device.BOOT_WAIT_SECONDS}s"):
            connection.write(b"RUNSUITE sand\n")

    def flushed(self, queued):
        """(seconds waited, the error or None) for a flush on a port whose
        output queue reads `queued` in turn, the last value repeating, on a
        clock only its sleeps move."""
        with self.stalled():
            connection = device.open_serial()
        self.enterContext(mock.patch.object(type(connection), "out_waiting", property(
            lambda unused: queued.pop(0) if len(queued) > 1 else queued[0])))
        clock = [0.0]
        fake_time = types.SimpleNamespace(
            monotonic=lambda: clock[0],
            sleep=lambda seconds: clock.__setitem__(0, clock[0] + seconds))
        with mock.patch.object(device, "time", fake_time):
            try:
                connection.flush()
            except device.BoardNotReading as error:
                return clock[0], error
        return clock[0], None

    def test_a_flush_returns_once_the_queue_drains(self):
        self.assertEqual(self.flushed([0]), (0, None))
        self.assertEqual(self.flushed([2, 1, 0]), (2 * device.FLUSH_POLL_SECONDS, None))

    def test_a_flush_that_never_drains_fails_at_the_write_timeout(self):
        waited, error = self.flushed([1])
        self.assertIsInstance(error, device.BoardNotReading)
        self.assertGreaterEqual(waited, device.BOOT_WAIT_SECONDS)
        self.assertLess(waited, device.BOOT_WAIT_SECONDS + 2 * device.FLUSH_POLL_SECONDS)

    def test_a_write_that_times_out_releases_the_lock(self):
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory, self.stalled(), \
             mock.patch.object(device, "open_when_free",
                               side_effect=lambda *unused, **unused_kw: device.open_serial()), \
             mock.patch.object(device, "records_root", return_value=Path(directory)), \
             mock.patch.object(device, "git_commit", return_value="deadbeef"), \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(device.BoardNotReading):
                device.run_suite(suite_args(), store, BOARD)
        store.release.assert_called_once_with(BOARD, "token")

    @unittest.skipUnless(hasattr(os, "openpty") and importlib.util.find_spec("serial"),
                         "needs a pseudo-terminal and pyserial")
    def test_a_real_port_nobody_reads_fails_the_write(self):
        """The OS, not a stand-in, refuses the write: a pseudo-terminal whose
        other end is never read fills, and pyserial's write timeout fires."""
        with port_guard.fake_port() as (unused, name), \
             mock.patch.object(device, "locked_port", return_value=name):
            with device.open_serial() as connection:
                connection.write_timeout = 0.5
                started = time.monotonic()
                with self.assertRaises(device.BoardNotReading):
                    connection.write(bytes(1 << 20))
                self.assertLess(time.monotonic() - started, device.BOOT_WAIT_SECONDS)


@unittest.skipUnless(importlib.util.find_spec("serial"), "needs pyserial")
class PortGuardTests(unittest.TestCase):
    """No test reaches a real board: port_guard fails the run of one that
    opens a real port or lists the real ones, even where the code under test
    swallowed the refusal."""

    # A name no host has, so a guard that let it through still opens nothing.
    PORT = "autana-test-no-such-port"

    def run_swallowing(self, statement, guard="port_guard"):
        child = ("import sys; sys.path.insert(0, sys.argv[1]); import " + guard + "\n"
                 "try:\n    " + statement + "\nexcept BaseException:\n    pass\n")
        return subprocess.run([sys.executable, "-c", child, str(DEVICE / "tests")],
                              capture_output=True, text=True)

    def test_opening_a_real_port_fails_the_run(self):
        for guard in ("port_guard", "isolation"):
            with self.subTest(guard=guard):
                result = self.run_swallowing(f"import serial; serial.Serial({self.PORT!r})", guard)
                self.assertEqual(result.returncode, 1)
                self.assertIn("opened the real serial port " + self.PORT, result.stderr)

    def test_listing_the_real_ports_fails_the_run(self):
        result = self.run_swallowing(
            "from serial.tools import list_ports; list_ports.comports()")
        self.assertEqual(result.returncode, 1)
        self.assertIn("listed the real USB serial ports", result.stderr)

    def test_a_refused_open_is_not_waited_out_as_a_busy_port(self):
        import serial
        sleeps = []
        with self.assertRaises(port_guard.RealPortTouched):
            device.open_when_free(device.PORT_WAIT_SECONDS, opener=lambda: serial.Serial(self.PORT),
                                  sleep=sleeps.append, now=lambda: sum(sleeps))
        port_guard.violations.remove("opened the real serial port " + self.PORT)
        self.assertEqual(sleeps, [])


class PortGuardCoverageTests(unittest.TestCase):
    def test_every_test_module_that_imports_device_imports_the_guard_first(self):
        """Each test directory runs in its own process, so the guard is only
        there if the module that brings device in (directly, or through
        autana) brought it in first."""
        engine = DEVICE.parents[1]
        listed = subprocess.run(["git", "ls-files", "*.py"], cwd=engine, capture_output=True,
                                text=True, check=True).stdout.split()
        unguarded = []
        for name in (name for name in listed if "/tests/" in name):
            tree = ast.parse((engine / name).read_text(encoding="utf-8"))
            imports = [(node.lineno, alias.name) for node in ast.walk(tree)
                       if isinstance(node, ast.Import) for alias in node.names]
            imports += [(node.lineno, node.module) for node in ast.walk(tree)
                        if isinstance(node, ast.ImportFrom)]
            device_at = min((line for line, module in imports if module in ("device", "autana")),
                            default=None)
            guard_at = min((line for line, module in imports
                            if module in ("port_guard", "isolation")), default=None)
            if device_at is not None and (guard_at is None or guard_at > device_at):
                unguarded.append(name)
        self.assertEqual(unguarded, [])


class HookIsolationTests(unittest.TestCase):
    def test_suite_child_reservation_does_not_run_inherited_hook(self):
        if os.environ.get("_AUTANA_HOOK_SUITE_CHILD"):
            with tempfile.TemporaryDirectory() as root:
                device_lock.LockStore(root=root).reserve_human(BOARD, "agent", "test")
            return
        with tempfile.TemporaryDirectory() as root:
            sentinel = Path(root) / "sentinel"
            command = (f'"{sys.executable}" -c "import os; '
                       "open(os.environ['_AUTANA_HOOK_SENTINEL'], 'w').close()\"")
            environment = os.environ.copy()
            environment.update({"AUTANA_LOCK_HOOK": command,
                                "_AUTANA_HOOK_SENTINEL": str(sentinel),
                                "_AUTANA_HOOK_SUITE_CHILD": "1"})
            result = subprocess.run([sys.executable, "-m", "unittest", "discover",
                                     "-s", str(DEVICE / "tests"), "-p", "test_device.py",
                                     "-k", "HookIsolationTests.test_suite_child_reservation"],
                                    env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr[-1000:])
            self.assertFalse(sentinel.exists())


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

        def open_port():
            if remaining[0] > 0:
                remaining[0] -= 1
                raise OSError(13, "Access is denied")
            return mock.Mock()

        return open_port

    def test_a_port_that_is_free_is_not_waited_for(self):
        device.open_when_free(120, self.opener(0), self.sleep, lambda: self.clock[0])
        self.assertEqual(self.slept, [])

    def test_returns_the_connection_without_closing_it(self):
        connection = mock.Mock()
        result = device.open_when_free(120, lambda: connection,
                                       self.sleep, lambda: self.clock[0])
        self.assertIs(result, connection)
        connection.close.assert_not_called()

    def test_a_straggler_is_waited_out_with_a_bounded_backoff(self):
        device.open_when_free(120, self.opener(6), self.sleep, lambda: self.clock[0])
        first, most = device.OPEN_FIRST_PAUSE_SECONDS, device.OPEN_MAX_PAUSE_SECONDS
        self.assertEqual(self.slept, [first, 2 * first, 4 * first, 8 * first, most, most])

    def test_last_backoff_pause_is_clipped(self):
        bound = device.OPEN_FIRST_PAUSE_SECONDS / 2
        with self.assertRaises(device.PortUnavailable):
            device.open_when_free(bound, self.opener(99), self.sleep, lambda: self.clock[0])
        self.assertEqual(self.slept, [bound])

    def test_clock_crossing_deadline_cannot_make_sleep_negative(self):
        bound = device.OPEN_FIRST_PAUSE_SECONDS
        readings = iter([0, bound / 2, 2 * bound, 2 * bound])
        with self.assertRaises(device.PortUnavailable):
            device.open_when_free(bound, self.opener(99), self.sleep, lambda: next(readings))
        self.assertTrue(all(pause >= 0 for pause in self.slept))

    def test_a_port_nobody_frees_reports_at_the_deadline(self):
        with self.assertRaises(device.PortUnavailable) as caught:
            device.open_when_free(5, self.opener(99), self.sleep, lambda: self.clock[0])
        self.assertIn("wait ran out", str(caught.exception))

    def test_the_timeout_names_the_previous_holder_and_what_of_it_still_runs(self):
        with tempfile.TemporaryDirectory() as root:
            store = device_lock.LockStore(root)
            held = store.acquire(BOARD, "sam@bench:41", "flash")
            store.release(BOARD, held["token"])
            active = mock.Mock(store=store, board=BOARD)
            device.ACTIVE_LOCK.held = active
            self.addCleanup(setattr, device.ACTIVE_LOCK, "held", None)
            scope = mock.Mock(process_start=mock.Mock(return_value=None),
                              process_name=mock.Mock(return_value="esptool"),
                              survivors_extra=mock.Mock(return_value=[4242]))
            with mock.patch.object(device, "require_live_lock"), \
                    mock.patch.object(device, "lock_scope", scope):
                with self.assertRaises(device.PortUnavailable) as caught:
                    device.open_when_free(5, self.opener(99), self.sleep, lambda: self.clock[0])
        message = str(caught.exception)
        self.assertIn("sam@bench:41 (autana flash)", message)
        self.assertIn("4242 (esptool)", message)
        self.assertEqual(scope.survivors_extra.call_args.args[0]["token"], held["token"])

    def test_without_a_lock_the_timeout_names_no_holder(self):
        with self.assertRaises(device.PortUnavailable) as caught:
            device.open_when_free(5, self.opener(99), self.sleep, lambda: self.clock[0])
        self.assertNotIn("previous holder", str(caught.exception))

    def test_reset_reenumeration_reason_reaches_the_timeout(self):
        with self.assertRaisesRegex(RuntimeError, "re-enumerating after reset"):
            device.open_when_free(0, self.opener(1), self.sleep,
                                  lambda: self.clock[0], "re-enumerating after reset")


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
    def test_suite_verdict_requires_its_completion_line(self):
        for command, marker in (("suite", b"RUNSUITE_COMPLETE name=sand found=1\n"),
                                ("selftest", b"SELFTEST_COMPLETE failures=0 elapsed_ms=42\n")):
            for data, reason, incomplete in ((b"", "idle", True),
                                             (b":1:good:PASS\n", "timeout", True),
                                             (b":1:good:PASS\n" + marker, "complete", False)):
                with self.subTest(command=command, reason=reason), mock.patch("builtins.print") as printed:
                    if incomplete:
                        with self.assertRaisesRegex(RuntimeError, f"no completion line arrived: the capture ended {reason}"):
                            device.print_suite_output(data, "record.log", command, reason, False, 0)
                    else:
                        self.assertEqual(device.print_suite_output(data, "record.log", command, reason, False, 0), 0)

    def test_suite_output_shows_failure_messages_and_caps_the_list(self):
        data = (b"boot detail\n" + b":1:good:PASS\n" +
                b"".join(f"file.c:{line}:bad_{line}:FAIL: wrong {line}\n".encode()
                         for line in range(1, 13)))
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "suite", "complete", False, 0)
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
            device.print_suite_output(data, "record.log", "suite", "complete", False, 0)
        lines = [call.args[0] for call in printed.call_args_list]
        by_suite = lines[lines.index("2 more in the capture; by suite:") + 1:][:2]
        self.assertEqual(by_suite, ["  suite_sand_scenes: 9 FAIL", "  suite_gfx: 3 FAIL"])

    def test_under_the_cap_there_is_no_per_suite_block(self):
        data = b"/p/suite_gfx.c:1:b:FAIL: y\n"
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "suite", "complete", False, 0)
        self.assertFalse(any("by suite" in call.args[0] for call in printed.call_args_list))

    def test_a_run_that_crashed_fails_with_every_test_passed(self):
        data = b":1:good:PASS\nabort() was called at PC 0x40376f2b on core 0\nRUNSUITE_COMPLETE name=sand found=1\n"
        with mock.patch("builtins.print") as printed:
            failed = device.print_suite_output(data, "record.log", "suite", "complete", False, 0)
        self.assertTrue(failed)
        printed.assert_any_call("crash: abort() was called at PC 0x40376f2b on core 0")

    def test_a_selftest_may_hold_its_own_boot_and_no_second(self):
        boot = b"ESP-ROM:esp32s3-20210327\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\n"
        with mock.patch("builtins.print"):
            self.assertFalse(device.print_suite_output(
                boot + b":1:good:PASS\nSELFTEST_COMPLETE failures=0\n",
                "record.log", "selftest", "complete", False, 1))
            self.assertTrue(device.print_suite_output(
                boot + b":1:good:PASS\nSELFTEST_COMPLETE failures=0\n" + boot, "record.log", "selftest", "complete", False, 1))

    def test_a_passing_run_still_names_its_capture(self):
        data = b"boot detail\n:1:good:PASS\n"
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "suite", "complete", False, 0)
        lines = [call.args[0] for call in printed.call_args_list]
        self.assertIn("suite capture: record.log", lines)
        self.assertNotIn("boot detail", "\n".join(lines))

    def test_suite_output_pass_and_verbose_capture(self):
        data = b"boot detail\n:1:good:PASS\n"
        with mock.patch("builtins.print") as printed:
            device.print_suite_output(data, "record.log", "selftest", "complete", True, 1)
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

    def test_selftest_summary_does_not_complete_a_capture(self):
        summary = b":1:good:PASS\n1 Tests 0 Failures 0 Ignored\n"
        marker = b"SELFTEST_COMPLETE failures=0 elapsed_ms=42\n"
        for ending in ("idle", "timeout", "complete"):
            with self.subTest(ending=ending), tempfile.TemporaryDirectory() as directory:
                chunks = [summary]
                if ending == "complete":
                    chunks += [b"E (1) selftest: final diagnostic\n", marker]
                data, reason = device.capture(FakeConnection(chunks),
                                              Path(directory) / "capture.log", 0.02,
                                              0 if ending == "idle" else None)
                self.assertEqual(reason, ending)
                self.assertEqual(marker in data, ending == "complete")

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
            b":1:test_one:PASS\nRUNSUITE_COMPLETE name=sand found=1\n",
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
        chunks = [b"SELFTEST_COMPLETE failures=0 elapsed_ms=42\n", b"ordinary after tests\n"]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.log"
            with self.assertRaises(KeyboardInterrupt):
                device.capture(InterruptedConnection(chunks), output,
                               None, None, complete=None)
            self.assertEqual(output.read_bytes(), b"".join(chunks))

    def test_capture_rejects_run_suite_when_build_has_no_suites(self):
        connection = FakeConnection([b"shell: ignoring line: 'RUNSUITE sand'\n"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "no test suites.*autana flash diag"):
                device.capture(connection, Path(directory) / "capture.log", 1, None,
                               suite_name="sand")

    def test_capture_rejects_unknown_suite(self):
        connection = FakeConnection([b"no suite named 'sand'\n"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "no suite named sand"):
                device.capture(connection, Path(directory) / "capture.log", 1, None,
                               suite_name="sand")

    def test_run_suite_prints_result_counts(self):
        connection = FakeConnection([b":1:test_one:PASS\nRUNSUITE_COMPLETE name=sand found=1\n"])
        args = suite_args()
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch.object(device, "records_root", return_value=Path(directory)), \
             mock.patch("builtins.print") as output:
            args.out = str(Path(directory) / "capture.log")
            self.assertEqual(device.run_suite(args, store, BOARD), 0)
        output.assert_any_call("suite results: 1 PASS, 0 FAIL")

    def test_run_suite_returns_failure_status(self):
        connection = FakeConnection([
            b":623:test_a_timed_out_job_falls_back_inline:FAIL: Expected 1 Was 0\n"
            b"RUNSUITE_COMPLETE name=sand found=1\n"
        ])
        args = suite_args()
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch.object(device, "records_root", return_value=Path(directory)):
            args.out = str(Path(directory) / "capture.log")
            self.assertEqual(device.run_suite(args, store, BOARD), 1)

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
        self.assertEqual(found, ["TUNE a=1 min=0 max=2"])

    def send_response(self, chunks, args):
        connection = FakeConnection(chunks)
        store = mock_store()
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print") as printed:
            status = device.send(args, store, BOARD)
        return status, connection, printed

    def send_args(self, line, reply="TUNE", until=None, seconds=1, optional=False):
        return Namespace(owner="agent", purpose="send", wait=0, line=line, reply=reply,
                         until=["TUNE_OK", "TUNE_ERR", "TUNE_END"] if until is None else until,
                         seconds=seconds, optional=optional)

    def test_send_writes_the_line_and_prints_the_reply(self):
        status, connection, printed = self.send_response([b"I (5) shell: x\nTUNE_OK launcher.ridge_trail=200\n"],
                                                          self.send_args("SET launcher.ridge_trail 200"))
        self.assertEqual(status, 0)
        self.assertEqual(connection.writes, [b"\nSET launcher.ridge_trail 200\n"])
        printed.assert_called_once_with("TUNE_OK launcher.ridge_trail=200")

    def test_send_fails_on_an_error_reply(self):
        connection = FakeConnection([b"TUNE_ERR range launcher.ridge_trail takes 0..255\n"])
        store = mock_store()
        with mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch("builtins.print"):
            self.assertEqual(device.send(self.send_args("SET launcher.ridge_trail 999"), store, BOARD), 1)

    def test_send_says_so_when_the_build_has_no_such_command(self):
        connection = FakeConnection([b"I (9) screenshot: ignoring line: 'TUNE'\n"])
        store = mock_store()
        with mock.patch.object(device, "open_when_free", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "needs a development image"):
                device.send(self.send_args("TUNE"), store, BOARD)

    def test_send_forwards_an_app_command_and_completes_on_its_own_end_line(self):
        """autana console's own case (autana.py's console()): an app's
        reply always starts with its own prefix in capitals, so send()
        completes as soon as that prefix's own _END arrives rather than
        waiting out the window."""
        status, connection, printed = self.send_response([b"EXAMPLE status=ok\n", b"EXAMPLE_END\n"],
                                                          self.send_args("example status", reply="EXAMPLE", until=["EXAMPLE_END", "EXAMPLE_ERR"], optional=True))
        self.assertEqual(status, 0)
        printed.assert_called_once_with("EXAMPLE status=ok\nEXAMPLE_END")

    def test_exchange_long_reply_uses_idle_time_and_parses_each_line_once(self):
        payload = f"{EXAMPLE_PREFIX} sample ".encode() + b"x" * 100 + b"\n"
        transcript = f"{EXAMPLE_PREFIX}_BEGIN\n".encode() + payload * 12000 + f"{EXAMPLE_PREFIX}_END\n".encode()
        chunks = [transcript[i:i + 4096] for i in range(0, len(transcript), 4096)]
        connection = FakeConnection(chunks)
        clock = [0.0]
        original_read = connection.read

        def read(size):
            clock[0] += 0.1
            return original_read(size)

        with mock.patch.object(connection, "read", side_effect=read), \
             mock.patch.object(device.time, "monotonic", side_effect=lambda: clock[0]), \
             mock.patch.object(device, "replies_to", wraps=device.replies_to) as parsed:
            found, complete, _ = device.exchange(connection, EXAMPLE_PREFIX.lower(), EXAMPLE_PREFIX,
                                               [f"{EXAMPLE_PREFIX}_END", f"{EXAMPLE_PREFIX}_ERR"], 3.0)
        self.assertTrue(complete)
        self.assertEqual(found, transcript.decode().splitlines())
        self.assertGreater(clock[0], 3.0)
        self.assertEqual(sum(len(call.args[0]) for call in parsed.call_args_list), len(transcript))

    def test_exchange_caps_active_replies_and_does_not_extend_for_unrelated_logs(self):
        for payload, expected_seconds in ((f"{EXAMPLE_PREFIX} sample\n".encode(), 5.0), (b"I (1) shell: frame\n", 3.0)):
            with self.subTest(payload=payload):
                connection = FakeConnection([payload] * 20)
                clock = [0.0]
                original_read = connection.read

                def read(size):
                    clock[0] += 1.0
                    return original_read(size)

                with mock.patch.object(connection, "read", side_effect=read), \
                     mock.patch.object(device.time, "monotonic", side_effect=lambda: clock[0]), \
                     mock.patch.object(device, "EXCHANGE_MAX_SECONDS", 5.0):
                    _, complete, _ = device.exchange(connection, EXAMPLE_PREFIX.lower(), EXAMPLE_PREFIX,
                                                   [f"{EXAMPLE_PREFIX}_END", f"{EXAMPLE_PREFIX}_ERR"], 3.0)
                self.assertFalse(complete)
                self.assertEqual(clock[0], expected_seconds)

    def test_exchange_partial_reply_returns_cutoff(self):
        with mock.patch.object(device, "open_when_free", return_value=FakeConnection()):
            found, complete, cut_off = device.exchange(FakeConnection([b"EXAMPLE partial"]),
                "example", "EXAMPLE", ["EXAMPLE_END", "EXAMPLE_ERR"], 0.001)
        self.assertEqual([], found)
        self.assertFalse(complete)
        self.assertTrue(cut_off)

    def test_send_partial_reply_contracts(self):
        for optional in (False, True):
            for framed in (False, True):
                with self.subTest(optional=optional, framed=framed):
                    args = self.send_args("example", reply="EXAMPLE", seconds=0.001, optional=optional,
                        until=["EXAMPLE_END", "EXAMPLE_ERR"] if framed else ["EXAMPLE"])
                    if optional and not framed:
                        self.assertEqual(0, self.send_response([b"EXAMPLE partial"], args)[0])
                    else:
                        with self.assertRaisesRegex(RuntimeError, "reply cut off"):
                            self.send_response([b"EXAMPLE partial"], args)
        args = self.send_args("example", reply="EXAMPLE", until=["EXAMPLE_END"], seconds=0.001, optional=True)
        self.assertEqual(0, self.send_response([b"unrelated partial"], args)[0])

    def test_exchange_renews_only_for_complete_reply_lines(self):
        for payload in (b"EXAMPLE partial", b"I (1) shell: mentions EXAMPLE inside a log\n",
                        b"NOTEXAMPLE value\n"):
            with self.subTest(payload=payload):
                clock = [0.0]
                connection = FakeConnection([payload] * 20)
                original_read = connection.read

                def read(size):
                    clock[0] += 1.0
                    return original_read(size)

                with mock.patch.object(connection, "read", side_effect=read), \
                     mock.patch.object(device.time, "monotonic", side_effect=lambda: clock[0]):
                    device.exchange(connection, "example", "EXAMPLE", ["EXAMPLE_END"], 3.0)
                self.assertEqual(3.0, clock[0])

    def test_send_optional_rejects_a_reply_without_its_end(self):
        with self.assertRaisesRegex(RuntimeError, "reply cut off"):
            self.send_response([b"EXAMPLE_BEGIN\n", b"EXAMPLE partial"],
                               self.send_args("example", reply="EXAMPLE",
                                              until=["EXAMPLE_END", "EXAMPLE_ERR"],
                                              seconds=0.01, optional=True))

    def test_send_forwards_an_app_command_and_fails_on_its_own_err_line(self):
        status, connection, printed = self.send_response([b"EXAMPLE_ERR not running\n"],
                                                          self.send_args("example status", reply="EXAMPLE", until=["EXAMPLE_END", "EXAMPLE_ERR"], optional=True))
        self.assertEqual(status, 1)

    def test_send_optional_treats_silence_as_success(self):
        """TOUCH/IMU answer only when something is wrong; a timeout with
        nothing seen is that verb's normal happy path, not a failure."""
        status, connection, printed = self.send_response([b"I (1) shell: unrelated log line\n"],
                                                          self.send_args("TOUCH down 1 2", reply="TOUCH", until=["TOUCH"], seconds=0.05, optional=True))
        self.assertEqual(status, 0)
        printed.assert_called_once_with("")

    def test_send_optional_still_prints_a_device_side_warning(self):
        status, connection, printed = self.send_response([b"W (2) console: TOUCH wants <down|up> <x> <y>: 'bad'\n"],
                                                          self.send_args("TOUCH bad", reply="TOUCH", until=["TOUCH"], seconds=0.5, optional=True))
        self.assertEqual(status, 0)
        printed.assert_called_once_with("TOUCH wants <down|up> <x> <y>: 'bad'")

    def test_send_without_optional_still_raises_on_silence(self):
        connection = FakeConnection([b"I (1) shell: unrelated log line\n"])
        store = mock_store()
        args = self.send_args("TUNE", seconds=0.05)
        with mock.patch.object(device, "open_when_free", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "no reply"):
                device.send(args, store, BOARD)

    def test_status_reports_human_note_and_age(self):
        with tempfile.TemporaryDirectory() as root:
            store = device_lock.LockStore(root, now=lambda: 1000)
            store.reserve_human(BOARD, "maintainer", "panel")
            entry = device_lock.status_entry(store, BOARD, now=1065)
        self.assertEqual((entry["state"], entry["holder"], entry["since"]),
                         ("human", {"owner": "maintainer", "purpose": "panel"}, 1000))
        line = device_lock.status_lines(entry, 1065)[0]
        self.assertTrue(line.startswith("human reservation: maintainer: panel (since "))
        self.assertIn("65s ago", line)

    def test_take_back_clears_human_reservation_and_prints_status(self):
        with tempfile.TemporaryDirectory() as root:
            store = device_lock.LockStore(root)
            store.reserve_human(BOARD, "maintainer", "panel")
            with mock.patch.object(device.device_lock, "LockStore", return_value=store), \
                    mock.patch.object(device, "plugged_boards", return_value=[]), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(device.main(["--board", BOARD, "take-back"]), 0)
            self.assertIsNone(store.status(BOARD)["human"])
        self.assertEqual(output.getvalue(), f"{device.short_id(BOARD)} {BOARD} not on USB\n  unlocked\n")

    def test_hand_records_reservation(self):
        store = mock.Mock()
        store.status.return_value = {"human": None, "lock": None, "queue": []}
        store.reserve_human.return_value = ("id", False)
        with mock.patch.object(device.device_lock, "LockStore", return_value=store):
            self.assertEqual(device.main(["--board", BOARD, "--owner", "agent",
                                          "hand-to-human", "--note", "check cable"]), 0)
        store.reserve_human.assert_called_once_with(BOARD, "agent", "check cable", wait=600, on_wait=mock.ANY)

    def test_an_unnamed_owner_is_user_at_host_colon_pid_not_unknown(self):
        store = mock.Mock()
        store.status.return_value = {"human": None, "lock": None, "queue": []}
        store.reserve_human.return_value = ("id", False)
        with mock.patch.object(device.device_lock, "LockStore", return_value=store), \
                mock.patch.object(device_lock.getpass, "getuser", return_value="sam"), \
                mock.patch.object(device_lock, "host_identity", return_value="devbox"):
            self.assertEqual(device.main(["--board", BOARD, "hand-to-human",
                                          "--note", "check cable"]), 0)
        store.reserve_human.assert_called_once_with(
            BOARD, f"sam@devbox:{os.getpid()}", "check cable", wait=600, on_wait=mock.ANY)


class TakeOwnerTests(unittest.TestCase):
    def test_take_status_names_bound_child_and_timed_hold_keeps_owner(self):
        with tempfile.TemporaryDirectory() as root, subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(60)"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL) as child:
            try:
                with mock.patch.dict(os.environ, {"_AUTANA_DEVICE_LOCK_ROOT": root}), \
                        mock.patch.object(device, "plugged_boards", return_value=[]):
                    store = device_lock.LockStore(root)
                    owner = device_lock.default_owner()
                    for flags, expected in ((["--until-released", "--pid", str(child.pid)],
                                             owner.rsplit(":", 1)[0] + f":{child.pid}"),
                                            ([], owner)):
                        with self.subTest(flags=flags), contextlib.redirect_stdout(io.StringIO()) as output:
                            self.assertEqual(device.main(["--board", BOARD, "--owner", owner,
                                                          "take", *flags, "bench"]), 0)
                        token = output.getvalue().strip()
                        try:
                            self.assertEqual(store.status(BOARD)["lock"]["owner"], expected)
                            with contextlib.redirect_stdout(io.StringIO()) as status:
                                self.assertEqual(device.main(["--board", BOARD, "status"]), 0)
                            self.assertIn(expected, status.getvalue())
                        finally:
                            store.release(BOARD, token)
            finally:
                child.kill()
                child.wait()


class RemovedParameterTests(unittest.TestCase):
    """A command's lock label is its kind, and hand-to-human takes only a note."""

    def refused(self, *argv):
        with contextlib.redirect_stderr(io.StringIO()) as errors, \
                self.assertRaises(SystemExit) as stop:
            device.main(list(argv))
        self.assertEqual(stop.exception.code, 2)
        return errors.getvalue()

    def test_no_command_takes_a_purpose(self):
        for command in (["send", "TUNE"], ["listen", "--seconds", "1"], ["reset"],
                        ["screenshot"], ["hand-to-human", "--note", "x"],
                        ["flash", "--variant", "dev", "--worktree", "."]):
            with self.subTest(command=command):
                self.assertIn("--purpose", self.refused(command[0], "--purpose", "why", *command[1:]))

    def test_hand_to_human_takes_no_token(self):
        self.assertIn("--token", self.refused("hand-to-human", "--note", "x", "--token", "t"))

    def test_check_token_is_a_device_command(self):
        store = mock.Mock()
        store.check_token.return_value = True
        with mock.patch.object(device.device_lock, "LockStore", return_value=store):
            self.assertEqual(device.main(["--board", BOARD, "check-token", "--token", "t"]), 0)
            store.check_token.return_value = False
            self.assertEqual(device.main(["--board", BOARD, "check-token", "--token", "t"]), 1)
        store.check_token.assert_called_with(BOARD, "t")

    def test_the_lock_module_has_no_command_line_of_its_own(self):
        self.assertFalse(hasattr(device_lock, "main"))


class LockLabelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.store = device_lock.LockStore(root=self.temp.name)

    def holder_label(self, environment):
        with mock.patch.dict(os.environ, environment):
            if not environment:
                os.environ.pop(device.COMMAND_ENV, None)
            with device.HeldLock(self.store, BOARD, "agent", "send", 0):
                entry = device_lock.status_entry(self.store, BOARD, durations={})
        return entry["holder"]["purpose"]

    def test_the_command_autana_ran_is_what_status_shows(self):
        for named in ("tune", "debug freeze"):
            with self.subTest(command=named):
                self.assertEqual(self.holder_label({device.COMMAND_ENV: named}), "autana " + named)

    def test_a_direct_call_shows_device_py_and_its_kind(self):
        self.assertEqual(self.holder_label({}), "device.py send")

    def test_a_capture_manifest_uses_the_same_label(self):
        with mock.patch.dict(os.environ, {device.COMMAND_ENV: "tune"}):
            self.assertEqual(device.command_label("send"), "autana tune")


class HumanWaitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.store = device_lock.LockStore(root=self.temp.name)
        self.clock = [0.0]
        self.on_sleep = None

    def sleep(self, seconds):
        self.clock[0] += seconds
        if self.on_sleep:
            self.on_sleep()

    def hand(self, *flags):
        with mock.patch.object(device.device_lock, "LockStore", return_value=self.store), \
             mock.patch.object(device.time, "monotonic", side_effect=lambda: self.clock[0]), \
             mock.patch.object(device.time, "sleep", side_effect=self.sleep), \
             mock.patch("builtins.print") as output:
            code = device.main(["--board", BOARD, "--owner", "agent", "hand-to-human",
                                "--note", "download mode", *flags])
        return code, output

    def test_release_before_deadline_succeeds(self):
        self.on_sleep = lambda: self.store.end_human(BOARD)
        with mock.patch.object(device_hook, "emit") as emit:
            code, output = self.hand("--until-back", "3")
        self.assertEqual(code, 0)
        self.assertEqual(output.call_args_list[-1].args[0], "human reservation released")
        self.assertEqual(emit.call_args_list, [
            mock.call("human-reserved", BOARD, "agent", note="download mode"),
            mock.call("human-cleared", BOARD, "agent", note="download mode"),
        ])

    def test_timeout_keeps_reservation(self):
        code, output = self.hand("--until-back", "2")
        self.assertEqual(code, device_lock.EXIT_BUSY)
        self.assertEqual(output.call_args_list[-1].args[0], "human reservation wait timed out; the reservation stands")
        self.assertEqual(self.store.status(BOARD)["human"]["note"], "download mode")

    def test_interrupt_keeps_reservation(self):
        def interrupt():
            raise KeyboardInterrupt

        self.on_sleep = interrupt
        code, output = self.hand("--until-back", "3")
        self.assertEqual(code, device.EXIT_INTERRUPTED)
        self.assertEqual(output.call_args_list[-1].args[0], "human reservation wait interrupted")
        self.assertIsNotNone(self.store.status(BOARD)["human"])

    def test_replaced_reservation_is_reported(self):
        def replace():
            self.store.end_human(BOARD)
            self.store.reserve_human(BOARD, "colleague", "download mode")

        self.on_sleep = replace
        code, output = self.hand("--until-back", "3")
        self.assertEqual(code, device_lock.EXIT_BUSY)
        self.assertEqual(output.call_args_list[-1].args[0], "human reservation replaced")
        self.assertEqual(self.store.status(BOARD)["human"]["note"], "download mode")

    def test_a_wait_past_the_reservation_lifetime_ends_as_expired(self):
        self.store.now = lambda: 1000 + self.clock[0]
        code, output = self.hand("--until-back", "7200")
        self.assertEqual(code, 0)
        self.assertEqual(output.call_args_list[-1].args[0], "human reservation expired")
        self.assertGreaterEqual(self.clock[0], device_lock.HUMAN_RESERVATION_SECONDS)

    def test_a_renewal_does_not_end_the_wait(self):
        def renew():
            self.store.reserve_human(BOARD, "agent", "download mode")

        self.on_sleep = renew
        code, output = self.hand("--until-back", "3")
        self.assertEqual(code, device_lock.EXIT_BUSY)
        self.assertEqual(output.call_args_list[-1].args[0], "human reservation wait timed out; the reservation stands")


class SlugTests(unittest.TestCase):
    def test_replaces_unsafe_characters_with_a_single_dash(self):
        self.assertEqual(device.slug("agent a/b:c"), "agent-a-b-c")

    def test_collapses_runs_and_strips_leading_and_trailing_dashes(self):
        self.assertEqual(device.slug("  --weird!!name--  "), "weird-name")

    def test_falls_back_to_unknown_for_input_with_no_safe_characters(self):
        self.assertEqual(device.slug("   "), "unknown")


class RecordsRootTests(unittest.TestCase):
    DEFAULT = Path(device.__file__).resolve().parents[2] / ".records" / "device"

    def test_defaults_into_the_checkout_regardless_of_cwd(self):
        with isolation.project():
            self.assertEqual(device.records_root(), self.DEFAULT)

    def test_the_projects_records_setting_names_it_instead(self):
        elsewhere = Path(tempfile.gettempdir()) / "elsewhere"
        with isolation.project(records=str(elsewhere)):
            self.assertEqual(device.records_root(), elsewhere)

    def test_a_relative_records_setting_is_relative_to_the_project(self):
        with isolation.project(records="history/device") as project:
            self.assertEqual(device.records_root(), project / "history" / "device")

    def test_a_captures_default_path_lands_under_the_configured_records(self):
        with isolation.project(records="history") as project:
            path, managed = device.resolve_capture_path(
                None, "listen", "agent", datetime(2026, 9, 16, 12, 30, 45))
        self.assertTrue(managed)
        self.assertIn(project / "history", path.parents)

    def test_an_environment_autana_records_is_ignored(self):
        with isolation.project(), mock.patch.dict(os.environ, {"AUTANA_RECORDS": "elsewhere"}):
            self.assertEqual(device.records_root(), self.DEFAULT)

    def test_a_bad_settings_file_stops_the_command_naming_file_and_key(self):
        with isolation.project() as project:
            (project / "autana.local.toml").write_text("recods = 'x'\n")
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                code = device.main(["status"])
        self.assertEqual(code, 1)
        self.assertIn("autana.local.toml", errors.getvalue())
        self.assertIn("recods", errors.getvalue())


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

    def test_returns_none_and_never_raises_when_git_is_not_installed(self):
        with mock.patch("subprocess.run", side_effect=OSError("git not found")):
            self.assertIsNone(device.git_commit())


class RecordCaptureTests(unittest.TestCase):
    def test_appends_an_index_line_and_leaves_an_explicit_out_uncompressed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            out = Path(directory) / "capture.log"
            out.write_bytes(b"x" * (device.COMPRESS_ABOVE_BYTES + 1))
            started_at = datetime(2026, 9, 16, 12, 30, 45)
            path = device.record_capture(
                out, False, started_at=started_at, board=BOARD, owner="agent",
                purpose="listen", command="listen", commit="deadbeef", reason="complete",
                error=None, root=root)
            self.assertEqual(path, out)
            self.assertTrue(out.is_file())
            lines = (root / "index.jsonl").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            self.assertEqual(json.loads(lines[0]), {
                "started_at": started_at.isoformat(),
                "acquired_at": None,
                "board": BOARD,
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
                log, True, started_at=datetime(2026, 9, 16, 12, 30, 45), board=BOARD,
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
                log, True, started_at=datetime(2026, 9, 16, 12, 30, 45), board=BOARD,
                owner="agent", purpose="run suite", command="run-suite", suite="sand",
                commit=None, reason="complete", error=None, root=root)
            self.assertEqual(path, log)
            self.assertTrue(log.is_file())


class RunSuiteDefaultPathTests(unittest.TestCase):
    def test_without_out_uses_the_default_path_and_writes_an_index_line(self):
        connection = FakeConnection([b":1:test_one:PASS\nRUNSUITE_COMPLETE name=sand found=1\n"])
        args = suite_args(owner="agent a")
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            fixed_now = datetime(2026, 9, 16, 12, 30, 45)
            with recorded_serial(connection, root), \
                 mock.patch.object(device, "now", return_value=fixed_now):
                self.assertEqual(device.run_suite(args, store, BOARD), 0)
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
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "open_serial", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value=None):
                with self.assertRaisesRegex(RuntimeError, "got different"):
                    device.run_suite(args, store, BOARD)
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            self.assertIsNotNone(entry["error"])
            self.assertIn("got different", entry["error"])
            self.assertTrue(Path(entry["capture_path"]).is_file())


class FlashDefaultPathTests(unittest.TestCase):
    def flash(self, directory, run, store):
        worktree = fake_flash.worktree(Path(directory) / "engine")
        root = Path(directory) / "records"
        connection = FakeConnection([])
        args = Namespace(owner="agent", purpose="flash", wait=0, variant="dev",
                         worktree=str(worktree), out=None)
        output = io.StringIO()
        with recorded_serial(connection, root), \
             mock.patch.object(device, "now", return_value=datetime(2026, 9, 16, 12, 30, 45)), \
             mock.patch.object(device, "run_to_end", run), \
             mock.patch.object(device, "find_board", return_value=device.Board(BOARD, BOARD)), \
             mock.patch.object(device, "reconnect_after_reset", return_value=connection) as reconnect, \
             contextlib.redirect_stdout(output):
            with device.build_image(args, BOARD) as built:
                with device.HeldLock(store, BOARD, built.args.owner, "flash", 0) as held:
                    _, connection = device.write_image(built, store, BOARD, held)
                    connection.close()
        self.assertEqual(connection.chunks, [])
        reconnect.assert_called_once_with()
        return worktree, root, output.getvalue()

    def test_flash_uses_esptool_exit_and_build_id_without_boot_console(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree, root, output = self.flash(
                directory, mock.Mock(side_effect=fake_flash.scripts("expected")), mock_store())
            self.assertIn("flashed BUILD_ID=expected (esptool hash verified; boot not verified)",
                          output)
            expected_log = root / "20260916" / "123045_flash-dev_agent.log"
            self.assertTrue(expected_log.is_file())
            self.assertFalse(expected_log.with_suffix(".image").exists())
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            self.assertEqual(entry["capture_path"], str(expected_log))
            self.assertEqual(entry["command"], "flash")
            self.assertEqual(entry["build_id"], "expected")
            self.assertIsNone(entry["error"])
            self.assertEqual(entry["worktree"], str(worktree.resolve()))
            self.assertEqual(entry["commit"], "deadbeef")
            self.assertEqual(entry["image_flags"], [])

    def test_builds_then_passes_the_lock_token_to_flash_image_sh(self):
        # flash_image.sh refuses to flash without the token; write_image()
        # is the one place that has the token to give it.
        with tempfile.TemporaryDirectory() as directory:
            run = mock.Mock(side_effect=fake_flash.scripts("expected"))
            self.flash(directory, run, mock_store("sekrit-token"))
            build, write = run.call_args_list
            self.assertEqual(Path(build.args[0][1]).name, "build.sh")
            self.assertNotIn(autana_config.TOKEN_ENV, build.kwargs["env"])
            self.assertEqual(Path(write.args[0][1]).name, "flash_image.sh")
            self.assertTrue(Path(write.args[0][-1]).name.startswith("autana-image-"))
            self.assertEqual(write.kwargs["env"][autana_config.TOKEN_ENV], "sekrit-token")
            self.assertEqual(write.kwargs["env"][autana_config.BOARD_ENV], BOARD)

    def test_the_private_variables_are_the_ones_flash_image_sh_reads(self):
        script = (DEVICE / "flash_image.sh").read_text(encoding="utf-8")
        self.assertIn("${" + autana_config.TOKEN_ENV + ":-}", script)
        self.assertIn("${" + autana_config.BOARD_ENV + ":-}", script)
        self.assertTrue(autana_config.TOKEN_ENV.startswith("_AUTANA_"))
        self.assertTrue(autana_config.BOARD_ENV.startswith("_AUTANA_"))


class PreparedSnapshotTests(unittest.TestCase):
    """The in-tree link can change while the prepared target stays locked."""
    def test_snapshot_uses_locked_path_after_repoint(self):
        import build_paths
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = fake_flash.worktree(root / 'project')
            launcher = worktree / 'launcher'
            (launcher / 'tools/build/build_paths.py').touch()
            config = worktree / autana_config.CONFIG_NAME
            config.write_text(f"build_root = '{(root / 'first').as_posix()}'\n")
            locked = build_paths.prepare(launcher, device.BUILD_DIRS['dev'])
            image = root / 'image'

            def build(command, lost=None, **options):
                fake_flash.write_image(locked, 'locked-dev')
                config.write_text(f"build_root = '{(root / 'second').as_posix()}'\n")
                redirected = build_paths.prepare(launcher, device.BUILD_DIRS['dev'])
                fake_flash.write_image(redirected, 'redirected-dev')

            with mock.patch.object(device, 'run_to_end', side_effect=build):
                result = device.build_snapshot(['unused'], launcher / device.BUILD_DIRS['dev'], image)
            self.assertEqual(result, 'locked-dev')

    def snapshot_command(self, with_build_paths):
        with tempfile.TemporaryDirectory() as directory:
            worktree = fake_flash.worktree(Path(directory) / 'project')
            launcher = worktree / 'launcher'
            if with_build_paths:
                (launcher / 'tools/build/build_paths.py').touch()
            else:
                self.assertFalse((launcher / 'tools/build/build_paths.py').exists())
            with mock.patch.object(device, 'run_to_end') as run,                  mock.patch.object(device, 'snapshot_image', return_value='id'):
                device.build_snapshot(['build.sh'], launcher / device.BUILD_DIRS['dev'], Path(directory) / 'image')
            return run.call_args.args[0]

    def test_snapshot_of_an_old_project_passes_no_prepared_build(self):
        self.assertEqual(self.snapshot_command(False), ['build.sh'])

    def test_snapshot_passes_prepared_build_when_the_project_has_build_paths(self):
        self.assertIn('--prepared-build', self.snapshot_command(True))


class BuildWorktreeTests(unittest.TestCase):
    """build_worktree(), behind `autana build`: the build half of a flash,
    touching no board and holding heavy only."""

    def main(self, variant, flags=(), run=None, build_paths=False):
        with tempfile.TemporaryDirectory() as directory:
            worktree = fake_flash.worktree(Path(directory) / "engine")
            if build_paths:
                (worktree / "launcher/tools/build/build_paths.py").touch()
            calls = []
            with mock.patch.object(device, "run_to_end",
                                   side_effect=run or (lambda command, lost=None, **options:
                                                       calls.append((command, lost, options)))), \
                 mock.patch.object(device, "board_for_lock", side_effect=AssertionError("board")):
                code = device.build_worktree(worktree, variant, flags)
        return code, calls

    def test_builds_the_variant_with_no_board_and_heavy_lock(self):
        code, calls = self.main("diag", ["--perf-scope"], build_paths=True)
        self.assertEqual(code, 0)
        (command, lost, options), = calls
        self.assertEqual([Path(command[1]).name] + command[2:-2],
                         ["build.sh", "--diag", "--perf-scope"])
        self.assertEqual(command[-2], "--prepared-build")
        self.assertEqual(Path(command[-1]).name, device.BUILD_DIRS["diag"])
        self.assertIsNotNone(lost)
        self.assertIn(autana_config.HEAVY_TOKEN_ENV, options["env"])
        self.assertNotIn(autana_config.TOKEN_ENV, options["env"])

    def test_an_old_project_gets_no_prepared_build_option(self):
        code, calls = self.main("diag", ["--perf-scope"])
        self.assertEqual(code, 0)
        (command, _, _), = calls
        self.assertEqual([Path(command[1]).name] + command[2:], ["build.sh", "--diag", "--perf-scope"])

    def test_a_failed_build_is_the_commands_exit_status(self):
        def failing(command, lost=None, **unused_options):
            raise subprocess.CalledProcessError(2, command)

        code, _ = self.main("dev", run=failing)
        self.assertEqual(code, 2)


class FlashCommandLineTests(unittest.TestCase):
    """main()'s own `flash` dispatch: --perf-scope becomes an extra_flags
    entry, the same way batch() and selftest() already build theirs."""

    def run_main(self, argv):
        calls = []
        connection = mock.Mock()
        store = mock_store()

        def fake_write_image(built, lock_store, board, held):
            self.assertIs(held, device.ACTIVE_LOCK.held)
            self.assertIs(held.store, store)
            connection.close.side_effect = lambda: self.assertIs(device.ACTIVE_LOCK.held, held)
            return "abc", connection

        def fake_build_image(args, port, extra_flags=()):
            calls.append(list(extra_flags))
            return contextlib.nullcontext("built")

        with mock.patch.object(device, "board_for_lock", return_value=(BOARD, None)), \
             mock.patch.object(device, "build_image", fake_build_image), \
             mock.patch.object(device, "write_image", side_effect=fake_write_image), \
             mock.patch.object(device, "await_console", side_effect=lambda args, board, build_id, acquired, connection: connection.close()), \
             mock.patch.object(device.device_lock, "LockStore", return_value=store):
            device.main(argv)
        connection.close.assert_called_once_with()
        return calls

    def test_successful_flash_closes_reconnected_handle(self):
        self.run_main(["--owner", "a", "flash", "--variant", "dev", "--worktree", "C:/wt"])

    def test_standalone_flash_records_one_flash_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = fake_flash.worktree(Path(directory) / "engine")
            store = mock_store()
            store.root = Path(directory) / "lock"
            connection = FakeConnection([b"BUILD_ID=abc\n" + device.CONSOLE_UP])
            with recorded_serial(connection, Path(directory) / "records"), \
                 mock.patch.object(device, "board_for_lock", return_value=(BOARD, None)), \
                 mock.patch.object(device.device_lock, "LockStore", return_value=store), \
                 mock.patch.object(device, "run_to_end", side_effect=fake_flash.scripts()), \
                 mock.patch.object(device, "find_board", return_value=device.Board(BOARD, BOARD)), \
                 mock.patch.object(device, "reconnect_after_reset", return_value=connection):
                code = device.main(["--owner", "a", "flash", "--variant", "dev",
                                    "--worktree", str(worktree)])
            self.assertEqual(code, 0)
            rows = device_lock.duration_rows(store.root)
            self.assertEqual([row["command"] for row in rows], ["flash"])
            self.assertIsNone(rows[0]["error"])

    def test_perf_scope_flag_becomes_an_extra_flag(self):
        calls = self.run_main(["--owner", "a", "flash", "--variant", "diag",
                               "--worktree", "C:/wt", "--perf-scope"])
        self.assertEqual(calls, [["--perf-scope"]])

    def test_layout_seed_becomes_an_extra_flag(self):
        calls = self.run_main(["--owner", "a", "flash", "--variant", "diag",
                               "--worktree", "C:/wt", "--layout-seed", "7"])
        self.assertEqual(calls, [["--layout-seed", "7"]])

    def test_no_perf_scope_flag_passes_nothing_extra(self):
        calls = self.run_main(["--owner", "a", "flash", "--variant", "dev", "--worktree", "C:/wt"])
        self.assertEqual(calls, [[]])


class ReportCommandTests(unittest.TestCase):
    """The `report` subcommand must work over a file already on disk without
    ever touching the board, no port lookup, no lock, no serial."""

    def test_never_looks_for_a_port_and_writes_the_report_file(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_text(":1:test_one:PASS\n:2:test_two:FAIL: boom\n")
            with mock.patch.object(device, "plugged_boards",
                                   side_effect=AssertionError("must not look for a board")):
                exit_code = device.main(["report", str(capture), "--index",
                                         str(Path(directory) / "index.jsonl")])
            self.assertEqual(exit_code, 0)
            report_path = capture.with_name("capture.md")
            self.assertTrue(report_path.is_file())
            self.assertIn("PASS: 1  FAIL: 1", report_path.read_text(encoding="utf-8"))

    def test_a_missing_capture_fails_without_raising(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "nope.log"
            with mock.patch.object(device, "plugged_boards",
                                   side_effect=AssertionError("must not look for a board")):
                exit_code = device.main(["report", str(missing)])
        self.assertEqual(exit_code, 1)


class RunSuiteReportGenerationTests(unittest.TestCase):
    """run_suite() must hand its finished capture to device_report so a
    result is readable without opening the raw log, see device_report.py."""

    def test_writes_a_report_beside_the_default_path_capture(self):
        connection = FakeConnection([
            b":1:test_one:PASS\n:2:test_two:FAIL: Expected 1 Was 0\nRUNSUITE_COMPLETE name=sand found=1\n"
        ])
        args = suite_args()
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            fixed_now = datetime(2026, 9, 16, 12, 30, 45)
            with recorded_serial(connection, root), \
                 mock.patch.object(device, "now", return_value=fixed_now):
                self.assertEqual(device.run_suite(args, store, BOARD), 1)
            report_path = root / "20260916" / "123045_runsuite-sand_agent.md"
            self.assertTrue(report_path.is_file())
            text = report_path.read_text(encoding="utf-8")
            self.assertIn("PASS: 1  FAIL: 1", text)
            self.assertIn("test_two", text)

    def test_the_run_ends_with_the_report_path_and_its_errors(self):
        connection = FakeConnection([
            b":1:test_one:PASS\nE (7) sand: bad grain\nRUNSUITE_COMPLETE name=sand found=1\n"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with recorded_serial(connection, root),                  mock.patch("sys.stdout", new_callable=io.StringIO) as out:
                device.run_suite(suite_args(), mock_store(), BOARD)
        lines = out.getvalue().splitlines()
        report = [i for i, line in enumerate(lines) if line.startswith("report: ")]
        self.assertEqual(len(report), 1)
        self.assertIn("E (7) sand: bad grain", lines[report[0] + 1])
        self.assertGreater(report[0], max(i for i, line in enumerate(lines) if "capture ended" in line) - 1)

    def test_a_broken_reporter_step_does_not_fail_the_suite_command(self):
        connection = FakeConnection([b":1:test_one:PASS\nRUNSUITE_COMPLETE name=sand found=1\n"])
        args = suite_args()
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with recorded_serial(connection, root), \
                 mock.patch.object(device_report, "write_report_for_capture",
                                   side_effect=RuntimeError("disk full")):
                self.assertEqual(device.run_suite(args, store, BOARD), 0)


class RunSuiteRecordsWorktreeTests(unittest.TestCase):
    """The manifest documents 'the worktree and commit involved' for every
    invocation (Device-Lock.md); run-suite used to leave worktree null."""

    def test_run_suite_records_the_ambient_cwd_as_worktree(self):
        connection = FakeConnection([b":1:test_one:PASS\nRUNSUITE_COMPLETE name=sand found=1\n"])
        args = suite_args()
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with recorded_serial(connection, root), \
                 mock.patch.object(device.Path, "cwd", return_value=Path("C:/some/worktree")):
                device.run_suite(args, store, BOARD)
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        self.assertEqual(entry["worktree"], str(Path("C:/some/worktree")))


FILTERED_DONE = (b"SUITE_TEST name=test_fire_fits selected=1\n"
                 b":1:test_fire_fits:PASS\n"
                 b"\nRUNSUITE_COMPLETE name=sand found=1 selected=1 unmatched=0\n")
NOTHING_MATCHED = (b"SUITE_TEST name=test_gas_fits selected=0\n"
                   b"SUITE_TEST name=test_water_fits selected=0\n"
                   b"\nRUNSUITE_COMPLETE name=sand found=1 selected=0 unmatched=1\n")


class TuneSetTests(unittest.TestCase):
    """--set: each value is SET on the held port before RUNSUITE, and a
    value the board does not echo back stops the capture before it runs."""

    def apply(self, setting, *chunks):
        connection = FakeConnection(list(chunks))
        with mock.patch.object(device, "EXCHANGE_SECONDS", 0.05), mock.patch("builtins.print"):
            device.apply_tunables(connection, [setting])
        return b"".join(connection.writes)

    def test_an_echoed_value_is_set(self):
        self.assertEqual(self.apply("r3d.side=3", b"TUNE_OK r3d.side=3\n"), b"\nSET r3d.side 3\n")

    def test_hex_is_compared_as_the_board_prints_it(self):
        self.apply("a.rgb=0x10", b"log line\nI (5) console: TUNE_OK a.rgb=16\n")

    def test_a_dropped_line_is_asked_again(self):
        class AnswersSecondAsk(FakeConnection):
            def read(self, unused_size):
                return b"TUNE_OK r3d.side=3\n" if len(self.writes) == 2 else b""

        connection = AnswersSecondAsk()
        with mock.patch.object(device, "EXCHANGE_SECONDS", 0.05), mock.patch("builtins.print"):
            device.apply_tunables(connection, ["r3d.side=3"])
        self.assertEqual(len(connection.writes), 2)

    def test_a_refusal_a_wrong_echo_or_silence_stops(self):
        for chunks in ([b"TUNE_ERR range r3d.side 99\n"], [b"TUNE_OK r3d.side=2\n"], []):
            with self.subTest(chunks=chunks), self.assertRaises(device.TuneRefused):
                self.apply("r3d.side=3", *chunks)

    def test_the_set_precedes_the_run_request(self):
        connection = FakeConnection([b"TUNE_OK r3d.side=3\n", b"RUNSUITE_COMPLETE name=sand found=1\n"])
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand", out=None, max_seconds=1,
                         idle_seconds=None, expect_build_id=None, test_filter=[], tune_set=["r3d.side=3"])
        with tempfile.TemporaryDirectory() as directory:
            args.out = str(Path(directory) / "capture.log")
            with mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=Path(directory) / "records"), \
                 mock.patch("builtins.print"):
                device.run_suite(args, mock_store(), BOARD)
        self.assertEqual(b"".join(connection.writes), b"\nSET r3d.side 3\n\nRUNSUITE sand\n")


class ConsoleSetupTests(unittest.TestCase):
    def run_setup(self, replies, lines):
        connection = FakeConnection(replies)
        args = suite_args()
        args.console_send = lines
        with tempfile.TemporaryDirectory() as directory:
            args.out = str(Path(directory) / "capture.log")
            with mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=Path(directory) / "records"), \
                 mock.patch.object(device, "EXCHANGE_SECONDS", 0.001), \
                 mock.patch("builtins.print"):
                try:
                    device.run_suite(args, mock_store(), BOARD)
                finally:
                    self.writes = connection.writes
                    self.entries = [json.loads(line) for line in
                                    (Path(directory) / "records/index.jsonl").read_text().splitlines()]
        return connection

    def test_lines_precede_runsuite_and_are_recorded(self):
        lines = ["sound stop all", "sound play tap loop"]
        self.run_setup([b"SOUND_END\n", b"SOUND playing\nSOUND_END\n",
                        b"RUNSUITE_COMPLETE name=sand found=1\n"], lines)
        self.assertEqual(self.writes[:2], [b"\nsound stop all\n", b"\nsound play tap loop\n"])
        self.assertIn(b"RUNSUITE", self.writes[2])
        self.assertEqual(self.entries[0]["console_send"], lines)

    def test_error_and_partial_timeout_stop_before_runsuite(self):
        for reply in (b"SOUND pending no clip\nSOUND_ERR missing\n",
                      b"SOUND pending no clip\n", b""):
            with self.subTest(reply=reply), self.assertRaisesRegex(device.BatchEnding, "sound play missing") as error:
                self.run_setup([reply], ["sound play missing"])
            self.assertFalse(any(b"RUNSUITE" in write for write in self.writes))
            if reply:
                self.assertIn("SOUND pending no clip", str(error.exception))
                self.assertEqual(len(self.writes), 1)

    def test_partial_reply_is_never_retried(self):
        connection = FakeConnection()
        with mock.patch.object(device, "exchange", return_value=(["SOUND pending"], False, True)) as exchange:
            with self.assertRaises(device.ConsoleRefused):
                device.apply_console_lines(connection, ["sound stop all", "sound play tap loop"])
        self.assertEqual(exchange.call_count, 1)

    def test_silence_restarts_the_sequence(self):
        connection = FakeConnection()
        with mock.patch.object(device, "exchange", side_effect=[
                ([device.reply_prefix("sound stop all")[1][0]], True, False), ([], False, False),
                ([device.reply_prefix("sound stop all")[1][0]], True, False),
                ([device.reply_prefix("sound play tap loop")[1][0]], True, False)]) as exchange, mock.patch("builtins.print"):
            device.apply_console_lines(connection, ["sound stop all", "sound play tap loop"])
        self.assertEqual([call.args[1] for call in exchange.call_args_list],
                         ["sound stop all", "sound play tap loop"] * 2)

    def test_silence_is_retried(self):
        connection = FakeConnection()
        def reply_on_retry(unused_size):
            return b"SOUND_END\n" if len(connection.writes) == 2 else b""
        with mock.patch.object(connection, "read", side_effect=reply_on_retry), \
             mock.patch.object(device, "EXCHANGE_SECONDS", 0.001), mock.patch("builtins.print"):
            device.apply_console_lines(connection, ["sound stop all"])
        self.assertEqual(connection.writes, [b"\nsound stop all\n"] * 2)


class TestFilterRunTests(unittest.TestCase):
    """`--test` narrows a suite on the board: the request carries the
    patterns, a pattern that selects nothing or one the board cannot take is
    an error, and the records say which rows ran. What the firmware really
    prints is pinned in launcher/test/tests/test_suite_filter_output.py."""

    def run_filtered(self, chunks, patterns):
        connection = FakeConnection(chunks)
        args = Namespace(owner="agent", purpose="test", wait=0, suite="sand", out=None,
                         max_seconds=1, idle_seconds=None, expect_build_id=None,
                         test_filter=patterns)
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            records = Path(directory) / "records"
            args.out = str(Path(directory) / "capture.log")
            with mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=records), \
                 mock.patch("builtins.print"):
                try:
                    code = device.run_suite(args, store, BOARD)
                    error = None
                except RuntimeError as caught:
                    code, error = None, caught
            index = records / "index.jsonl"
            manifest = [json.loads(line) for line in index.read_text(encoding="utf-8").splitlines()]
        return code, error, connection.writes, manifest

    def test_the_patterns_ride_the_run_request(self):
        _, _, writes, _ = self.run_filtered([FILTERED_DONE], ["fire", "gas"])
        self.assertEqual(b"".join(writes), b"\nRUNSUITE sand fire,gas\n")

    def test_no_filter_sends_only_the_suite_name(self):
        _, _, writes, _ = self.run_filtered([b"RUNSUITE_COMPLETE name=sand found=1\n"], [])
        self.assertEqual(b"".join(writes), b"\nRUNSUITE sand\n")

    def test_a_filtered_run_completes_and_records_its_filter(self):
        code, error, _, manifest = self.run_filtered([FILTERED_DONE], ["fire"])
        self.assertEqual((code, error), (0, None))
        self.assertEqual(manifest[-1]["test_filter"], ["fire"])

    def test_a_pattern_matching_nothing_fails_listing_the_tests(self):
        _, error, _, _ = self.run_filtered([NOTHING_MATCHED], ["fyre"])
        self.assertIsInstance(error, device.NoTestMatched)
        self.assertIn("test_gas_fits", str(error))
        self.assertIn("test_water_fits", str(error))

    def test_a_refused_pattern_is_a_filter_error(self):
        chunks = [b"SUITE_FILTER_REFUSED pattern=xxxx\n"
                  b"\nRUNSUITE_COMPLETE name=sand found=1 selected=0 unmatched=0\n"]
        _, error, _, _ = self.run_filtered(chunks, ["xxxx"])
        self.assertIsInstance(error, device.TestFilterError)
        self.assertNotIsInstance(error, device.NoTestMatched)
        self.assertIn("xxxx", str(error))

    def test_an_image_that_predates_the_filter_is_a_filter_error(self):
        # It reads "sand fire" as a suite name and answers found=0 with no counts.
        chunks = [b"\nRUNSUITE_COMPLETE name=sand fire found=0\n"]
        _, error, _, _ = self.run_filtered(chunks, ["fire"])
        self.assertIsInstance(error, device.TestFilterError)
        self.assertRegex(str(error), "^this image predates --test")

    def test_a_filtered_truncated_run_is_a_capture_error(self):
        _, error, _, _ = self.run_filtered([b":1:good:PASS\n"], ["fire"])
        self.assertIsInstance(error, RuntimeError)
        self.assertNotIsInstance(error, device.TestFilterError)
        self.assertRegex(str(error), "no completion line arrived: the capture ended timeout")

    def test_an_unfiltered_truncated_run_is_a_capture_error(self):
        _, error, _, _ = self.run_filtered([b":1:good:PASS\n"], [])
        self.assertIsInstance(error, RuntimeError)
        self.assertNotIsInstance(error, device.TestFilterError)
        self.assertRegex(str(error), "no completion line arrived: the capture ended timeout")

    def test_a_request_too_long_for_the_console_line_is_a_filter_error(self):
        connection = FakeConnection([b"W (5) console: console line too long (max 48) - dropped\n"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(device.TestFilterError):
                device.capture(connection, Path(directory) / "capture.log", 1, None,
                               suite_name="sand")

    def test_patterns_split_on_commas_and_repeat_without_duplicates(self):
        self.assertEqual(device.test_patterns(["fire,gas", "water", "gas"]),
                         ["fire", "gas", "water"])
        self.assertEqual(device.test_patterns(None), [])

    def test_a_pattern_that_would_break_the_request_line_is_refused(self):
        for bad in ("has space", "", "a;b"):
            with self.assertRaises(device.TestFilterError, msg=bad):
                device.test_patterns([bad])

    def test_no_test_matched_is_a_test_filter_error_and_a_runtime_error(self):
        self.assertTrue(issubclass(device.NoTestMatched, device.TestFilterError))
        self.assertTrue(issubclass(device.TestFilterError, RuntimeError))


class BatchTests(unittest.TestCase):
    """A batch flashes once and captures every suite N times under ONE lock,
    the property that stops another agent flashing between two captures of
    the same image. No serial port, lock file or build is touched here."""

    def test_console_lines_forwarded_to_every_run(self):
        lines = ["sound stop all", "sound play tap loop"]
        _, calls, _, _ = self.run_batch(suites=("first", "second"), runs=2, console_send=lines)
        self.assertEqual([args.console_send for args in calls["suite_args"]], [lines] * 4)

    def run_batch(self, suites=("run_sand_perf_suite",), runs=3, fail_run=None,
                  perf_scope=False, script_text="--diag --dev --perf-scope", out=False,
                  expect_build_id=None, flashed_build_id="abc123-diag", flash=True,
                  test_filter=None, filter_error_run=None, error_class=None,
                  layout_seed=0, truncated_run=None, console_send=None):
        real_run_suite = device.run_suite
        calls = {"locks": 0, "build": [], "flash": [], "run_suite": [], "events": [],
                 "suite_args": []}

        class FakeLock:
            def __init__(self, *unused, **unused_keywords):
                calls["locks"] += 1
                self.held = {"acquired_at": 1000.0}
                self.error = None

            def __enter__(self):
                calls["events"].append("lock")
                return self

            def __exit__(self, *unused):
                calls["events"].append("unlock")
                return False

        def fake_build_image(args, port, extra_flags=()):
            calls["events"].append("build")
            calls["build"].append(list(extra_flags))
            return contextlib.nullcontext("built")

        def fake_write_image(built, store, port, held_lock):
            calls["events"].append("flash")
            calls["flash"].append((held_lock, built))
            calls["flash_connection"] = mock.Mock()
            return flashed_build_id, calls["flash_connection"]

        def fake_run_suite(args, store, port, held_lock=None, worktree=None, commit=None):
            calls["events"].append("capture")
            calls["run_suite"].append((args.suite, args.out, args.purpose, held_lock,
                                       args.expect_build_id, worktree, commit))
            calls["suite_args"].append(args)
            if filter_error_run is not None and len(calls["run_suite"]) == filter_error_run:
                raise (error_class or device.TestFilterError)("the board cannot filter")
            if truncated_run == len(calls["run_suite"]):
                args.idle_seconds = 0
                with mock.patch.object(device, "holding", return_value=contextlib.nullcontext(held_lock)), \
                     mock.patch.object(device, "open_when_free",
                                       return_value=FakeConnection([b":1:test_one:PASS\n"])):
                    return real_run_suite(args, store, port, held_lock, worktree, commit)
            if args.out:
                Path(args.out).write_text(":1:test_one:PASS\n", encoding="utf-8")
            if fail_run is not None and len(calls["run_suite"]) == fail_run:
                raise RuntimeError("capture timed out")
            return 0

        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "wt"
            (worktree / "launcher" / "tools" / "build").mkdir(parents=True)
            (worktree / "launcher" / "tools" / "build" / "build.sh").write_text(script_text)
            calls["worktree"] = str(worktree.resolve())
            out_path = str(Path(directory) / "raw.txt") if out else None
            args = Namespace(owner="agent", purpose="p", wait=0, worktree=str(worktree),
                             variant="diag", suite=list(suites), runs=runs, perf_scope=perf_scope,
                             max_seconds=1, idle_seconds=None, out=out_path,
                             expect_build_id=expect_build_id, flash=flash,
                             test_filter=test_filter, layout_seed=layout_seed, console_send=console_send)
            store = mock.Mock()
            with mock.patch.object(device, "HeldLock", FakeLock), \
                 mock.patch.object(device, "build_image", fake_build_image), \
                 mock.patch.object(device, "write_image", fake_write_image), \
                 mock.patch.object(device, "run_suite", fake_run_suite), \
                 mock.patch.object(device, "await_console",
                                   lambda *unused: calls["events"].append("boot")), \
                 mock.patch.object(device, "records_root", return_value=Path(directory) / "rec"), \
                 mock.patch.object(device, "git_commit", return_value="c0ffee"), \
                 mock.patch("builtins.print"):
                try:
                    code = device.batch(args, store, BOARD)
                except device.TestFilterError:
                    code = "filter error"
                except RuntimeError:
                    if expect_build_id and expect_build_id != flashed_build_id:
                        calls["flash_connection"].close.assert_called_once_with()
                        self.assertEqual(calls["run_suite"], [])
                    raise
                summaries = list((Path(directory) / "rec").rglob("*_batch_*.md"))
                summary = summaries[0].read_text(encoding="utf-8") if summaries else ""
                index = Path(directory) / "rec" / "index.jsonl"
                manifest = [json.loads(line) for line in index.read_text(encoding="utf-8").splitlines()] \
                    if index.exists() else []
        return code, calls, summary, manifest

    def test_the_command_line_flag_reaches_batch_as_the_filter(self):
        seen = []
        with mock.patch.object(device, "board_for_lock", return_value=(BOARD, None)), \
             mock.patch.object(device, "device_lock") as fake_lock_module, \
             mock.patch.object(device, "batch", side_effect=lambda args, *unused: seen.append(args) or 0):
            fake_lock_module.LockStore.return_value = mock.Mock()
            device.main(["--owner", "a", "batch", "--worktree", "C:/wt", "--suite", "sand",
                         "--test", "fire,gas", "--test", "water"])
        self.assertEqual(seen[0].test_filter, ["fire,gas", "water"])

    def test_the_filter_reaches_every_capture_as_patterns(self):
        _, calls, _, _ = self.run_batch(runs=2, test_filter=["fire,gas", "water"])
        self.assertEqual([args.test_filter for args in calls["suite_args"]],
                         [["fire", "gas", "water"]] * 2)

    def test_an_unfiltered_batch_asks_for_no_filter(self):
        _, calls, _, _ = self.run_batch(runs=1)
        self.assertEqual(calls["suite_args"][0].test_filter, [])

    def test_a_filter_error_ends_the_batch_at_the_first_capture_and_frees_the_lock(self):
        for error_class in (device.TestFilterError, device.NoTestMatched):
            code, calls, _, _ = self.run_batch(runs=3, test_filter=["fire"], filter_error_run=1,
                                               error_class=error_class)
            self.assertEqual(code, "filter error")
            self.assertEqual(len(calls["run_suite"]), 1)
            self.assertEqual(calls["events"][-1], "unlock")

    def test_an_ordinary_capture_error_in_a_filtered_batch_still_continues(self):
        code, calls, _, _ = self.run_batch(runs=3, test_filter=["fire"], fail_run=2)
        self.assertEqual(code, 1)
        self.assertEqual(len(calls["run_suite"]), 3)

    def test_a_bad_pattern_is_refused_before_anything_is_built(self):
        code, calls, _, _ = self.run_batch(test_filter=["has space"])
        self.assertEqual(code, "filter error")
        self.assertEqual(calls["events"], [])

    def test_the_summary_and_manifest_name_the_filter(self):
        _, _, summary, manifest = self.run_batch(runs=2, test_filter=["fire", "gas"])
        self.assertIn("Test filter: `fire, gas`", summary)
        self.assertEqual(manifest[-1]["test_filter"], ["fire", "gas"])

    def test_an_unfiltered_summary_names_no_filter(self):
        _, _, summary, manifest = self.run_batch(runs=2)
        self.assertNotIn("Test filter", summary)
        self.assertNotIn("test_filter", manifest[-1])

    def test_one_lock_one_flash_for_every_capture(self):
        code, calls, _, _ = self.run_batch(suites=("run_sand_perf_suite", "run_gfx_suite"), runs=3)
        self.assertEqual(code, 0)
        self.assertEqual(calls["locks"], 1)
        self.assertEqual(len(calls["flash"]), 1)
        self.assertEqual(len(calls["run_suite"]), 6)

    def test_builds_before_the_lock_then_flashes_and_captures_under_it(self):
        _, calls, _, _ = self.run_batch(runs=2)
        self.assertEqual(calls["events"],
                         ["build", "lock", "flash", "boot", "capture", "capture", "unlock"])
        self.assertEqual(calls["flash"][0][1], "built")

    def test_every_capture_runs_inside_the_batch_lock_on_the_flashed_build(self):
        _, calls, _, _ = self.run_batch(runs=2)
        batch_lock = calls["flash"][0][0]
        self.assertIsNotNone(batch_lock)
        for suite, out, purpose, held_lock, expected, unused_worktree, unused_commit in calls["run_suite"]:
            self.assertIs(held_lock, batch_lock)
            self.assertEqual(expected, "abc123-diag")

    def test_run_suite_entries_carry_the_batch_worktree_and_its_commit(self):
        """Each run-suite call must be told the batch's own --worktree and
        that worktree's HEAD, not the ambient cwd, see device.py's
        run_suite() docstring and the manifest bug this replaced."""
        _, calls, _, _ = self.run_batch(runs=1)
        for suite, out, purpose, unused_held_lock, unused_expected, worktree, commit in calls["run_suite"]:
            self.assertEqual(worktree, calls["worktree"])
            self.assertEqual(commit, "c0ffee")

    def test_a_truncated_unfiltered_capture_is_a_run_error(self):
        code, calls, summary, manifest = self.run_batch(runs=2, truncated_run=1)
        self.assertEqual(code, 1)
        self.assertEqual(len(calls["run_suite"]), 2)
        self.assertIn("- run 1: no completion line arrived: the capture ended idle", summary)
        self.assertIn("no completion line arrived", manifest[-1]["error"])

    def test_a_capture_error_is_recorded_and_the_batch_continues(self):
        code, calls, summary, _ = self.run_batch(runs=3, fail_run=2)
        self.assertEqual(code, 1)
        self.assertEqual(len(calls["run_suite"]), 3)
        self.assertIn("- run 2: capture timed out", summary)

    def test_no_flash_skips_the_build_and_flash_but_still_locks_and_captures(self):
        code, calls, _, _ = self.run_batch(
            suites=("run_sand_perf_suite", "run_gfx_suite"), runs=2, flash=False)
        self.assertEqual(code, 0)
        self.assertEqual(calls["locks"], 1)
        self.assertEqual(calls["build"], [])
        self.assertEqual(calls["flash"], [])
        self.assertEqual(calls["events"], ["lock", "capture", "capture", "capture", "capture",
                                           "unlock"])

    def test_no_flash_passes_expect_build_id_straight_to_every_capture(self):
        _, calls, _, _ = self.run_batch(runs=2, flash=False, expect_build_id="abc123-diag")
        for suite, out, purpose, held_lock, expected, unused_worktree, unused_commit in calls["run_suite"]:
            self.assertEqual(expected, "abc123-diag")

    def test_no_flash_with_no_expect_build_id_checks_nothing(self):
        _, calls, _, _ = self.run_batch(runs=1, flash=False)
        self.assertIsNone(calls["run_suite"][0][4])

    def test_layout_seed_without_flash_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "--layout-seed.*--flash"):
            self.run_batch(flash=False, layout_seed=3)

    def test_perf_scope_without_flash_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "--perf-scope.*--flash"):
            self.run_batch(flash=False, perf_scope=True)

    def test_a_single_entry_skips_the_summary_file_and_its_manifest_row(self):
        """One suite, one run: exactly what a bare `run-suite` would leave
        behind; the summary and its own "batch" manifest row are for
        telling several captures apart, and a lone capture has nothing to
        tell apart."""
        code, calls, summary, manifest = self.run_batch(
            suites=("run_sand_perf_suite",), runs=1, flash=False)
        self.assertEqual(code, 0)
        self.assertEqual(summary, "")
        self.assertNotIn("batch", [entry["command"] for entry in manifest])

    def test_a_single_entry_names_its_capture_like_run_suite_would(self):
        _, calls, _, _ = self.run_batch(suites=("run_sand_perf_suite",), runs=1, flash=False)
        self.assertIsNone(calls["run_suite"][0][1])

    def test_a_single_entry_with_out_still_uses_it_directly(self):
        _, calls, _, _ = self.run_batch(
            suites=("run_sand_perf_suite",), runs=1, flash=False, out=True)
        self.assertTrue(calls["run_suite"][0][1].endswith("raw.txt"))

    def test_a_single_entry_purpose_carries_no_run_suffix(self):
        _, calls, _, _ = self.run_batch(suites=("run_sand_perf_suite",), runs=1, flash=False)
        self.assertEqual(calls["run_suite"][0][2], "p")

    def test_a_single_entry_skips_the_summary_even_with_flash(self):
        code, calls, summary, manifest = self.run_batch(
            suites=("run_sand_perf_suite",), runs=1, flash=True)
        self.assertEqual(code, 0)
        self.assertEqual(summary, "")
        self.assertNotIn("batch", [entry["command"] for entry in manifest])

    def test_several_entries_still_get_the_summary_and_manifest_row(self):
        code, calls, summary, manifest = self.run_batch(
            suites=("run_sand_perf_suite",), runs=2, flash=False)
        self.assertEqual(code, 0)
        self.assertNotEqual(summary, "")
        self.assertIn("batch", [entry["command"] for entry in manifest])

    def test_writes_one_summary_for_the_batch(self):
        _, _, summary, _ = self.run_batch(runs=2)
        self.assertIn("# Device Batch Report", summary)
        self.assertIn("- Build Id: `abc123-diag`", summary)

    def test_perf_scope_is_passed_to_the_build(self):
        _, calls, _, _ = self.run_batch(perf_scope=True)
        self.assertEqual(calls["build"], [["--perf-scope"]])

    def test_layout_seed_is_passed_to_the_build(self):
        _, calls, _, _ = self.run_batch(layout_seed=3)
        self.assertEqual(calls["build"], [["--layout-seed", "3"]])

    def test_out_is_used_for_one_suite_one_run(self):
        _, calls, _, _ = self.run_batch(suites=("run_sand_perf_suite",), runs=1, out=True)
        self.assertTrue(calls["run_suite"][0][1].endswith("raw.txt"))

    def test_out_with_more_than_one_run_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "--out only makes sense"):
            self.run_batch(suites=("run_sand_perf_suite",), runs=2, out=True)

    def test_out_with_more_than_one_suite_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "--out only makes sense"):
            self.run_batch(suites=("run_sand_perf_suite", "run_gfx_suite"), runs=1, out=True)

    def test_expect_build_id_matching_the_flash_runs_normally(self):
        code, calls, _, _ = self.run_batch(runs=1, expect_build_id="abc123-diag")
        self.assertEqual(code, 0)
        self.assertEqual(len(calls["run_suite"]), 1)

    def test_expect_build_id_mismatch_refuses_to_run_any_suite(self):
        with self.assertRaisesRegex(RuntimeError, "build id mismatch"):
            self.run_batch(runs=3, expect_build_id="other-build", flashed_build_id="abc123-diag")


class ToolchainAddr2LineTests(unittest.TestCase):
    """toolchain_addr2line() looks under IDF_TOOLS_PATH when set (the same
    override ESP-IDF's own install script honours) and ~/.espressif
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

    def test_a_frame_watch_warning_is_not_a_crash(self):
        data = b"W (5123) frame_watch: FRAME_WATCH alloc in 16 of 16 frames at 0x4201abcd\n"
        with mock.patch.object(device, "toolchain_addr2line",
                               side_effect=AssertionError("must not be called")):
            self.assertEqual(device.decode_crash_addresses(data, Path("x.elf")), [])


FRAME_WATCH_SOURCE = Path(__file__).resolve().parents[3] / "launcher" / "main" / "profile" / "frame_watch.c"


class FrameWatchLineTests(unittest.TestCase):
    """The firmware's FRAME_WATCH warning, as device.py reads it: echoed by a
    quiet capture, its heap sites decoded apart from a crash's."""

    def firmware_lines(self, kind, site):
        """The warning as each of frame_watch.c's FRAME_WATCH formats prints it."""
        source = FRAME_WATCH_SOURCE.read_text(encoding="utf-8")
        formats = re.findall(r'"(FRAME_WATCH [^"]*)", kind', source)
        self.assertEqual(len(formats), 2, "frame_watch.c's FRAME_WATCH warnings changed shape")
        lines = []
        for form in formats:
            line = form.replace("%s", kind, 1).replace("%d", "9", 1).replace("%d", "16", 1)
            lines.append("W (5123) frame_watch: " + line.replace("0x%08lx", site).replace("%.*s", "a format"))
        return lines

    def firmware_line(self, kind, site):
        return self.firmware_lines(kind, site)[0]

    def test_every_firmware_line_matches_what_device_py_parses(self):
        for line in self.firmware_lines("alloc", "0x4201abcd"):
            match = device.FRAME_WATCH_LINE_RE.search(line)
            self.assertIsNotNone(match, line)
            self.assertEqual(match.groups(), ("alloc", "9", "16", "0x4201abcd"))

    def test_a_quiet_capture_echoes_a_frame_watch_warning(self):
        output = io.StringIO()
        sink = device.ErrorLineSink(output)
        sink.write(b"I (10) shell: ordinary\n" + self.firmware_line("free", "0x4201abcd").encode() + b"\n")
        sink.finish()
        self.assertNotIn("ordinary", output.getvalue())
        self.assertIn("FRAME_WATCH free in 9 of 16 frames at 0x4201abcd", output.getvalue())

    def test_heap_sites_are_decoded_and_a_log_site_is_not(self):
        data = (self.firmware_line("alloc", "0x4201abcd") + "\n" +
                self.firmware_line("console", "0x3c10e270") + "\n").encode()
        result = mock.Mock(stdout="ui.c:88\n")
        with mock.patch.object(device, "toolchain_addr2line", return_value=Path("addr2line")), \
             mock.patch.object(device.subprocess, "run", return_value=result) as run:
            decoded = device.decode_frame_watch_sites(data, Path("x.elf"))
        self.assertEqual(decoded, ["ui.c:88"])
        command = run.call_args[0][0]
        self.assertIn("0x4201abcd", command)
        self.assertNotIn("0x3c10e270", command)


class FindElfForBuildIdTests(unittest.TestCase):
    """find_elf_for_build_id() picks the build actually on the board: the
    one whose own build_id.txt matches; never merely the newest on disk."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.archive = Path(directory.name) / "elf"
        patcher = mock.patch.object(device, "elf_archive", return_value=self.archive)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_core_dumps_longer_hash_names_the_same_build(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory)
            build = worktree / "launcher" / "build.diag"
            build.mkdir(parents=True)
            (build / "build_id.txt").write_text("834b7bb03fd6-diag\n", encoding="ascii")
            (build / "launcher.elf").write_bytes(b"")
            self.assertEqual(device.find_elf_for_build_id(worktree, "834b7bb03fd6e"), build / "launcher.elf")
            self.assertIsNone(device.find_elf_for_build_id(worktree, "834b7bb03fd7e"))

    def test_a_build_built_over_is_found_in_the_archive(self):
        self.archive.mkdir()
        (self.archive / "834b7bb03fd6-diag.elf").write_bytes(b"")
        with tempfile.TemporaryDirectory() as directory:
            found = device.find_elf_for_build_id(Path(directory), "834b7bb03fd6e")
        self.assertEqual(found, self.archive / "834b7bb03fd6-diag.elf")

    def keep_crash_of(self, build_id, capture):
        """keep_crashed_elf() over `capture` in a worktree whose one build is `build_id`."""
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "launcher" / "build.diag"
            build.mkdir(parents=True)
            (build / "build_id.txt").write_text(build_id + "\n", encoding="ascii")
            (build / "launcher.elf").write_bytes(b"elf")
            with mock.patch.object(device.Path, "cwd", return_value=Path(directory)):
                device.keep_crashed_elf(capture)

    def test_a_crash_keeps_the_build_its_panic_names(self):
        # The capture's BUILD_ID line may be another build's; the panic's own line wins.
        self.keep_crash_of("834b7bb03fd6-diag",
                           b"BUILD_ID=aaaaaaaaaaaa-diag\nassert failed\nELF file SHA256: 834b7bb03fd6e\n")
        self.assertEqual([path.name for path in self.archive.glob("*.elf")], ["834b7bb03fd6-diag.elf"])

    def test_a_failed_copy_is_one_line_not_the_captures_failure(self):
        with mock.patch.object(device.shutil, "copyfile", side_effect=OSError("disk full")), \
             contextlib.redirect_stderr(io.StringIO()) as errors:
            self.keep_crash_of("834b7bb03fd6-diag", b"ELF file SHA256: 834b7bb03fd6e\n")
        self.assertIn("disk full", errors.getvalue())

    def test_a_crash_with_no_elf_here_keeps_nothing(self):
        self.keep_crash_of("aaaaaaaaaaaa-diag", b"ELF file SHA256: 834b7bb03fd6e\n")
        self.assertFalse(self.archive.exists())

    def test_the_archive_keeps_the_newest_crashed_builds(self):
        for index in range(device.ELF_ARCHIVE_KEEP + 1):
            build_id = f"{index:012x}-dev"
            self.keep_crash_of(build_id, f"BUILD_ID={build_id}\n".encode())
            os.utime(self.archive / f"{build_id}.elf", (index, index))
        kept = sorted(path.name for path in self.archive.glob("*.elf"))
        self.assertEqual(len(kept), device.ELF_ARCHIVE_KEEP)
        self.assertNotIn(f"{0:012x}-dev.elf", kept)

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


class SerialLineOrderTests(unittest.TestCase):
    def test_posix_line_failure_closes_the_open_handle(self):
        class Serial:
            def __init__(self):
                self.is_open = False
                self.closed = False
            def open(self):
                self.is_open = True
            def __setattr__(self, name, value):
                if name == "rts" and self.is_open:
                    raise failure("port vanished")
                object.__setattr__(self, name, value)
            def close(self):
                self.closed = True
        original_init = Serial.__init__
        handles = []
        def initialized(handle):
            original_init(handle)
            handles.append(handle)
        Serial.__init__ = initialized
        for failure in (OSError, KeyboardInterrupt):
            with self.subTest(failure=failure), \
                 mock.patch.dict(sys.modules, {"serial": Namespace(Serial=Serial)}), \
                 mock.patch.object(device, "locked_port", return_value="fake"), \
                 mock.patch.object(device, "os", Namespace(name="posix")), \
                 mock.patch.object(device.board_serial, "os", Namespace(name="posix")):
                with self.assertRaises(failure):
                    device.open_serial()
                self.assertTrue(handles[-1].closed)

    def test_posix_port_without_modem_lines_opens_but_io_failure_closes(self):
        for line in ("rts", "dtr"):
            for code in (errno.ENOTTY, errno.EINVAL, errno.EIO):
                with self.subTest(line=line, code=code):
                    handles = []
                    class Serial:
                        def __init__(self):
                            self.is_open = False
                            self.closed = False
                            handles.append(self)
                        def open(self):
                            self.is_open = True
                        def __setattr__(self, name, value):
                            if name == line and self.is_open:
                                raise OSError(code, "modem line unavailable")
                            object.__setattr__(self, name, value)
                        def close(self):
                            self.closed = True
                    with mock.patch.dict(sys.modules, {"serial": Namespace(Serial=Serial)}), \
                         mock.patch.object(device, "locked_port", return_value="fake"), \
                         mock.patch.object(device, "os", Namespace(name="posix")), \
                         mock.patch.object(device.board_serial, "os", Namespace(name="posix")):
                        if code == errno.EIO:
                            with self.assertRaises(OSError) as caught:
                                device.open_serial()
                            self.assertEqual(caught.exception.errno, code)
                            self.assertTrue(handles[0].closed)
                        else:
                            connection = device.open_serial()
                            self.assertIs(connection, handles[0])
                            self.assertFalse(connection.closed)
                            connection.close()

    def test_posix_lowers_rts_before_dtr_and_windows_uses_one_dcb(self):
        class Serial:
            def __init__(self):
                self.dtr = self.rts = True
                self.is_open = False
                self.transitions = []

            def __setattr__(self, name, value):
                object.__setattr__(self, name, value)
                if name in ("dtr", "rts") and getattr(self, "is_open", False):
                    self.transitions.append((self.dtr, self.rts))

            def open(self):
                self.is_open = True
                if platform == "posix":
                    self.transitions.extend([(True, True), (self.dtr, True), (self.dtr, self.rts)])
                else:
                    self.transitions.append((self.dtr, self.rts))

        for platform in ("posix", "nt"):
            with self.subTest(platform=platform), \
                 mock.patch.dict(sys.modules, {"serial": Namespace(Serial=Serial)}), \
                 mock.patch.object(device, "locked_port", return_value="fake-port"), \
                 mock.patch.object(device, "os", Namespace(name=platform)), \
                 mock.patch.object(device.board_serial, "os", Namespace(name=platform)):
                connection = device.open_serial()
                self.assertEqual(connection.transitions[-1], (False, False))
                self.assertNotIn((False, True), connection.transitions)
                if platform == "posix":
                    self.assertEqual(connection.transitions[-2:], [(True, False), (False, False)])
                    self.assertTrue(connection.exclusive)
                else:
                    self.assertEqual(connection.transitions, [(False, False)])


class ResetReconnectTests(unittest.TestCase):
    """USB listing and opening are separate stages of the same reset."""

    platform = "nt"

    def setUp(self):
        self.clock = 0.0
        self.ticks = 0
        self.slept = []
        self.events = []
        self.usb = FakeUsbBoard(BOARD, ["COM5", "COM7"])
        self.other = []
        device.ACTIVE_LOCK.held = Namespace(board=BOARD)
        self.addCleanup(delattr, device.ACTIVE_LOCK, "held")
        original_open, original_reconnect = device.open_when_free, device.reconnect_after_reset

        def opened(*args, **kwargs):
            return original_open(*args, **dict(kwargs, sleep=self.sleep, now=self.now))

        def reconnect(*args, **kwargs):
            return original_reconnect(*args, **dict(kwargs, sleep=self.sleep, now=self.now))

        for patch in (mock.patch.dict(sys.modules, {"serial": self.usb.serial_module(self.now, self.sleep, self.platform)}),
                      mock.patch.object(device, "os", Namespace(name=self.platform)),
                      mock.patch.object(device.board_serial, "os", Namespace(name=self.platform)),
                      mock.patch.object(device, "plugged_boards", self.boards),
                      mock.patch.object(device, "require_live_lock"),
                      mock.patch.object(device, "require_unlost"),
                      mock.patch.object(device, "previous_holder_note", return_value=""),
                      mock.patch.object(device, "python_with_pyserial", return_value="python"),
                      mock.patch.object(device.time, "monotonic", self.now),
                      mock.patch.object(device, "open_when_free", side_effect=opened),
                      mock.patch.object(device, "reconnect_after_reset", side_effect=reconnect),
                      mock.patch.object(device.subprocess, "run", self.usb.esptool(self.now, self.sleep))):
            patch.start()
            self.addCleanup(patch.stop)

    def now(self):
        self.ticks += 1
        self.assertLess(self.ticks, 10000, "wait did not terminate on the fake clock")
        return self.clock

    def sleep(self, seconds):
        self.assertGreaterEqual(seconds, 0)
        self.slept.append(seconds)
        self.clock += seconds
        self.assertLess(self.clock, 2 * device.PORT_WAIT_SECONDS)
        for at, action in list(self.events):
            if self.clock >= at:
                self.events.remove((at, action))
                action()

    def boards(self):
        return self.other + ([device.Board(BOARD, self.usb.port)]
                             if self.usb.listed(self.now()) else [])

    def test_listed_but_unopenable_port_waits_for_open(self):
        probe = FakeUsbBoard(BOARD, ["COM5"])
        probe.reset(self.now())
        for at in (probe.busy_at, probe.returns):
            self.assertTrue(probe.listed(at))
            with self.assertRaises(FileNotFoundError):
                probe.open(probe.port, False, False, at)
        self.usb.reset(self.now())
        with device.reconnect_after_reset() as connection:
            self.assertGreaterEqual(self.clock, self.usb.opens)
            self.assertEqual(connection.boot, self.usb.boot)

    def test_a_reset_returns_the_live_handle_under_its_new_name(self):
        with device.reset() as connection:
            self.assertEqual((connection.port, connection.boot), ("COM7", self.usb.boot))
            self.assertGreaterEqual(self.clock, self.usb.open_at)
            connection.read(1)
        self.assertIsNone(connection.boot)
        self.assertEqual(self.usb.resets, 1)

    def test_a_reset_capture_reads_the_boot_without_a_second_open(self):
        self.usb.boot_line = b"SELFTEST_COMPLETE failures=0\n"
        original = device.open_serial
        opened_boots = []

        def once_per_boot():
            connection = original()
            if self.usb.resets:
                self.assertGreaterEqual(self.clock, self.usb.open_at)
            self.assertNotIn(connection.boot, opened_boots)
            opened_boots.append(connection.boot)
            return connection

        with mock.patch.object(device, "open_serial", side_effect=once_per_boot):
            data, reason = device.reset_and_capture(os.devnull, 5, None)
        self.assertEqual((data, reason, self.usb.resets), (self.usb.boot_line, "complete", 1))

    def test_another_board_staying_listed_does_not_hide_the_reset(self):
        self.other = [device.Board("other-board", "COM9")]
        with device.reset() as connection:
            self.assertEqual(connection.port, "COM7")
            self.assertGreaterEqual(self.clock, self.usb.open_at)
            self.assertLess(self.clock, 0.2 + device.RESET_LEAVE_SECONDS)

    def test_lock_lost_during_wait_stops(self):
        self.usb.reset(self.now())

        def live():
            if self.clock >= self.usb.leaves:
                raise device.LockLost("lost during reset")

        with mock.patch.object(device, "require_live_lock", side_effect=live):
            with self.assertRaises(device.LockLost):
                device.reconnect_after_reset()
        self.assertLess(self.clock, self.usb.returns)

    def test_silent_rts_reset_gets_one_watchdog_and_captures_new_boot(self):
        run = self.usb.esptool(self.now, self.sleep)
        afters = []

        def resetting(command, **kwargs):
            after = command[command.index("--after") + 1]
            afters.append(after)
            self.usb.boot_line = (b"SELFTEST_COMPLETE failures=0\n"
                                  if after == "watchdog_reset" else b"")
            return run(command, **kwargs)

        with mock.patch.object(device.subprocess, "run", side_effect=resetting):
            data, reason = device.reset_and_capture(os.devnull, 10, None)
        self.assertEqual(afters, ["hard_reset", "watchdog_reset"])
        self.assertEqual((data, reason), (self.usb.boot_line, "complete"))

    def test_reconnected_handle_quiet_past_first_byte_cutoff_is_read_on(self):
        self.usb.boot_line = b"boot before loss\n"
        connection = device.reset()
        self.events.append((self.clock + 0.4, lambda: self.usb.reset(self.now())))
        original = device.reconnect_after_reset

        def reconnect(*args, **kwargs):
            handle = original(*args, **kwargs)
            handle.unread = b""
            self.events.append((self.clock + device.RESET_FIRST_BYTE_SECONDS + 0.4,
                                lambda: setattr(handle, "unread", b"SELFTEST_COMPLETE failures=0\n")))
            return handle

        with mock.patch.object(device, "reconnect_after_reset", side_effect=reconnect):
            data, reason = device.capture_after_reset(os.devnull, 10, None, connection=connection)
        self.assertEqual(reason, "complete")
        self.assertIn(b"boot before loss", data)
        self.assertIn(b"SELFTEST_COMPLETE", data)

    def test_failed_mid_capture_reconnect_keeps_bytes(self):
        self.usb.boot_line = b"boot before loss\n"
        connection = device.reset()
        self.usb.returns = device.PORT_WAIT_SECONDS
        self.events.append((self.clock + 0.4, lambda: self.usb.reset(self.now())))
        started = self.clock
        data, reason = device.capture_after_reset(os.devnull, 3, None, connection=connection)
        self.assertLessEqual(self.clock - started, 3 + device.USB_POLL_SECONDS)
        self.assertEqual((data, reason), (self.usb.boot_line, "port lost"))
        self.assertIsNone(connection.boot)

    def test_never_returns_uses_the_default_reset_bound(self):
        self.usb.returns = device.PORT_WAIT_SECONDS
        with self.assertRaises(device.PortUnavailable):
            device.reset()
        self.assertAlmostEqual(self.clock, 0.2 + device.PORT_REAPPEAR_SECONDS)

    def test_never_leaves_waits_only_the_default_leave_bound(self):
        self.usb.leaves = self.usb.returns = self.usb.opens = self.usb.busy_at = device.PORT_WAIT_SECONDS
        with device.reset():
            pass
        self.assertAlmostEqual(self.clock, 0.2 + device.RESET_LEAVE_SECONDS)

    def test_leave_wait_cannot_exceed_remaining_capture_time(self):
        with device.reconnect_after_reset(0):
            pass
        self.assertEqual(self.clock, 0)

    def test_open_gets_only_time_left_in_reconnect_bound(self):
        self.usb.reset(self.now())
        self.usb.opens = self.usb.open_at = device.PORT_WAIT_SECONDS
        with self.assertRaises(device.PortUnavailable):
            device.reconnect_after_reset(device.RESET_LEAVE_SECONDS)
        self.assertAlmostEqual(self.clock, device.RESET_LEAVE_SECONDS)

    def test_resetting_thunk_returns_port_or_preserves_failure(self):
        failures = (None, OSError("open failed"), RuntimeError("write failed"),
                    subprocess.CalledProcessError(7, ["resetter"]),
                    device.LockLost("lock was lost during the flash"))
        for failure in failures:
            for unavailable in (False, True):
                with self.subTest(failure=failure, unavailable=unavailable):
                    connection = mock.Mock()
                    run = mock.Mock(side_effect=failure)
                    with mock.patch.object(device, "reconnect_after_reset",
                            side_effect=device.PortUnavailable("missing") if unavailable else None,
                            return_value=connection) as reconnect:
                        if failure is None and not unavailable:
                            self.assertIs(device.run_resetting(run), connection)
                        else:
                            expected = type(failure) if failure else device.PortUnavailable
                            with self.assertRaises(expected) as caught:
                                device.run_resetting(run)
                            if failure:
                                self.assertIs(caught.exception, failure)
                        run.assert_called_once_with()
                        if isinstance(failure, device.LockLost):
                            reconnect.assert_not_called()
                        else:
                            reconnect.assert_called_once_with()
                        self.assertEqual(connection.close.call_count,
                                         int(failure is not None and not unavailable
                                             and not isinstance(failure, device.LockLost)))

    def test_poll_sleeps_clip_leave_and_return_bounds(self):
        bound = device.USB_POLL_SECONDS * 1.5
        with device.reconnect_after_reset(bound):
            pass
        self.assertAlmostEqual(self.clock, bound)
        self.clock = 0
        self.usb.reset(self.now())
        self.usb.leaves = 0
        self.usb.returns = device.PORT_WAIT_SECONDS
        with self.assertRaises(device.PortUnavailable):
            device.reconnect_after_reset(bound)
        self.assertAlmostEqual(self.clock, bound)

    def test_failed_flash_reconnects_before_releasing_lock(self):
        held = Namespace(held={"acquired_at": None}, kind="batch")
        failure = RuntimeError("write failed")
        def write(*unused):
            self.usb.reset(self.now())
            raise failure
        built = mock.MagicMock(build_id="abc", write=["flash"], run=mock.Mock(side_effect=write))
        locked = []
        @contextlib.contextmanager
        def lock():
            locked.append(True)
            try:
                yield held
            finally:
                locked.pop()
        original = device.reconnect_after_reset
        def reconnect():
            self.assertEqual(locked, [True])
            self.assertFalse(built.record.called)
            return original()
        with mock.patch.object(device, "holding", return_value=lock()), \
             mock.patch.object(device, "reconnect_after_reset", side_effect=reconnect):
            with self.assertRaises(RuntimeError) as caught:
                device.write_image(built, None, BOARD, held)
        self.assertIs(caught.exception, failure)
        self.assertEqual(self.usb.boot, 1)
        self.assertGreaterEqual(self.clock, self.usb.open_at)

    def test_flash_lock_lost_message_survives_without_reconnect(self):
        held = Namespace(held={"acquired_at": None}, kind="batch")
        failure = device.LockLost("device lock was lost during the flash; the flash was stopped")
        built = mock.MagicMock(run=mock.Mock(side_effect=failure))
        with mock.patch.object(device, "holding", return_value=contextlib.nullcontext(held)), \
             mock.patch.object(device, "reconnect_after_reset", side_effect=device.LockLost) as reconnect:
            with self.assertRaises(device.LockLost) as caught:
                device.write_image(built, None, BOARD, held)
        self.assertIs(caught.exception, failure)
        self.assertEqual(str(caught.exception), str(failure))
        reconnect.assert_not_called()

    def test_flash_handle_closes_when_lock_exit_or_record_raises(self):
        held = Namespace(held={"acquired_at": None}, kind="batch")
        for stage in ("lock", "record", "snapshot"):
            with self.subTest(stage=stage):
                connection = mock.Mock()
                failure = RuntimeError("cleanup failed")
                built = mock.MagicMock(build_id="abc")
                if stage == "record":
                    built.record.side_effect = failure
                if stage == "snapshot":
                    built.__exit__.side_effect = failure
                @contextlib.contextmanager
                def lock():
                    yield held
                    if stage == "lock":
                        raise failure
                with mock.patch.object(device, "holding", return_value=lock()), \
                     mock.patch.object(device, "reconnect_after_reset", return_value=connection):
                    with self.assertRaises(RuntimeError) as caught:
                        device.write_image(built, None, BOARD, held)
                self.assertIs(caught.exception, failure)
                connection.close.assert_called_once_with()

    def test_boot_handle_closes_when_capture_path_resolution_raises(self):
        connection = mock.Mock()
        args = Namespace(owner="agent", variant="diag")
        failure = OSError("capture path unavailable")
        with mock.patch.object(device, "resolve_capture_path", side_effect=failure):
            with self.assertRaises(OSError) as caught:
                device.await_console(args, BOARD, "abc", None, connection)
        self.assertIs(caught.exception, failure)
        connection.close.assert_called_once_with()

    def test_flash_boot_of_a_chip_left_in_download_mode_gets_the_watchdog(self):
        run = self.usb.esptool(self.now, self.sleep)
        afters = []

        def resetting(command, **kwargs):
            after = command[command.index("--after") + 1]
            afters.append(after)
            self.usb.boot_line = (b"BUILD_ID=abc\n" + device.CONSOLE_UP + b"\n"
                                  if after == "watchdog_reset" else b"")
            return run(command, **kwargs)

        args = Namespace(owner="agent", purpose="boot", variant="diag", worktree=".")
        with mock.patch.object(device.subprocess, "run", side_effect=resetting), \
             mock.patch.object(device, "resolve_capture_path", return_value=(os.devnull, False)), \
             mock.patch.object(device, "record_capture", return_value="boot.log"), \
             mock.patch.object(device, "git_commit", return_value=None), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            device.await_console(args, BOARD, "abc", None, device.reset())
        self.assertEqual(afters, ["hard_reset", "watchdog_reset"])
        self.assertIn("booted BUILD_ID=abc", output.getvalue())

    def test_flash_boot_uses_returned_handle_without_opening_again(self):
        held = Namespace(held={"acquired_at": None}, kind="batch")
        def write(*unused):
            self.usb.reset(self.now())
        built = mock.MagicMock(build_id="abc", write=["flash"], run=mock.Mock(side_effect=write))
        self.usb.boot_line = b"BUILD_ID=abc\n" + device.CONSOLE_UP + b"\n"
        with mock.patch.object(device, "holding", return_value=contextlib.nullcontext(held)):
            build_id, connection = device.write_image(built, None, BOARD, held)
        self.assertEqual(build_id, "abc")
        args = Namespace(owner="agent", purpose="boot", variant="diag", worktree=".")
        with mock.patch.object(device, "open_when_free", side_effect=AssertionError("second open")), \
             mock.patch.object(device, "resolve_capture_path", return_value=(os.devnull, False)), \
             mock.patch.object(device, "record_capture", return_value="boot.log"), \
             mock.patch.object(device, "git_commit", return_value=None), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            device.await_console(args, BOARD, build_id, None, connection)
        self.assertIn("booted BUILD_ID=abc", output.getvalue())
        self.assertIsNone(connection.boot)


class PosixResetReconnectTests(ResetReconnectTests):
    platform = "posix"


class CaptureAfterResetTests(unittest.TestCase):
    """The first handle after a reset is the new boot's: one that hears
    nothing at all is a board the reset left in download mode."""

    def setUp(self):
        clock = (tick * device.USB_POLL_SECONDS for tick in itertools.count())
        patch = mock.patch.object(device.time, "monotonic", side_effect=lambda: next(clock))
        patch.start()
        self.addCleanup(patch.stop)

    def test_a_board_silent_from_the_reset_ends_as_silent_at_once(self):
        started = time.monotonic()
        with mock.patch.object(device, "open_when_free", return_value=FakeConnection([])), \
             mock.patch.object(device, "RESET_FIRST_BYTE_SECONDS", 0.05):
            data, reason = device.capture_after_reset(os.devnull, 30, 300, connection=FakeConnection([]))
        self.assertEqual((data, reason), (b"", "silent"))
        self.assertLess(time.monotonic() - started, 5)

    def test_output_that_goes_idle_is_not_reopened(self):
        output = FakeConnection([b"I (640) boot: some output\n"])
        with mock.patch.object(device, "open_when_free", return_value=output) as opened, \
             mock.patch.object(device, "RESET_FIRST_BYTE_SECONDS", 0.05):
            unused_data, reason = device.capture_after_reset(os.devnull, 30, 0.2, connection=output)
        self.assertEqual((reason, opened.call_count), ("idle", 0))


class ListenElfResolutionTests(unittest.TestCase):
    """listen() decodes against --elf when given, and otherwise against
    whatever find_elf_for_build_id() resolves from the capture itself."""

    def run_listen(self, elf_arg, data_lines, matched_elf):
        connection = FakeConnection([b"\n".join(data_lines) + b"\n"])
        args = Namespace(owner="agent", purpose="autana monitor", wait=0,
                         seconds=1, out=None, elf=elf_arg)
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch.object(device, "find_elf_for_build_id", return_value=matched_elf) as finder, \
                 mock.patch.object(device, "decode_crash_addresses", return_value=[]) as decode:
                device.listen(args, store, BOARD)
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
        store = mock_store()
        printed = io.StringIO()
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "open_when_free", return_value=connection), \
             mock.patch.object(device, "records_root", return_value=Path(directory)), \
             mock.patch.object(device, "git_commit", return_value="deadbeef"), \
             mock.patch.object(device, "find_elf_for_build_id", return_value=None), \
             contextlib.redirect_stdout(printed):
            device.listen(args, store, BOARD)
        self.assertIn("listen capture: ", printed.getvalue())
        self.assertIn("error: failed", printed.getvalue())
        self.assertNotIn("ordinary line", printed.getvalue())


class ListenLifecycleTests(unittest.TestCase):
    @contextlib.contextmanager
    def listen_environment(self, store, root, output):
        with mock.patch.object(device.device_lock, "LockStore", return_value=store), \
             mock.patch.object(device, "records_root", return_value=root), \
             mock.patch.object(device, "git_commit", return_value="deadbeef"), \
             mock.patch.object(device, "find_elf_for_build_id", return_value=None), \
             contextlib.redirect_stdout(output):
            yield

    def run_listen(self, connection, flags, output=None):
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = output or io.StringIO()
            with self.listen_environment(store, root, output), \
                 mock.patch.object(device, "open_when_free", return_value=connection):
                code = device.main(["--board", BOARD, "listen", *flags])
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

    def test_a_board_that_reboots_while_listened_to_fails_the_listen(self):
        connection = FakeConnection([b"BUILD_ID=abc123\nrst:0xc (RTC_SW_CPU_RST),boot:0x8\n"])
        code, output, _, _, _ = self.run_listen(connection, ["--seconds", "0.1"])
        self.assertEqual(code, 1)
        self.assertIn("rebooted: 1 boots, expected at most 0", output)

    def test_listen_requires_exactly_one_duration_mode(self):
        for flags in ([], ["--seconds", "1", "--follow"]):
            with self.subTest(flags=flags), self.assertRaises(SystemExit) as caught:
                device.main(["--board", BOARD, "listen", *flags])
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
        connection = InterruptedConnection([b"SELFTEST_COMPLETE failures=0 elapsed_ms=42\n", b"after tests\n"])
        code, _, entry, payload, _ = self.run_listen(connection, ["--seconds", "1"])
        self.assertEqual(code, 0)
        self.assertEqual(entry["reason"], "stopped")
        self.assertEqual(payload, b"SELFTEST_COMPLETE failures=0 elapsed_ms=42\nafter tests\n")

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
             self.listen_environment(store, Path(directory), io.StringIO()):
            code = device.main(["--board", BOARD, "listen", "--follow"])
            entry = json.loads((Path(directory) / "index.jsonl").read_text().strip())
        self.assertEqual(code, 0)
        self.assertEqual(entry["reason"], "stopped")

    def test_interrupt_during_port_open_releases_lock(self):
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory, \
             self.listen_environment(store, Path(directory), io.StringIO()), \
             mock.patch.object(device, "open_when_free", side_effect=KeyboardInterrupt):
            code = device.main(["--board", BOARD, "listen", "--follow"])
            entry = json.loads((Path(directory) / "index.jsonl").read_text().strip())
        self.assertEqual(code, 0)
        self.assertEqual(entry["reason"], "stopped")
        store.release.assert_called_once()


def raw_core_dump(build_hash):
    """Coredump-partition bytes as the panic handler leaves them: a header,
    then ELF notes, one of them the app's build. The other bytes are filler."""
    note = device.COREDUMP_INFO_NOTE + b"\0" * (-len(device.COREDUMP_INFO_NOTE) % device.ELF_NOTE_ALIGN)
    sha = build_hash.encode().ljust(device.COREDUMP_INFO_SHA_BYTES, b"\0")
    return b"\x24\x42" * 10 + note + struct.pack("<I", 3) + sha + b"\xa5" * 32


class CoredumpBuildTests(unittest.TestCase):
    """coredump_build() names the build the dump itself came from."""

    def test_reads_the_hash_the_dump_carries(self):
        self.assertEqual(device.coredump_build(raw_core_dump("834b7bb03fd6e")), "834b7bb03fd6e")

    def test_an_erased_partition_holds_no_dump(self):
        self.assertIsNone(device.coredump_build(b"\xff" * 4096))

    def test_a_note_without_hex_is_no_build(self):
        self.assertIsNone(device.coredump_build(raw_core_dump("not-hex")))


class CoredumpTests(unittest.TestCase):
    """Read the dump raw under the board lock, then decode it against the
    build it came from, which is not the running one after a later flash."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def run_coredump(self, raw=b"", erase=False, elf=None, found_elf=Path("archive/834b7bb03fd6-diag.elf"),
                     read_fails=False):
        args = Namespace(owner="agent", purpose="autana coredump", wait=0, erase=erase, elf=elf)
        store = mock_store()
        core = self.root / "120000_coredump_agent.log"
        commands = []

        def run(command, check=False):
            commands.append(command)
            if read_fails and "read_partition" in command:
                raise subprocess.CalledProcessError(7, command)
            if "read_partition" in command:
                Path(command[command.index("--output") + 1]).write_bytes(raw)
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(device, "resolve_capture_path", return_value=(core, True)),              mock.patch.object(device, "find_elf_for_build_id", return_value=found_elf) as finder,              mock.patch.object(device, "locked_port", return_value="COM9"),              mock.patch.object(device, "toolchain_tool", return_value=None),              mock.patch.object(device, "idf_path", return_value="idf"),              mock.patch.object(device.subprocess, "run", side_effect=run),              mock.patch.object(device, "reset") as reset,              mock.patch("builtins.print"):
            code = device.coredump(args, store, BOARD)
        store.release.assert_called_once()
        # One reset after parttool, read or erase, failed or not; none inside it.
        self.assertEqual(reset.call_count, 1)
        self.assertEqual(commands[0][commands[0].index("--esptool-args") + 1], device.PARTTOOL_NO_RESET)
        return code, finder, commands, core.with_suffix(".bin")

    def test_decodes_against_the_build_the_dump_names_not_the_running_one(self):
        code, finder, commands, saved = self.run_coredump(raw_core_dump("834b7bb03fd6e"))
        self.assertEqual(code, 0)
        self.assertEqual(finder.call_args[0][1], "834b7bb03fd6e")
        read, decode = commands
        self.assertEqual(read[read.index("--port") + 1], "COM9")
        self.assertNotIn("--port", decode)
        self.assertEqual(decode[decode.index("--core") + 1], str(saved))
        self.assertEqual(decode[-1], str(Path("archive/834b7bb03fd6-diag.elf")))
        self.assertTrue(saved.is_file())

    def test_no_elf_for_the_dumps_build_keeps_the_dump_and_fails(self):
        code, _, commands, saved = self.run_coredump(raw_core_dump("834b7bb03fd6e"), found_elf=None)
        self.assertEqual(code, 1)
        self.assertEqual(len(commands), 1)
        self.assertTrue(saved.is_file())

    def test_an_empty_partition_reads_as_no_dump(self):
        code, finder, commands, saved = self.run_coredump(b"\xff" * 64)
        self.assertEqual(code, 0)
        finder.assert_not_called()
        self.assertEqual(len(commands), 1)
        self.assertFalse(saved.exists())

    def test_a_dump_that_names_no_build_is_kept(self):
        code, finder, commands, saved = self.run_coredump(b"\x10\x20\x00\x00" + b"\xa5" * 60, found_elf=None)
        self.assertEqual(code, 1)
        self.assertEqual(len(commands), 1)
        self.assertTrue(saved.is_file())

    def test_an_elf_given_is_the_one_decoded(self):
        code, finder, commands, _ = self.run_coredump(raw_core_dump("834b7bb03fd6e"), elf="other.elf")
        self.assertEqual(code, 0)
        finder.assert_not_called()
        self.assertEqual(commands[-1][-1], "other.elf")

    def test_a_failed_read_keeps_its_exit_status(self):
        code, _, commands, _ = self.run_coredump(read_fails=True)
        self.assertEqual(code, 7)
        self.assertEqual(len(commands), 1)

    def test_erase_clears_only_the_coredump_partition(self):
        code, finder, commands, _ = self.run_coredump(erase=True)
        self.assertEqual(code, 0)
        finder.assert_not_called()
        self.assertIn("erase_partition", commands[0])
        self.assertIn("--partition-name=" + device.COREDUMP_PARTITION, commands[0])

    def test_a_saved_dump_decodes_with_no_board(self):
        saved = self.root / "dump.bin"
        saved.write_bytes(raw_core_dump("834b7bb03fd6e"))
        with mock.patch.object(device.subprocess, "run",
                               return_value=subprocess.CompletedProcess([], 0)) as run,              mock.patch.object(device, "toolchain_tool", return_value=None),              mock.patch.object(device, "board_for_lock") as board,              contextlib.redirect_stdout(io.StringIO()):
            code = device.main(["coredump", "--core", str(saved), "--elf", "x.elf"])
        self.assertEqual(code, 0)
        board.assert_not_called()
        self.assertEqual(run.call_args[0][0][-1], "x.elf")


class WaiterNoticeTests(unittest.TestCase):
    def test_holder_reports_each_waiter_once(self):
        store = mock_store()
        store.tickets.return_value = [{"ticket": "one", "owner": "sam", "purpose": "test"}]
        lock = device.HeldLock(store, BOARD, "agent", "monitor", 0, announce_waiters=True)
        lock.stop.wait = mock.Mock(side_effect=[False, False, True])
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            lock.keep_alive()
        self.assertEqual(stderr.getvalue().count("sam is waiting"), 1)

    def test_a_flash_or_suite_holder_never_invites_ctrl_c(self):
        store = mock_store()
        store.tickets.return_value = [{"ticket": "one", "owner": "sam", "purpose": "test"}]
        lock = device.HeldLock(store, BOARD, "agent", "flash", 0)
        lock.stop.wait = mock.Mock(side_effect=[False, False, True])
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            lock.keep_alive()
        self.assertEqual(stderr.getvalue(), "")


class HeartbeatRetryTests(unittest.TestCase):
    """A heartbeat that raises (WSL meeting a guard Windows is still deleting)
    is retried, not the end of the thread that keeps the lock alive."""

    def test_a_raising_heartbeat_is_retried(self):
        store = mock_store()
        store.heartbeat.side_effect = [FileNotFoundError(2, "No such file or directory"), True]
        lock = device.HeldLock(store, BOARD, "agent", "flash", 0)
        lock.stop.wait = mock.Mock(side_effect=[False, False, True])
        with mock.patch.object(device.HeldLock, "HEARTBEAT_SECONDS", 0), \
             contextlib.redirect_stderr(io.StringIO()) as stderr:
            lock.keep_alive()
        self.assertEqual(store.heartbeat.call_count, 2)
        self.assertFalse(lock.lost.is_set())
        self.assertIn("retrying", stderr.getvalue())

    def test_a_retried_heartbeat_that_is_refused_loses_the_lock(self):
        store = mock_store()
        store.heartbeat.side_effect = [FileNotFoundError(2, "No such file or directory"), False]
        lock = device.HeldLock(store, BOARD, "agent", "flash", 0)
        lock.stop.wait = mock.Mock(side_effect=[False, False, True])
        with mock.patch.object(device.HeldLock, "HEARTBEAT_SECONDS", 0), \
             contextlib.redirect_stderr(io.StringIO()):
            lock.keep_alive()
        self.assertTrue(lock.lost.is_set())

    def test_a_heartbeat_failing_past_the_stale_window_loses_the_lock(self):
        store = mock_store()
        store.heartbeat.side_effect = FileNotFoundError(2, "No such file or directory")
        lock = device.HeldLock(store, BOARD, "agent", "flash", 0)
        lock.renewed_at = time.monotonic() - device_lock.DEFAULT_STALE_SECONDS
        lock.stop.wait = mock.Mock(side_effect=[False, True])
        with mock.patch.object(device.HeldLock, "HEARTBEAT_SECONDS", 0), \
             contextlib.redirect_stderr(io.StringIO()):
            lock.keep_alive()
        self.assertEqual(store.heartbeat.call_count, 1)
        self.assertTrue(lock.lost.is_set())

    def test_a_raising_borrowed_check_is_retried(self):
        store = mock_store()
        store.check_token.side_effect = [FileNotFoundError(2, "No such file or directory"), True]
        lock = device.HeldLock(store, BOARD, "agent", "flash", 0)
        lock.borrowed = True
        lock.stop.wait = mock.Mock(side_effect=[False, False, True])
        with contextlib.redirect_stderr(io.StringIO()):
            lock.keep_alive()
        self.assertEqual(store.check_token.call_count, 2)
        self.assertFalse(lock.lost.is_set())


class ResetCommandTests(unittest.TestCase):
    def test_parser_forwards_capture_options(self):
        store = mock.Mock()
        with mock.patch.object(device.device_lock, "LockStore", return_value=store), \
             mock.patch.object(device, "reset_device", return_value=0) as reset_device:
            code = device.main(["--board", BOARD, "reset", "--capture", "--seconds", "15"])
        self.assertEqual(code, 0)
        args = reset_device.call_args[0][0]
        self.assertTrue(args.capture)
        self.assertEqual(args.seconds, 15.0)

    def test_capture_reconnects_after_a_lost_handle_and_records(self):
        class VanishingConnection(FakeConnection):
            def read(self, unused_size):
                raise OSError("USB device disappeared")

        store = mock_store()
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=1.0, out=None)
        first = VanishingConnection([])
        second = FakeConnection([b"cpu_start: Multicore app\nSELFTEST_COMPLETE\n"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "reset", return_value=first) as reset, \
                 mock.patch.object(device, "reconnect_after_reset", return_value=second) as reconnect, \
                 mock.patch.object(device, "open_when_free") as open_when_free, \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"):
                code = device.reset_device(args, store, BOARD)
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        self.assertEqual(code, 0)
        reset.assert_called_once_with()
        store.release.assert_called_once_with(BOARD, "token")
        reconnect.assert_called_once()
        open_when_free.assert_not_called()
        self.assertEqual(entry["command"], "reset")
        self.assertEqual(entry["reason"], "complete")

    def test_capture_keeps_data_when_a_later_reconnect_runs_out_of_time(self):
        class VanishingConnection(FakeConnection):
            def read(self, unused_size):
                if self.chunks:
                    return super().read(unused_size)
                raise OSError("USB device disappeared")

        store = mock_store()
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=20.0, out=None)
        connection = VanishingConnection([b"boot output\n"])
        timeout = device.PortUnavailable("board did not come back after reset before capture ended")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "reset", return_value=connection), \
                 mock.patch.object(device, "reconnect_after_reset", side_effect=timeout), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch("builtins.print") as printed:
                code = device.reset_device(args, store, BOARD)
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
            capture = Path(entry["capture_path"]).read_bytes()
        self.assertEqual(code, 0)
        self.assertEqual(capture, b"boot output\n")
        self.assertEqual(entry["reason"], "port lost")
        self.assertIsNone(entry["error"])
        self.assertNotIn(mock.call("boot output\n", end=""), printed.call_args_list)
        printed.assert_any_call("reset capture: " + entry["capture_path"])

    def test_reset_releases_the_lock_when_esptool_fails(self):
        store = mock_store()
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=1.0, out=None)
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(device, "reset",
                               side_effect=subprocess.CalledProcessError(1, "esptool")), \
             mock.patch.object(device, "open_when_free", return_value=FakeConnection([])), \
             mock.patch.object(device, "records_root", return_value=Path(directory)):
            with self.assertRaises(subprocess.CalledProcessError):
                device.reset_device(args, store, BOARD)
        store.release.assert_called_once_with(BOARD, "token")

    def test_reset_records_an_os_error(self):
        store = mock_store()
        args = Namespace(owner="agent", purpose="autana reset", wait=0, capture=True,
                         seconds=1.0, out=None)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "records"
            with mock.patch.object(device, "reset", side_effect=OSError("port vanished")), \
                 mock.patch.object(device, "open_when_free", return_value=FakeConnection([])), \
                 mock.patch.object(device, "records_root", return_value=root):
                with self.assertRaisesRegex(OSError, "port vanished"):
                    device.reset_device(args, store, BOARD)
            entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        self.assertEqual(entry["error"], "port vanished")
        self.assertIsNone(entry["reason"])


class SelftestTests(unittest.TestCase):
    """selftest() flashes diag+autorun under one held lock, then resets,
    waits for USB serial, and captures until SELFTEST_COMPLETE."""

    def run_selftest(self, perf_scope=False, capture=None):
        calls = {"flash_extra_flags": None, "held_lock": None, "events": []}

        def fake_build_image(args, port, extra_flags=()):
            calls["flash_extra_flags"] = list(extra_flags)
            calls["build_saw_lock"] = getattr(device.ACTIVE_LOCK, "held", None)
            return contextlib.nullcontext("built")

        def fake_write_image(built, store, port, held_lock):
            calls["held_lock"] = held_lock
            calls["built"] = built
            calls["flash_connection"] = mock.Mock()
            return "abc123-diag", calls["flash_connection"]

        connection = FakeConnection([b"free heap after framebuffer: 123456 bytes\n",
                                     b"SELFTEST_COMPLETE failures=0 elapsed_ms=42\n"])

        def fake_reset():
            calls["events"].append("reset")
            calls["capture_lock"] = device.ACTIVE_LOCK.held
            return connection

        def fake_wait(*unused, **unused_keywords):
            if calls["events"]:
                calls["events"].append("reopen")
            return connection

        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "wt"
            (worktree / "launcher" / "tools" / "build").mkdir(parents=True)
            root = Path(directory) / "records"
            args = Namespace(owner="agent", purpose="autana selftest", wait=0,
                             worktree=str(worktree), out=None, perf_scope=perf_scope,
                             max_seconds=5, idle_seconds=None)
            store = mock_store()
            with mock.patch.object(device, "build_image", fake_build_image), \
                 mock.patch.object(device, "write_image", fake_write_image), \
                 mock.patch.object(device, "reset", fake_reset), \
                 mock.patch.object(device, "open_when_free", side_effect=fake_wait), \
                 mock.patch.object(device, "records_root", return_value=root), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 mock.patch.object(device, "reset_and_capture", wraps=device.reset_and_capture) as captured:
                if isinstance(capture, Exception):
                    captured.side_effect = capture
                elif capture is not None:
                    captured.side_effect = lambda path, *unused: (Path(path).write_bytes(capture[0]), capture)[1]
                try:
                    code = device.selftest(args, store, BOARD)
                except RuntimeError as caught:
                    code = caught
                calls["reports"] = "\n".join(path.read_text(encoding="utf-8")
                                             for path in root.rglob("*.md"))
                entry = json.loads((root / "index.jsonl").read_text(encoding="utf-8").strip())
        return code, calls, entry

    def test_capture_error_without_log_skips_report(self):
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            code, calls, entry = self.run_selftest(capture=RuntimeError("USB disconnected"))
        self.assertIsInstance(code, RuntimeError)
        self.assertIn("report skipped: capture log was not created", errors.getvalue())
        self.assertNotIn("No such file", errors.getvalue())
        self.assertEqual(calls["reports"], "")
        self.assertEqual(entry["error"], "USB disconnected")

    def test_incomplete_selftest_fails_and_keeps_the_report(self):
        for reason in ("idle", "timeout"):
            with self.subTest(reason=reason), contextlib.redirect_stdout(io.StringIO()) as output:
                code, calls, entry = self.run_selftest(capture=(b"", reason))
                self.assertIsInstance(code, RuntimeError)
                self.assertEqual(str(code), f"no completion line arrived: the capture ended {reason}")
                self.assertIn("selftest results: 0 PASS, 0 FAIL", output.getvalue())
                self.assertEqual(entry["reason"], reason)
                self.assertIn("# Device Capture Report", calls["reports"])
                self.assertIn(reason, calls["reports"])

    def test_successful_selftest_closes_flash_handle(self):
        code, calls, _ = self.run_selftest()
        self.assertEqual(code, 0)
        calls["flash_connection"].close.assert_called_once_with()

    def test_flashes_diag_with_autorun_under_the_batch_lock(self):
        code, calls, entry = self.run_selftest()
        self.assertEqual(code, 0)
        self.assertEqual(calls["flash_extra_flags"], ["--autorun"])
        self.assertIsNotNone(calls["held_lock"])

    def test_builds_before_the_lock_it_flashes_and_captures_under(self):
        _, calls, _ = self.run_selftest()
        self.assertIsNone(calls["build_saw_lock"])
        self.assertEqual(calls["built"], "built")
        self.assertIs(calls["capture_lock"], calls["held_lock"])

    def test_perf_scope_is_passed_to_the_build(self):
        _, calls, _ = self.run_selftest(perf_scope=True)
        self.assertEqual(sorted(calls["flash_extra_flags"]), ["--autorun", "--perf-scope"])

    def test_captures_from_the_handle_reset_returns(self):
        _, calls, _ = self.run_selftest()
        self.assertEqual(calls["events"], ["reset"])

    def test_records_the_selftest_command_and_build_id(self):
        _, _, entry = self.run_selftest()
        self.assertEqual(entry["command"], "selftest")
        self.assertEqual(entry["build_id"], "abc123-diag")
        self.assertIsNone(entry["error"])

    def test_the_capture_ends_on_selftest_complete_not_its_deadline(self):
        _, _, entry = self.run_selftest()
        self.assertEqual(entry["reason"], "complete")

    def test_sentinel_failures_without_fail_lines_fail_selftest(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            code, _, entry = self.run_selftest(capture=(
                b":1:test_one:PASS\nSELFTEST_COMPLETE failures=2 elapsed_ms=10\n", "complete"))
        self.assertEqual(code, 1)
        self.assertEqual(entry["reason"], "complete")
        self.assertIsNone(entry["error"])
        self.assertIn("SELFTEST_COMPLETE failures=2, but 0 FAIL lines", output.getvalue())

    def test_a_failing_run_is_reported_but_still_records_cleanly(self):
        with tempfile.TemporaryDirectory() as directory:
            worktree = Path(directory) / "wt"
            (worktree / "launcher" / "tools" / "build").mkdir(parents=True)
            root = Path(directory) / "records"
            connection = FakeConnection([
                b":1:test_one:FAIL: boom\nSELFTEST_COMPLETE failures=1 elapsed_ms=10\n",
            ])
            args = Namespace(owner="agent", purpose="autana selftest", wait=0,
                             worktree=str(worktree), out=None, perf_scope=False,
                             max_seconds=5, idle_seconds=None)
            store = mock_store()
            with recorded_serial(connection, root), \
                 mock.patch.object(device, "build_image"), \
                 mock.patch.object(device, "write_image", return_value=("abc123-diag", FakeConnection())), \
                 mock.patch.object(device, "reset", return_value=connection):
                code = device.selftest(args, store, BOARD)
        self.assertEqual(code, 1)


class TouchPointTests(unittest.TestCase):
    """The default screenshot's pixels in the panel's own frame, which
    TAP, PRESS and DRAG take."""

    @staticmethod
    def coordinate_png(width, height):
        """Each pixel's red and green are its own x and y."""
        import screenshot as screenshot_tool
        return screenshot_tool.encode_png(width, height, (b"".join(bytes((x, y, 0)) for x in range(width))
                                                          for y in range(height)))

    @staticmethod
    def pixels(png):
        width, height = struct.unpack_from(">II", png, 16)
        pos, compressed = 8, bytearray()
        while pos < len(png):
            length, = struct.unpack_from(">I", png, pos)
            if png[pos + 4:pos + 8] == b"IDAT":
                compressed += png[pos + 8:pos + 8 + length]
            pos += length + 12
        raw = zlib.decompress(compressed)
        stride = 1 + width * 3
        return {(x, y): tuple(raw[y * stride + 1 + x * 3:y * stride + 3 + x * 3])
                for y in range(height) for x in range(width)}

    def test_panel_point_finds_the_pixel_turn_png_moved_for_every_quarter(self):
        import screenshot as screenshot_tool
        width, height = 3, 5
        for quarter in range(4):
            turned = self.pixels(screenshot_tool.turn_png(self.coordinate_png(width, height), quarter))
            for (x, y), source in turned.items():
                self.assertEqual(screenshot_tool.panel_point(x, y, quarter, width, height), source,
                                 f"quarter {quarter} at ({x}, {y})")

    def test_maps_the_default_screenshot_to_the_panel(self):
        self.assertEqual(device.touch_point(0, 0), (367, 0))
        self.assertEqual(device.touch_point(447, 367), (0, 447))
        # render_lab's NEXT SCENE with the board landscape, tapped on the board.
        self.assertEqual(device.touch_point(222, 246), (121, 222))

    def test_rejects_a_point_outside_the_default_screenshot(self):
        for x, y in ((448, 0), (0, 368), (-1, 10), (10, -1)):
            with self.assertRaises(ValueError):
                device.touch_point(x, y)


class ScreenshotCommandTests(unittest.TestCase):
    """device.screenshot() under a faked serial port, driving the real
    launcher/tools/device/screenshot.py decode, see that module's own tests
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
        store = mock_store()
        with tempfile.TemporaryDirectory() as directory:
            out = str(Path(directory) / "shot.bmp")
            args = Namespace(owner="agent", purpose="autana screenshot", wait=0, out=out, timeout=1.0)
            with mock.patch.object(device, "open_when_free", return_value=connection):
                code = device.screenshot(args, store, BOARD)
            self.assertEqual(code, 0)
            self.assertTrue((Path(directory) / "shot.png").is_file())
            self.assertEqual(json.loads((Path(directory) / "shot.json").read_text()),
                             {"heap": 1, "image_turn_quarter": 3})
        store.acquire.assert_called_once()

    def test_defaults_its_out_path_to_a_timestamped_name_in_the_cwd(self):
        connection = FakeConnection([self.wire_lines(self.minimal_bmp())])
        store = mock_store()
        fixed_now = datetime(2026, 9, 16, 12, 30, 45)
        with tempfile.TemporaryDirectory() as directory:
            args = Namespace(owner="agent", purpose="autana screenshot", wait=0, out=None, timeout=1.0)
            with mock.patch.object(device, "open_when_free", return_value=connection), \
                 mock.patch.object(device, "now", return_value=fixed_now), \
                 mock.patch.object(device.Path, "cwd", return_value=Path(directory)):
                device.screenshot(args, store, BOARD)
            self.assertTrue((Path(directory) / "screenshot_20260916_123045.png").is_file())

    def capture_marked(self, directory, orientation, **view):
        connection = FakeConnection([self.wire_lines(
            self.marked_bmp(), json.dumps({"orientation_quarter": orientation}))])
        store = mock_store()
        out = str(Path(directory) / "shot.png")
        args = Namespace(owner="agent", purpose="p", wait=0, out=out, timeout=1.0, **view)
        with mock.patch.object(device, "open_when_free", return_value=connection):
            device.screenshot(args, store, BOARD)
        return out

    def test_default_turns_the_framebuffer_to_the_board_shape_and_records_it(self):
        with tempfile.TemporaryDirectory() as directory:
            out = self.capture_marked(directory, 2)
            self.assertEqual(self.image_shape(out), (3, 2))
            self.assertEqual(self.first_pixel(out), bytes((18, 17, 16)))
            self.assertEqual(json.loads(Path(directory, "shot.json").read_text())["image_turn_quarter"], 3)

    def test_framebuffer_mode_keeps_the_bytes_orientation_and_records_zero_turn(self):
        with tempfile.TemporaryDirectory() as directory:
            out = self.capture_marked(directory, 1, framebuffer=True)
            self.assertEqual(self.image_shape(out), (2, 3))
            self.assertEqual(self.first_pixel(out), bytes((15, 14, 13)))
            self.assertEqual(json.loads(Path(directory, "shot.json").read_text())["image_turn_quarter"], 0)

    def test_as_shown_uses_the_captured_orientation_quarter(self):
        with tempfile.TemporaryDirectory() as directory:
            out = self.capture_marked(directory, 2, as_shown=True)
            self.assertEqual(self.image_shape(out), (2, 3))
            self.assertEqual(self.first_pixel(out), bytes((6, 5, 4)))
            self.assertEqual(json.loads(Path(directory, "shot.json").read_text())["image_turn_quarter"], 2)

    def test_screenshot_views_are_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            device.main(["screenshot", "--as-shown", "--framebuffer"])

    def test_a_refusal_propagates_as_a_runtime_error(self):
        connection = FakeConnection([b"SCREENSHOT_REFUSED: no room in PSRAM\n"])
        store = mock_store()
        args = Namespace(owner="agent", purpose="p", wait=0, out=None, timeout=1.0)
        with mock.patch.object(device, "open_when_free", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "no room in PSRAM"):
                device.screenshot(args, store, BOARD)




if __name__ == "__main__":
    unittest.main()
