"""The board lock as every command meets it: boards found by USB serial
number, the guard on each port primitive, what a lost lock stops, the flash
hand-off to build.sh and flash_image.sh, and the durations status estimates from."""

import isolation  # noqa: F401  (first: keeps the suite out of real records)
import ast
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from argparse import Namespace
from datetime import datetime
from pathlib import Path
from unittest import mock

DEVICE = Path(__file__).resolve().parents[1]
ENGINE = DEVICE.parents[1]
sys.path.insert(0, str(DEVICE))
import device  # noqa: E402
import device_lock  # noqa: E402

BOARD_A = "90:70:69:FE:A3:08"
BOARD_B = "90:70:69:FE:B1:22"


def usb(serial, port):
    return types.SimpleNamespace(vid=device.ESPRESSIF_VID, serial_number=serial, device=port)


class FakeSerial:
    opened = []

    def __init__(self):
        self.port = None

    def open(self):
        FakeSerial.opened.append(self.port)


@contextlib.contextmanager
def plugged(*ports):
    """pyserial as device.py imports it, with `ports` on USB. The list can be
    changed while patched, as a board renumbering after a reset does."""
    listing = list(ports)
    list_ports = types.ModuleType("serial.tools.list_ports")
    list_ports.comports = lambda: list(listing)
    tools = types.ModuleType("serial.tools")
    tools.list_ports = list_ports
    serial = types.ModuleType("serial")
    serial.tools = tools
    serial.Serial = FakeSerial
    FakeSerial.opened = []
    with mock.patch.dict(sys.modules, {"serial": serial, "serial.tools": tools,
                                       "serial.tools.list_ports": list_ports}):
        yield listing


def durations_rows(store):
    path = store.root / device_lock.DURATIONS_FILE
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def replace_lock_once(store, started):
    """Hands BOARD_A's lock to another token once `started` exists, under the
    guard so a heartbeat cannot write the old token back over it."""
    def replace():
        deadline = time.monotonic() + 30
        while not started.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        with store.guard(BOARD_A):
            lock = store.read_json(store.lock_path(BOARD_A))
            store.write_json(store.lock_path(BOARD_A), dict(lock, token="other"))

    threading.Thread(target=replace, daemon=True).start()


class Store(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.store = device_lock.LockStore(self.root / "locks")


class PrimitiveGuardTests(Store):
    def test_every_port_primitive_asks_for_the_locked_port(self):
        tree = ast.parse((DEVICE / "device.py").read_text(encoding="utf-8"))
        primitives = {
            node.name for node in tree.body if isinstance(node, ast.FunctionDef)
            and (any(isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute)
                     and call.func.attr == "Serial" for call in ast.walk(node))
                 or (any(isinstance(arg, ast.Constant) and arg.value == "esptool"
                         for arg in ast.walk(node))
                     and any(isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute)
                             and call.func.attr == "run" for call in ast.walk(node))))
        }
        self.assertEqual(primitives, {"open_serial", "reset"})
        for name in primitives:
            with self.subTest(name=name), mock.patch.object(
                    device, "locked_port", side_effect=RuntimeError("no lock")) as guard:
                with self.assertRaisesRegex(RuntimeError, "no lock"):
                    getattr(device, name)()
                guard.assert_called_once()

    def test_no_lock_no_port(self):
        with plugged(usb(BOARD_A, "COM5")):
            with self.assertRaisesRegex(RuntimeError, "requires the device lock"):
                device.open_serial()

    def test_a_replaced_or_stale_lock_refuses_the_port(self):
        clock = [1000.0]
        store = device_lock.LockStore(self.root / "locks", now=lambda: clock[0])
        with plugged(usb(BOARD_A, "COM5")), device.HeldLock(store, BOARD_A, "a", "send", 0) as held:
            self.assertEqual(device.locked_port(), "COM5")
            path = store.lock_path(BOARD_A)
            store.write_json(path, dict(held.held, token="replacement"))
            with self.assertRaises(device.LockLost):
                device.locked_port()
            store.write_json(path, held.held)
            clock[0] += device_lock.DEFAULT_STALE_SECONDS + 1
            with self.assertRaises(device.LockLost):
                device.locked_port()
            clock[0] -= device_lock.DEFAULT_STALE_SECONDS + 1

    def test_a_nested_lock_restores_the_outer_one(self):
        inner_store = device_lock.LockStore(self.root / "inner")
        with device.HeldLock(self.store, BOARD_A, "a", "send", 0) as outer:
            with device.HeldLock(inner_store, BOARD_A, "b", "send", 0) as inner:
                self.assertIs(device.ACTIVE_LOCK.held, inner)
            self.assertIs(device.ACTIVE_LOCK.held, outer)

    def test_a_second_process_cannot_take_a_live_lock_and_a_reclaim_stops_the_first(self):
        probe = ("import sys, time; sys.path.insert(0, sys.argv[1]); import device_lock; "
                 "s = device_lock.LockStore(sys.argv[2], now=lambda: time.time() + float(sys.argv[4])); "
                 "print(bool(s.acquire(sys.argv[3], 'second', 'send', wait=0)))")
        with device.HeldLock(self.store, BOARD_A, "first", "listen", 0) as held:
            def second(offset):
                return subprocess.run([sys.executable, "-c", probe, str(DEVICE),
                                       str(self.store.root), BOARD_A, str(offset)],
                                      capture_output=True, text=True).stdout.strip()
            self.assertEqual(second(0), "False")
            self.assertEqual(second(device_lock.DEFAULT_STALE_SECONDS + 1), "True")
            with self.assertRaises(device.LockLost):
                device.require_live_lock()
            self.store.write_json(self.store.lock_path(BOARD_A), held.held)

    def test_check_token_cli_refuses_a_foreign_missing_stale_or_dead_lock(self):
        held = self.store.acquire(BOARD_A, "agent", "flash")

        def check(token):
            return subprocess.run([sys.executable, str(DEVICE / "device_lock.py"),
                                   "--root", str(self.store.root),
                                   "--board", " " + BOARD_A.lower() + "\n",
                                   "check-token", "--token", token],
                                  capture_output=True).returncode
        self.assertEqual(check(held["token"]), 0)
        self.assertNotEqual(check("foreign"), 0)
        self.store.lock_path(BOARD_A).unlink()
        self.assertNotEqual(check(held["token"]), 0)
        self.store.write_json(self.store.lock_path(BOARD_A), dict(held, heartbeat_at=1))
        self.assertNotEqual(check(held["token"]), 0)
        self.store.write_json(self.store.lock_path(BOARD_A),
                              dict(held, heartbeat_at=time.time(), pid=99999999))
        self.assertNotEqual(check(held["token"]), 0)


class BoardTests(Store):
    """Boards are named by USB serial number; two plugged in at once are two
    boards with two locks."""

    def test_the_only_board_plugged_in_is_found_without_a_name(self):
        with plugged(usb(BOARD_A.lower(), "COM5")):
            self.assertEqual(device.find_board(), device.Board(BOARD_A, "COM5"))

    def test_autana_board_picks_one_of_two_by_serial(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")), \
                mock.patch.dict(os.environ, {"AUTANA_BOARD": BOARD_B.lower()}):
            self.assertEqual(device.find_board(), device.Board(BOARD_B, "COM7"))
            self.assertEqual(device.find_board(BOARD_A), device.Board(BOARD_A, "COM5"))

    def test_two_boards_and_none_chosen_fails_naming_both(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")):
            with self.assertRaises(RuntimeError) as caught:
                device.find_board()
        self.assertNotIsInstance(caught.exception, device.NoBoard)
        self.assertIn(BOARD_A, str(caught.exception))
        self.assertIn(BOARD_B, str(caught.exception))

    def test_a_board_not_on_usb_is_no_board(self):
        with plugged(usb(BOARD_A, "COM5")):
            with self.assertRaises(device.NoBoard):
                device.find_board(BOARD_B)
        with plugged():
            with self.assertRaises(device.NoBoard):
                device.find_board()

    def test_each_board_locks_independently(self):
        self.assertTrue(self.store.acquire(BOARD_A, "a", "flash"))
        self.assertTrue(self.store.acquire(BOARD_B, "b", "flash"))
        self.assertIsNone(self.store.acquire(BOARD_A, "c", "flash", wait=0))
        self.assertEqual(self.store.boards(), [BOARD_A, BOARD_B])

    def status(self, *argv):
        with mock.patch.object(device_lock, "default_root", return_value=self.store.root), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(device.main(["status", *argv]), 0)
        return output.getvalue()

    def test_status_json_lists_every_board_plugged_in_or_locked(self):
        self.store.acquire(BOARD_A, "alice", "firmware", kind="flash")
        self.store.set_human("90:70:69:FE:C0:01", "maintainer", "on the bench")
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")):
            boards = json.loads(self.status("--json"))["boards"]
        by_board = {entry["board"]: entry for entry in boards}
        self.assertEqual(list(by_board), sorted(by_board))
        self.assertEqual((by_board[BOARD_A]["port"], by_board[BOARD_A]["state"],
                          by_board[BOARD_A]["holder"]),
                         ("COM5", "held", {"owner": "alice", "purpose": "firmware"}))
        self.assertEqual((by_board[BOARD_B]["port"], by_board[BOARD_B]["state"]),
                         ("COM7", "unlocked"))
        self.assertEqual((by_board["90:70:69:FE:C0:01"]["port"],
                          by_board["90:70:69:FE:C0:01"]["state"]), (None, "human"))
        self.assertEqual(set(by_board[BOARD_B]), {
            "board", "port", "state", "holder", "since", "elapsed_seconds",
            "estimated_free", "stale", "waiting"})

    def test_status_names_one_board_when_one_is_chosen(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")):
            with mock.patch.dict(os.environ, {"AUTANA_BOARD": BOARD_B}):
                text = self.status()
        self.assertEqual(text, f"board {BOARD_B} (on COM7)\n  unlocked\n")

    def test_status_keeps_a_reclaimable_lock_visible(self):
        self.store.write_json(self.store.lock_path(BOARD_A), {
            "acquired_at": 1, "board": BOARD_A, "heartbeat_at": 1, "host": "elsewhere",
            "kind": "listen", "owner": "gone", "pid": 1, "purpose": "listen", "token": "t"})
        with plugged():
            entry = json.loads(self.status("--json"))["boards"][0]
        self.assertEqual((entry["state"], entry["stale"]),
                         ("unlocked", {"owner": "gone", "purpose": "listen",
                                       "reason": "heartbeat expiry"}))


class PortFollowingTests(Store):
    """A reset can bring the board back on another COM number; the lock is
    the board's, so every port operation looks the port up again."""

    def test_open_and_reset_use_the_port_the_board_has_now(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM6")) as ports, \
                mock.patch.object(device.subprocess, "run") as run, \
                mock.patch.object(device, "python_with_pyserial", return_value="python"), \
                device.HeldLock(self.store, BOARD_A, "a", "reset", 0):
            device.open_serial()
            device.reset()
            ports[:] = [usb(BOARD_B, "COM5"), usb(BOARD_A, "COM9")]
            device.open_serial()
            device.reset()
        self.assertEqual(FakeSerial.opened, ["COM5", "COM9"])
        reset_ports = [call.args[0][call.args[0].index("-p") + 1] for call in run.call_args_list]
        self.assertEqual(reset_ports, ["COM5", "COM9"])

    def test_a_board_off_usb_is_waited_for_not_mistaken(self):
        with plugged() as ports, device.HeldLock(self.store, BOARD_A, "a", "reset", 0):
            def back(unused_seconds):
                ports[:] = [usb(BOARD_A, "COM9")]
            connection = device.open_when_free(5, sleep=back)
        self.assertEqual(connection.port, "COM9")


class LostLockTests(Store):
    def lose_heartbeat(self, held):
        with mock.patch.object(self.store, "heartbeat", return_value=False):
            self.assertTrue(held.lost.wait(5))

    def test_a_lost_heartbeat_alone_stops_the_next_port_operation(self):
        with plugged(usb(BOARD_A, "COM5")), \
                mock.patch.object(device.HeldLock, "HEARTBEAT_SECONDS", 0.01), \
                contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(device.LockLost):
            with device.HeldLock(self.store, BOARD_A, "a", "listen", 0) as held:
                self.lose_heartbeat(held)
                self.assertTrue(self.store.check_token(BOARD_A, held.held["token"]))
                with self.assertRaises(device.LockLost):
                    device.open_serial()
                with self.assertRaises(device.LockLost):
                    device.reset()

    def test_a_lost_heartbeat_stops_a_capture_mid_read(self):
        class Endless:
            def read(self, unused_size):
                return b"line\n"

        with mock.patch.object(device.HeldLock, "HEARTBEAT_SECONDS", 0.01), \
                contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(device.LockLost):
            with device.HeldLock(self.store, BOARD_A, "a", "listen", 0) as held:
                self.lose_heartbeat(held)
                started = time.monotonic()
                with self.assertRaises(device.LockLost):
                    device.capture(Endless(), self.root / "capture.log", 30, None, complete=None)
                self.assertLess(time.monotonic() - started, 5)

    def test_a_heartbeat_on_a_stale_lock_is_refused(self):
        clock = [1000.0]
        store = device_lock.LockStore(self.root / "locks", now=lambda: clock[0])
        held = store.acquire(BOARD_A, "a", "listen")
        clock[0] += device_lock.DEFAULT_STALE_SECONDS - 1
        self.assertTrue(store.heartbeat(BOARD_A, held["token"]))
        clock[0] += device_lock.DEFAULT_STALE_SECONDS + 1
        self.assertFalse(store.heartbeat(BOARD_A, held["token"]))

    def test_a_replaced_lock_raises_at_exit_and_records_the_loss(self):
        with self.assertRaises(device.LockLost):
            with device.HeldLock(self.store, BOARD_A, "a", "send", 0, kind="send") as held:
                self.store.write_json(self.store.lock_path(BOARD_A),
                                      dict(held.held, token="replacement"))
        self.assertEqual(durations_rows(self.store)[-1]["error"], "device lock was lost")

    def test_a_command_that_kept_its_lock_ends_cleanly(self):
        with device.HeldLock(self.store, BOARD_A, "a", "send", 0, kind="send"):
            pass
        self.assertIsNone(durations_rows(self.store)[-1]["error"])
        self.assertIsNone(self.store.status(BOARD_A)["lock"])

    def reopen_after_reset(self, second_open):
        class PortGone:
            said = False

            def read(self, unused_size):
                if not self.said:
                    self.said = True
                    return b"boot line\n"
                raise OSError("USB device disappeared")

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                return False

        opens = mock.Mock(side_effect=[PortGone(), second_open])
        with mock.patch.object(device, "open_when_free", opens), \
                contextlib.redirect_stderr(io.StringIO()):
            return device.capture_after_reset(self.root / "reset.log", 30, None)

    def test_a_reopen_after_reset_ends_port_lost_only_on_usb_absence(self):
        with device.HeldLock(self.store, BOARD_A, "a", "reset", 0):
            self.assertEqual(self.reopen_after_reset(device.PortUnavailable("gone"))[1],
                             "port lost")
            with self.assertRaises(device.LockLost):
                self.reopen_after_reset(device.LockLost())
            with self.assertRaisesRegex(RuntimeError, "several boards"):
                self.reopen_after_reset(RuntimeError("several boards are plugged in"))

    def test_send_stops_on_a_lost_lock(self):
        class Silent:
            def __init__(self, held):
                self.held = held

            def reset_input_buffer(self):
                pass

            def write(self, unused):
                pass

            def flush(self):
                pass

            def read(self, unused_size):
                self.held.lost.set()
                return b""

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                return False

        args = Namespace(owner="a", purpose="send", wait=0, line="TUNE", reply="TUNE",
                         until=["TUNE_END"], seconds=30, optional=False)
        with mock.patch.object(device, "open_when_free",
                               side_effect=lambda: Silent(device.ACTIVE_LOCK.held)):
            started = time.monotonic()
            with self.assertRaises(device.LockLost):
                device.send(args, self.store, BOARD_A)
        self.assertLess(time.monotonic() - started, 5)

    def test_screenshot_stops_on_a_lost_lock(self):
        import screenshot as screenshot_tool

        def lost_mid_read(unused_connection, unused_timeout, on_status=None):
            device.ACTIVE_LOCK.held.lost.set()
            return b"png", "{}"

        args = Namespace(owner="a", purpose="screenshot", wait=0, timeout=1,
                         out=str(self.root / "shot.png"), framebuffer=False, as_shown=False)
        with mock.patch.object(device, "open_when_free", return_value=contextlib.nullcontext()), \
                mock.patch.object(screenshot_tool, "read_screenshot", lost_mid_read), \
                mock.patch.object(screenshot_tool, "write_capture") as written:
            with self.assertRaises(device.LockLost):
                device.screenshot(args, self.store, BOARD_A)
        written.assert_not_called()


def no_build(*unused, **unused_keywords):
    pass


class FlashTests(Store):
    def flash(self, write, store=None, variant="dev", build=no_build):
        store = store or self.store
        worktree = self.root / "engine"
        for script in (device.BUILD_SCRIPT, device.FLASH_SCRIPT):
            (worktree / script).parent.mkdir(parents=True, exist_ok=True)
            (worktree / script).write_text("")
        args = Namespace(owner="agent", purpose="flash", wait=0, variant=variant,
                         worktree=str(worktree), out=None)
        with mock.patch.object(device, "open_when_free", return_value=contextlib.nullcontext()), \
                mock.patch.object(device, "run_build", side_effect=build), \
                mock.patch.object(device, "run_while_held", side_effect=write), \
                mock.patch.object(device, "git_commit", return_value="c0ffee"), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            try:
                device.flash(args, store, BOARD_A)
            finally:
                self.output = output.getvalue()
        return self.output

    def entry(self):
        index = Path(os.environ["AUTANA_RECORDS"]) / "index.jsonl"
        return json.loads(index.read_text(encoding="utf-8").splitlines()[-1])

    def test_every_variant_names_its_build_and_says_its_boot_is_unverified(self):
        for variant in ("release", "dev", "diag"):
            with self.subTest(variant=variant):
                output = self.flash(lambda *unused, stdout, **unused_kw:
                                    stdout.write(b"BUILD_ID=abc\n"), variant=variant)
                self.assertIn("flashed BUILD_ID=abc (esptool hash verified; boot not verified)",
                              output)
                self.assertEqual((self.entry()["board"], self.entry()["build_id"]),
                                 (BOARD_A, "abc"))

    def test_a_flash_log_without_a_build_id_fails(self):
        with self.assertRaisesRegex(RuntimeError, "no BUILD_ID"):
            self.flash(lambda *unused, stdout, **unused_kw: stdout.write(b"built\n"))
        self.assertIn("no BUILD_ID", self.entry()["error"])

    def test_the_build_runs_with_the_board_free_for_anyone_else(self):
        seen = []

        def build(*unused, stdout, **unused_keywords):
            other = device_lock.LockStore(self.store.root)
            taken = other.acquire(BOARD_A, "someone-else", "flash", wait=0)
            seen.append(bool(taken))
            other.release(BOARD_A, taken["token"])
            stdout.write(b"BUILD_ID=abc\n")

        self.flash(no_build, build=build)
        self.assertEqual(seen, [True])
        self.assertEqual(self.entry()["build_id"], "abc")

    def test_a_failed_build_never_takes_the_lock(self):
        def failing_build(command, *unused, stdout, **unused_keywords):
            stdout.write(b"error: it does not compile\n")
            raise subprocess.CalledProcessError(1, command)

        with mock.patch.object(self.store, "acquire", wraps=self.store.acquire) as acquire, \
                self.assertRaisesRegex(RuntimeError, "build.sh failed .exit 1.: "
                                                     "error: it does not compile"):
            self.flash(no_build, build=failing_build)
        acquire.assert_not_called()
        self.assertIn("build.sh failed", self.entry()["error"])
        self.assertIsNone(self.entry()["acquired_at"])

    def test_the_build_and_the_flash_share_one_log(self):
        def build(*unused, stdout, **unused_keywords):
            stdout.write(b"built\n")

        def write(*unused, stdout, **unused_keywords):
            stdout.write(b"BUILD_ID=abc\nflashed\n")

        self.flash(write, build=build)
        log = Path(self.entry()["capture_path"]).read_bytes()
        self.assertEqual(log.replace(b"\r\n", b"\n"), b"built\nBUILD_ID=abc\nflashed\n")

    def test_a_heartbeat_lost_during_the_flash_fails_before_the_flash_is_claimed(self):
        def build_then_lose(*unused, stdout, **unused_keywords):
            stdout.write(b"BUILD_ID=abc\n")
            device.ACTIVE_LOCK.held.lost.set()

        with self.assertRaises(device.LockLost):
            self.flash(build_then_lose)
        self.assertNotIn("flashed", self.output)
        self.assertEqual((self.entry()["build_id"], self.entry()["error"]),
                         (None, "device lock was lost"))

    def test_a_lock_gone_after_the_build_fails(self):
        def build_then_lose(*unused, stdout, **unused_keywords):
            stdout.write(b"BUILD_ID=abc\n")
            lock = self.store.read_json(self.store.lock_path(BOARD_A))
            self.store.write_json(self.store.lock_path(BOARD_A), dict(lock, token="other"))

        with self.assertRaises(device.LockLost):
            self.flash(build_then_lose)
        self.assertEqual(self.entry()["build_id"], None)

    def test_a_refused_build_id_update_fails(self):
        with mock.patch.object(self.store, "set_expected_build_id", return_value=False), \
                self.assertRaises(device.LockLost):
            self.flash(lambda *unused, stdout, **unused_kw: stdout.write(b"BUILD_ID=abc\n"))
        self.assertNotIn("flashed", self.output)

    def test_an_in_flight_flash_is_stopped_when_the_lock_is_lost(self):
        worktree = self.root / "engine"
        script = worktree / device.FLASH_SCRIPT
        script.parent.mkdir(parents=True)
        (worktree / device.BUILD_SCRIPT).parent.mkdir(parents=True)
        (worktree / device.BUILD_SCRIPT).write_text("")
        started = self.root / "started"
        script.write_text("import pathlib, sys, time\n"
                          f"pathlib.Path({str(started)!r}).write_text('')\n"
                          "time.sleep(60)\n")
        args = Namespace(owner="agent", purpose="flash", wait=0, variant="dev",
                         worktree=str(worktree), out=str(self.root / "flash.log"))
        popen = subprocess.Popen
        children = []

        def recorded(*arguments, **options):
            children.append(popen(*arguments, **options))
            return children[-1]

        def lose_when_started(held):
            deadline = time.monotonic() + 30
            while not started.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            held.lost.set()

        with mock.patch.object(device, "git_bash", return_value=sys.executable), \
                mock.patch.object(device, "open_when_free", return_value=contextlib.nullcontext()), \
                mock.patch.object(device.subprocess, "Popen", side_effect=recorded), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(device.LockLost):
                with device.HeldLock(self.store, BOARD_A, "agent", "flash", 0) as held:
                    threading.Thread(target=lose_when_started, args=(held,), daemon=True).start()
                    begun = time.monotonic()
                    device.flash(args, self.store, BOARD_A, held_lock=held)
        self.assertLess(time.monotonic() - begun, 30)
        self.assertIsNotNone(children[-1].poll())

    def test_a_script_that_names_its_build_and_then_fails_fails_the_flash(self):
        worktree = self.root / "engine"
        for script in (device.BUILD_SCRIPT, device.FLASH_SCRIPT):
            (worktree / script).parent.mkdir(parents=True, exist_ok=True)
        (worktree / device.BUILD_SCRIPT).write_text("print('BUILD_ID=abc')\n")
        (worktree / device.FLASH_SCRIPT).write_text(
            "import sys\n"
            "print('A fatal error occurred: Could not open COM3, the port is busy')\n"
            "print('FAILED: CMakeFiles/flash')\n"
            "sys.exit(2)\n")
        log = self.root / "flash.log"
        args = Namespace(owner="agent", purpose="flash", wait=0, variant="dev",
                         worktree=str(worktree), out=str(log))
        with mock.patch.object(device, "git_bash", return_value=sys.executable), \
                mock.patch.object(device, "open_when_free", return_value=contextlib.nullcontext()), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(RuntimeError) as caught:
                device.flash(args, self.store, BOARD_A)
        self.assertEqual(str(caught.exception),
                         "flash_image.sh failed (exit 2): A fatal error occurred: Could not open "
                         f"COM3, the port is busy - flash log: {log}")
        self.assertNotIn("flashed", output.getvalue())
        self.assertIn("exit 2", self.entry()["error"])
        self.assertIsNone(self.entry()["build_id"])

    def test_the_editors_build_runs_before_it_queues_and_a_failed_one_never_does(self):
        commands = [["build"], ["write"]]
        seen = []

        def build(command, **unused_keywords):
            seen.append(self.store.status(BOARD_A)["lock"])

        with mock.patch.object(device, "run_build", side_effect=build),                 mock.patch.object(device, "run_while_held") as write:
            device.flash_script(self.store, BOARD_A, "agent", "flash", commands, 0)
        self.assertEqual(seen, [None])
        self.assertEqual(write.call_args.args[0], ["write"])

        failed = subprocess.CalledProcessError(1, ["build"])
        with mock.patch.object(device, "run_build", side_effect=failed),                 mock.patch.object(self.store, "acquire") as acquire,                 self.assertRaises(subprocess.CalledProcessError):
            device.flash_script(self.store, BOARD_A, "agent", "flash", commands, 0)
        acquire.assert_not_called()

    def test_a_lost_lock_stops_the_scripts_whole_process_tree(self):
        started = self.root / "grandchild.pid"
        script = self.root / "build.py"
        script.write_text(
            "import pathlib, subprocess, sys, time\n"
            "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
            f"pathlib.Path({str(started)!r}).write_text(str(child.pid))\n"
            "time.sleep(60)\n")

        replace_lock_once(self.store, started)
        with mock.patch.object(device.HeldLock, "HEARTBEAT_SECONDS", 0.05), \
                contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(device.LockLost):
            device.flash_script(self.store, BOARD_A, "agent", "flash",
                                [[sys.executable, "-c", "pass"], [sys.executable, str(script)]], 0,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        grandchild = int(started.read_text())
        self.addCleanup(subprocess.run, ["taskkill", "/F", "/PID", str(grandchild)]
                        if os.name == "nt" else ["kill", "-9", str(grandchild)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 5
        while device_lock.process_alive(grandchild) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(device_lock.process_alive(grandchild))


class EditorFlashTests(Store):
    """The boot animation editor's Build & Flash goes through the same shared
    flash call as device.py flash, so a lost lock stops it too."""

    def test_a_lost_lock_stops_the_editors_flash(self):
        sys.path.insert(0, str(ENGINE / "launcher" / "tools" / "boot_anim"))
        self.addCleanup(sys.path.remove, str(ENGINE / "launcher" / "tools" / "boot_anim"))
        import boot_anim_editor_server as editor

        started = self.root / "script.pid"
        script = self.root / "flash_image.py"
        script.write_text("import os, pathlib, time\n"
                          f"pathlib.Path({str(started)!r}).write_text(str(os.getpid()))\n"
                          "time.sleep(60)\n")
        build = self.root / "build.py"
        build.write_text("")
        generator = self.root / "generator.py"
        generator.write_text("print('/* header */')\n")
        store = device_lock.LockStore()

        replace_lock_once(store, started)
        payload = {key: 0 for key in editor.PAYLOAD_KEYS}
        with mock.patch.object(editor, "TIMELINE_JSON", str(self.root / "timeline.json")), \
                mock.patch.object(editor, "TIMELINE_HEADER", str(self.root / "timeline.h")), \
                mock.patch.object(editor, "GENERATOR", str(generator)), \
                mock.patch.object(device, "flash_commands",
                                  return_value=[[sys.executable, str(build)],
                                                [sys.executable, str(script)]]), \
                mock.patch.object(editor, "_ensure_image_current"), \
                mock.patch.object(editor, "find_bash", return_value=sys.executable), \
                mock.patch.object(device.HeldLock, "HEARTBEAT_SECONDS", 0.05), \
                contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(device.LockLost):
            editor.Renderer.__new__(editor.Renderer).build_and_flash(payload, BOARD_A)
        deadline = time.monotonic() + 5
        pid = int(started.read_text())
        while device_lock.process_alive(pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(device_lock.process_alive(pid))


class FlashImageScriptTests(unittest.TestCase):
    """The real flash_image.sh, copied beside the files it sources, with
    idf.py and pyserial stubbed: a token that is not the board's live lock
    stops it before any idf.py call."""

    def setUp(self):
        try:
            self.bash = device.git_bash()
        except RuntimeError as error:
            self.skipTest(str(error))
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tree = Path(temp.name)
        (self.tree / "launcher").mkdir()
        for relative in ("scripts/device/flash_image.sh", "launcher/tools/build/idf.sh",
                         "launcher/tools/build/idf_shim.bat", "launcher/tools/build/espressif.py",
                         "scripts/device/device.py", "scripts/device/device_lock.py",
                         "scripts/device/device_hook.py", "scripts/device/device_report.py",
                         "scripts/device/main_copy.py"):
            (self.tree / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ENGINE / relative, self.tree / relative)
        stubs = self.tree / "stubs"
        (stubs / "serial" / "tools").mkdir(parents=True)
        (stubs / "serial" / "__init__.py").write_text("")
        (stubs / "serial" / "tools" / "__init__.py").write_text("")
        (stubs / "serial" / "tools" / "list_ports.py").write_text(
            "from types import SimpleNamespace\n"
            "def comports():\n"
            f"    return [SimpleNamespace(vid=0x303A, serial_number={BOARD_A!r}, device='COM42')]\n")
        self.log = self.tree / "idf.log"
        (stubs / "idf_stub.py").write_text(
            "import sys\n"
            f"with open({str(self.log)!r}, 'a') as log:\n"
            "    log.write(' '.join(sys.argv[1:]) + '\\n')\n")
        # One stub per platform: cmd would run a POSIX idf.py found first on
        # PATH through the .py file association instead of idf.py.bat.
        if os.name == "nt":
            (stubs / "idf.py.bat").write_bytes(
                f'@"{sys.executable}" "{stubs / "idf_stub.py"}" %*\r\n'.encode())
            self.export = stubs / "export.bat"
            self.export.write_bytes(f'@set "PATH={stubs};%PATH%"\r\n'.encode())
        else:
            (stubs / "idf.py").write_text(
                f'#!/bin/sh\nexec "{sys.executable}" "{stubs / "idf_stub.py"}" "$@"\n')
            (stubs / "idf.py").chmod(0o755)
            self.export = stubs / "export.sh"
            self.export.write_text(f'PATH="{stubs.as_posix()}:$PATH"\n')
        self.stubs = stubs
        self.store = device_lock.LockStore(self.tree / "locks")

    def run_script(self, token, *flags):
        environment = dict(os.environ, AUTANA_DEVICE_LOCK_TOKEN=token, AUTANA_BOARD=BOARD_A,
                           AUTANA_DEVICE_LOCK_ROOT=str(self.store.root), PYTHONPATH=str(self.stubs),
                           PATH=str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"])
        return subprocess.run([self.bash, str(self.tree / "scripts/device/flash_image.sh"),
                               *flags, str(self.export)], env=environment,
                              stdin=subprocess.DEVNULL, capture_output=True, text=True,
                              timeout=120)

    def idf_calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_a_foreign_token_stops_before_any_idf_call(self):
        held = self.store.acquire(BOARD_A, "agent", "flash")
        self.addCleanup(self.store.release, BOARD_A, held["token"])
        result = self.run_script("foreign")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("device lock token is not active for board " + BOARD_A, result.stderr)
        self.assertEqual(self.idf_calls(), [])

    def test_the_live_token_flashes_the_variants_build_to_the_port_the_board_has_now(self):
        held = self.store.acquire(BOARD_A, "agent", "flash")
        self.addCleanup(self.store.release, BOARD_A, held["token"])
        for flags, build in (((), "build"), (("--dev",), "build.dev"), (("--diag",), "build.diag")):
            with self.subTest(build=build):
                result = self.run_script(held["token"], *flags)
                self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])
                self.assertEqual(self.idf_calls()[-1], f"-B {build} -p COM42 flash")


class DurationTests(Store):
    def test_estimates_follow_the_kind_not_the_purpose(self):
        clock = [1000.0]
        store = device_lock.LockStore(self.root / "locks", now=lambda: clock[0])
        store.acquire(BOARD_A, "alice", "firmware", kind="flash")
        store.enqueue(BOARD_A, "bob", "watch", kind="listen")
        store.enqueue(BOARD_A, "cara", "firmware", kind="flash")
        clock[0] = 1020.0
        entry = device_lock.status_entry(store, BOARD_A, durations={
            "flash": 120, "listen": 30, "firmware": 999, "watch": 999})
        self.assertEqual(entry["elapsed_seconds"], 20)
        self.assertEqual(entry["estimated_free"], 1120)
        self.assertEqual([(w["owner"], w["estimated_start"]) for w in entry["waiting"]],
                         [("bob", 1120), ("cara", 1150)])

    def test_an_unknown_kind_ends_the_estimates_after_it(self):
        clock = [1000.0]
        store = device_lock.LockStore(self.root / "locks", now=lambda: clock[0])
        store.acquire(BOARD_A, "alice", "flash", kind="flash")
        store.enqueue(BOARD_A, "bob", "look", kind="new")
        store.enqueue(BOARD_A, "cara", "flash", kind="flash")
        entry = device_lock.status_entry(store, BOARD_A, durations={"flash": 100})
        self.assertEqual([w["estimated_start"] for w in entry["waiting"]], [1100, None])
        store.set_human(BOARD_A, "maintainer", "bench")
        entry = device_lock.status_entry(store, BOARD_A, durations={"flash": 100})
        self.assertEqual([w["estimated_start"] for w in entry["waiting"]], [None, None])

    def test_history_needs_enough_successes_and_uses_only_the_recent_runs(self):
        root = self.store.root
        for seconds in range(device_lock.ESTIMATE_MINIMUM_RUNS - 1):
            device_lock.record_duration("send", seconds, root=root)
        device_lock.record_duration("send", 999, "failed", root=root)
        self.assertEqual(device_lock.duration_history(root), {})
        device_lock.record_duration("send", 50, root=root)
        self.assertIn("send", device_lock.duration_history(root))
        recent = device_lock.ESTIMATE_RECENT_RUNS
        for seconds in [10_000] * recent + list(range(recent)):
            device_lock.record_duration("flash", seconds, root=root)
        self.assertEqual(device_lock.duration_history(root)["flash"], (recent - 1) / 2)

    def test_the_estimate_is_a_median(self):
        for seconds in (10, 10, 1000):
            device_lock.record_duration("flash", seconds, root=self.store.root)
        self.assertEqual(device_lock.duration_history(self.store.root), {"flash": 10})

    def test_durations_are_recorded_under_the_kind_not_the_purpose(self):
        with device.HeldLock(self.store, BOARD_A, "a", "watch the sand fall", 0, kind="listen"):
            pass
        self.assertEqual(durations_rows(self.store)[-1]["command"], "listen")

    def test_the_file_stays_bounded_however_many_kinds_there_are(self):
        root = self.store.root
        path = root / device_lock.DURATIONS_FILE
        kinds = device_lock.DURATIONS_TRIM_LINES // device_lock.ESTIMATE_RECENT_RUNS + 5
        root.mkdir(parents=True)
        path.write_text("".join(
            json.dumps({"command": f"kind{index % kinds}", "duration_seconds": index,
                        "error": None}) + "\n"
            for index in range(device_lock.DURATIONS_TRIM_LINES)), encoding="utf-8")
        for index in range(10):
            device_lock.record_duration(f"kind{index % kinds}", index, root=root)
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertLessEqual(len(lines), device_lock.DURATIONS_TRIM_LINES)

    def trimmed(self, rows):
        path = self.root / device_lock.DURATIONS_FILE
        device_lock.trim_durations(path, rows)
        return durations_rows(types.SimpleNamespace(root=self.root))

    def test_a_trim_keeps_each_kinds_newest_successes_and_drops_failures(self):
        def row(kind, seconds, error=None):
            return {"command": kind, "duration_seconds": seconds, "error": error}

        recent = device_lock.ESTIMATE_RECENT_RUNS
        rows = ([row("selftest", 900), row("selftest", 901), row("flash", 5, "boom")]
                + [row("flash", seconds) for seconds in range(3 * recent)]
                + [row("send", 1, "timed out")])
        kept = self.trimmed(rows)
        self.assertEqual([r["duration_seconds"] for r in kept if r["command"] == "flash"],
                         list(range(2 * recent, 3 * recent)))
        self.assertEqual([r["duration_seconds"] for r in kept if r["command"] == "selftest"],
                         [900, 901])
        self.assertFalse([r for r in kept if r["error"]])

    def test_a_trim_caps_the_file_when_kinds_pile_up(self):
        recent = device_lock.ESTIMATE_RECENT_RUNS
        kinds = device_lock.DURATIONS_TRIM_LINES // recent + 5
        rows = [{"command": f"kind{index % kinds}", "duration_seconds": index, "error": None}
                for index in range(kinds * recent)]
        kept = self.trimmed(rows)
        self.assertEqual(len(kept), device_lock.DURATIONS_TRIM_LINES)
        self.assertEqual(kept, rows[-device_lock.DURATIONS_TRIM_LINES:])

    def test_each_command_records_its_own_duration_and_a_nested_error(self):
        with device.HeldLock(self.store, BOARD_A, "a", "batch", 0, kind="batch") as outer:
            time.sleep(0.02)
            with device.holding(self.store, BOARD_A, None, outer, "flash"):
                time.sleep(0.02)
            with self.assertRaisesRegex(RuntimeError, "boom"):
                with device.holding(self.store, BOARD_A, None, outer, "run-suite"):
                    raise RuntimeError("boom")
        rows = {row["command"]: row for row in durations_rows(self.store)}
        self.assertGreater(rows["batch"]["duration_seconds"], rows["flash"]["duration_seconds"])
        self.assertEqual([rows[kind]["error"] for kind in ("batch", "flash", "run-suite")],
                         [None, None, "boom"])

    def test_a_command_that_answers_an_error_is_recorded_with_it(self):
        class Answers:
            def reset_input_buffer(self):
                pass

            def write(self, unused):
                pass

            def flush(self):
                pass

            def read(self, unused_size):
                return b"TUNE_ERR unknown\n"

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                return False

        args = Namespace(owner="a", purpose="send", wait=0, line="SET x 1", reply="TUNE",
                         until=["TUNE_ERR"], seconds=5, optional=False)
        with mock.patch.object(device, "open_when_free", return_value=Answers()), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(device.send(args, self.store, BOARD_A), 1)
        self.assertEqual(durations_rows(self.store)[-1]["error"], "TUNE_ERR unknown")

    def test_a_waiter_hears_its_place_at_most_every_thirty_seconds(self):
        clock = [1000.0]
        store = device_lock.LockStore(self.root / "locks", now=lambda: clock[0])
        store.acquire(BOARD_A, "alice", "flash", kind="flash")

        def sleep(seconds):
            clock[0] += seconds

        with mock.patch.object(device_lock.time, "sleep", side_effect=sleep), \
                contextlib.redirect_stderr(io.StringIO()) as notices:
            with self.assertRaisesRegex(RuntimeError, "not acquired"):
                device.HeldLock(store, BOARD_A, "bob", "look", 100)
        lines = notices.getvalue().splitlines()
        self.assertEqual(lines, ["waiting for board: queue place 1; estimated start "
                                 "unknown (no duration history)"] * 4)

    def test_a_waiter_hears_its_place_again_once_the_notice_interval_has_passed(self):
        clock = [1000.0]
        store = device_lock.LockStore(self.root / "locks", now=lambda: clock[0])
        store.acquire(BOARD_A, "alice", "flash", kind="flash")
        ticket = store.enqueue(BOARD_A, "bob", "look", kind="send")
        waiter = device.HeldLock.__new__(device.HeldLock)
        waiter.store, waiter.board, waiter.last_notice = store, BOARD_A, None
        interval = device.HeldLock.NOTICE_SECONDS
        with contextlib.redirect_stderr(io.StringIO()) as notices:
            for offset in (0, interval - 0.01, interval):
                clock[0] = 1000.0 + offset
                waiter.wait_notice(ticket)
        self.assertEqual(len(notices.getvalue().splitlines()), 2)

    def test_a_capture_record_carries_the_board_and_when_it_was_acquired(self):
        store = device_lock.LockStore(self.root / "locks", now=lambda: 1000.0)
        args = Namespace(owner="a", purpose="reset", wait=0, capture=True, seconds=1.0,
                         out=str(self.root / "reset.log"))
        with mock.patch.object(device, "open_when_free", return_value=contextlib.nullcontext()), \
                mock.patch.object(device, "reset_and_capture", return_value=(b"boot\n", "idle")), \
                mock.patch.object(device, "git_commit", return_value="c0ffee"), \
                contextlib.redirect_stdout(io.StringIO()):
            device.reset_device(args, store, BOARD_A)
        index = Path(os.environ["AUTANA_RECORDS"]) / "index.jsonl"
        entry = json.loads(index.read_text(encoding="utf-8").splitlines()[-1])
        self.assertEqual((entry["board"], entry["acquired_at"]),
                         (BOARD_A, datetime.fromtimestamp(1000.0).isoformat()))


class Replies:
    """A port that answers once with `data`, then stays quiet."""

    def __init__(self, data):
        self.data = data

    def read(self, unused_size):
        data, self.data = self.data, b""
        return data

    def write(self, unused):
        pass

    def flush(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        return False


class SuiteFailureTests(Store):
    """A suite that reports FAIL fails its command, in the exit status and in
    the recorded durations, which never count it towards an estimate."""

    FAILED_SUITE = b":1:test_one:FAIL: boom\nSUITE_DONE sand\n"

    def suite_args(self, **extra):
        return Namespace(owner="a", purpose="p", wait=0, suite="sand",
                         out=str(self.root / "suite.log"), max_seconds=5, idle_seconds=None,
                         expect_build_id=None, **extra)

    def errors(self):
        return {row["command"]: row["error"] for row in durations_rows(self.store)}

    def test_run_suite(self):
        with mock.patch.object(device, "open_when_free",
                               side_effect=lambda *unused, **unused_kw: Replies(self.FAILED_SUITE)), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(device.run_suite(self.suite_args(), self.store, BOARD_A), 1)
        self.assertIsNone(self.errors()["run-suite"])

    def test_selftest(self):
        args = Namespace(owner="a", purpose="p", wait=0, worktree=str(self.root), out=None,
                         perf_scope=False, max_seconds=5, idle_seconds=None)
        replies = Replies(b":1:test_one:FAIL: boom\nSELFTEST_COMPLETE failures=1\n")
        with mock.patch.object(device, "build_image"), \
                mock.patch.object(device, "flash", return_value="abc"), \
                mock.patch.object(device, "reset"), \
                mock.patch.object(device, "open_when_free", return_value=replies), \
                mock.patch.object(device, "git_commit", return_value="c0ffee"), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(device.selftest(args, self.store, BOARD_A), 1)
        self.assertIsNone(self.errors()["selftest"])

    def test_batch(self):
        args = Namespace(owner="a", purpose="p", wait=0, worktree=str(self.root), variant="diag",
                         suite=["sand"], runs=2, perf_scope=False, max_seconds=5,
                         idle_seconds=None, out=None)
        with mock.patch.object(device, "build_image"), \
                mock.patch.object(device, "flash", return_value="abc"), \
                mock.patch.object(device, "open_when_free",
                                  side_effect=lambda *unused, **unused_kw: Replies(self.FAILED_SUITE)), \
                mock.patch.object(device, "git_commit", return_value="c0ffee"), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(device.batch(args, self.store, BOARD_A), 1)
        self.assertIsNone(self.errors()["batch"])
        index = Path(os.environ["AUTANA_RECORDS"]) / "index.jsonl"
        entry = json.loads(index.read_text(encoding="utf-8").splitlines()[-1])
        self.assertEqual((entry["command"], entry["error"]), ("batch", None))

    def test_a_batch_capture_that_breaks_is_an_error(self):
        args = Namespace(owner="a", purpose="p", wait=0, worktree=str(self.root), variant="diag",
                         suite=["sand"], runs=1, perf_scope=False, max_seconds=5,
                         idle_seconds=None, out=None)
        with mock.patch.object(device, "build_image"), \
                mock.patch.object(device, "flash", return_value="abc"), \
                mock.patch.object(device, "run_suite", side_effect=RuntimeError("port lost")), \
                mock.patch.object(device, "git_commit", return_value="c0ffee"), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(device.batch(args, self.store, BOARD_A), 1)
        self.assertEqual(self.errors()["batch"], "a batch capture failed")


class EstimateTests(Store):
    def setUp(self):
        super().setUp()
        self.clock = [1000.0]
        self.store = device_lock.LockStore(self.root / "locks", now=lambda: self.clock[0])

    def test_since_stays_when_the_holder_took_the_board(self):
        held = self.store.acquire(BOARD_A, "alice", "flash", kind="flash")
        self.clock[0] = 1200.0
        self.assertTrue(self.store.heartbeat(BOARD_A, held["token"]))
        entry = device_lock.status_entry(self.store, BOARD_A, durations={})
        self.assertEqual((entry["since"], entry["elapsed_seconds"]), (1000.0, 200))

    def test_a_holder_past_its_estimate_frees_now_and_waiters_follow_from_now(self):
        self.store.acquire(BOARD_A, "alice", "flash", kind="flash")
        self.store.enqueue(BOARD_A, "bob", "watch", kind="listen")
        self.store.enqueue(BOARD_A, "cara", "flash", kind="flash")
        self.clock[0] = 1500.0
        entry = device_lock.status_entry(self.store, BOARD_A,
                                         durations={"flash": 100, "listen": 30})
        self.assertEqual(entry["estimated_free"], 1500.0)
        self.assertEqual([w["estimated_start"] for w in entry["waiting"]], [1500.0, 1530.0])

    def test_on_a_free_board_the_first_waiter_starts_now(self):
        self.store.enqueue(BOARD_A, "bob", "watch", kind="listen")
        self.store.enqueue(BOARD_A, "cara", "flash", kind="flash")
        entry = device_lock.status_entry(self.store, BOARD_A, durations={"listen": 30})
        self.assertEqual([w["estimated_start"] for w in entry["waiting"]], [1000.0, 1030.0])

    def test_a_human_reservation_outranks_a_live_lock(self):
        self.store.acquire(BOARD_A, "alice", "flash", kind="flash")
        self.store.set_human(BOARD_A, "maintainer", "checking the panel")
        entry = device_lock.status_entry(self.store, BOARD_A, durations={"flash": 100})
        self.assertEqual((entry["state"], entry["holder"], entry["estimated_free"]),
                         ("human", {"owner": "maintainer", "purpose": "checking the panel"},
                          None))


class IsolationCheckTests(Store):
    """The records guard every test module imports, run in a child process
    whose "real" records root is a scratch directory."""

    CHILD = """
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[3])
import isolation
real, elsewhere = Path(sys.argv[1]), Path(sys.argv[2])
try:
    (real / "refused.txt").write_text("x")
    print("written", flush=True)
except PermissionError:
    print("refused", flush=True)
try:
    open(real / "swallowed.txt", "w").close()
except Exception:
    pass
(elsewhere / "allowed.txt").write_text("x")
print("allowed", flush=True)
"""

    def test_a_write_into_a_real_root_is_refused_and_fails_the_run(self):
        real = self.root / "real-records"
        elsewhere = self.root / "elsewhere"
        real.mkdir()
        elsewhere.mkdir()
        result = subprocess.run(
            [sys.executable, "-c", self.CHILD, str(real), str(elsewhere), str(Path(__file__).parent)],
            env=dict(os.environ, AUTANA_RECORDS=str(real)), capture_output=True, text=True,
            timeout=60)
        self.assertEqual(result.stdout.split(), ["refused", "allowed"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("swallowed.txt", result.stderr)
        self.assertEqual(list(real.iterdir()), [])
        self.assertTrue((elsewhere / "allowed.txt").exists())


class BoardChoiceTests(Store):
    """Which board a command acts on, and that it can queue for one the
    holder's reset has taken off USB."""

    def test_only_an_espressif_port_with_a_serial_number_is_a_board(self):
        other_vendor = types.SimpleNamespace(vid=0x10C4, serial_number="0001", device="COM3")
        no_serial = types.SimpleNamespace(vid=device.ESPRESSIF_VID, serial_number=None,
                                          device="COM4")
        with plugged(other_vendor, no_serial, usb(BOARD_A, "COM5")):
            self.assertEqual(device.plugged_boards(), [device.Board(BOARD_A, "COM5")])

    def test_a_padded_autana_board_still_names_its_board(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")), \
                mock.patch.dict(os.environ, {"AUTANA_BOARD": " " + BOARD_B.lower() + "\n"}):
            self.assertEqual(device.find_board(), device.Board(BOARD_B, "COM7"))

    def test_the_lock_cli_stores_the_board_as_every_record_spells_it(self):
        subprocess.run([sys.executable, str(DEVICE / "device_lock.py"), "--root",
                        str(self.store.root), "--board", " " + BOARD_A.lower() + " ",
                        "acquire", "--owner", "a", "--purpose", "p"],
                       capture_output=True, check=True)
        records = [self.store.read_json(path) for path in self.store.root.glob("*.json")]
        self.assertEqual([record["board"] for record in records], [BOARD_A])

    def test_resolve_port_honours_board_without_autana_board(self):
        with plugged(usb(BOARD_A, "COM5"), usb(BOARD_B, "COM7")), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(device.main(["--board", BOARD_B, "resolve-port"]), 0)
        self.assertEqual(output.getvalue(), "COM7\n")

    def test_boards_include_one_known_only_from_a_waiters_ticket(self):
        self.store.enqueue(BOARD_B, "bob", "look")
        self.assertEqual(self.store.boards(), [BOARD_B])

    def main(self, *argv):
        with mock.patch.object(device_lock, "default_root", return_value=self.store.root), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            code = device.main(list(argv))
        return code, errors.getvalue()

    def test_a_waiter_queues_while_the_board_is_off_usb(self):
        self.store.acquire(BOARD_A, "alice", "flash", kind="flash")
        with plugged():
            self.assertEqual(device.board_for_lock(self.store), BOARD_A)
            waiting = mock.Mock(wraps=self.store.enqueue)
            with mock.patch.object(device_lock.LockStore, "enqueue", waiting):
                code, errors = self.main("--wait", "0.2", "--owner", "bob", "send", "TUNE")
        self.assertEqual(code, 1)
        self.assertIn("waiting for board: queue place 1", errors)
        self.assertTrue(errors.endswith("device: device lock was not acquired\n"), errors)
        self.assertEqual(waiting.call_args.args[:2], (BOARD_A, "bob"))

    def test_take_back_works_while_the_board_is_unplugged(self):
        self.store.set_human(BOARD_A, "maintainer", "bench")
        with plugged():
            self.assertEqual(self.main("take-back")[0], 0)
        self.assertIsNone(self.store.status(BOARD_A)["human"])

    def test_with_several_boards_known_and_none_plugged_the_choice_fails(self):
        self.store.set_human(BOARD_A, "maintainer", "bench")
        self.store.enqueue(BOARD_B, "bob", "look")
        with plugged(), self.assertRaises(RuntimeError) as caught:
            device.board_for_lock(self.store)
        self.assertIn(BOARD_A + ", " + BOARD_B, str(caught.exception))
        with plugged(usb(BOARD_B, "COM7")):
            self.assertEqual(device.board_for_lock(self.store), BOARD_B)


if __name__ == "__main__":
    unittest.main()
