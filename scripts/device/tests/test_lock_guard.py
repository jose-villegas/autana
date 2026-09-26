"""The board lock as every command meets it: boards found by USB serial
number, the guard on each port primitive, what a lost lock stops, the flash
hand-off to build_flash.sh, and the durations status estimates from."""

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
                                   "--root", str(self.store.root), "--board", BOARD_A.lower(),
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


class FlashTests(Store):
    def flash(self, build, store=None, variant="dev"):
        store = store or self.store
        worktree = self.root / "engine"
        (worktree / "launcher" / "tools" / "build").mkdir(parents=True, exist_ok=True)
        (worktree / "launcher" / "tools" / "build" / "build_flash.sh").write_text("")
        args = Namespace(owner="agent", purpose="flash", wait=0, variant=variant,
                         worktree=str(worktree), out=None)
        with mock.patch.object(device, "open_when_free", return_value=contextlib.nullcontext()), \
                mock.patch.object(device, "run_while_held", side_effect=build), \
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

    def test_a_heartbeat_lost_during_the_build_fails_before_the_flash_is_claimed(self):
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
        script = worktree / "launcher" / "tools" / "build" / "build_flash.sh"
        script.parent.mkdir(parents=True)
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
        self.assertIsNotNone(children[0].poll())


class BuildFlashScriptTests(unittest.TestCase):
    """The real build_flash.sh, copied beside the files it sources, with
    idf.py and pyserial stubbed: a token that is not the board's live lock
    stops it after the build and before any flash."""

    def setUp(self):
        try:
            self.bash = device.git_bash()
        except RuntimeError as error:
            self.skipTest(str(error))
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.tree = Path(temp.name)
        for relative in ("launcher/tools/build/build_flash.sh", "launcher/tools/build/idf.sh",
                         "launcher/tools/build/idf_variant.sh", "launcher/tools/build/idf_shim.bat",
                         "launcher/tools/build/espressif.py", "scripts/quiet.sh",
                         "scripts/device/device.py", "scripts/device/device_lock.py",
                         "scripts/device/device_hook.py", "scripts/device/device_report.py"):
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
            "import sys\nfrom pathlib import Path\n"
            "args = sys.argv[1:]\n"
            f"with open({str(self.log)!r}, 'a') as log:\n"
            "    log.write(' '.join(args) + '\\n')\n"
            "if 'build' in args:\n"
            "    build = Path(args[args.index('-B') + 1])\n"
            "    build.mkdir(parents=True, exist_ok=True)\n"
            "    (build / 'sdkconfig').write_text('CONFIG_LAUNCHER_RELEASE=y\\n')\n"
            "    (build / 'launcher.bin').write_bytes(b'')\n"
            "    (build / 'build_id.txt').write_text('stub-build\\n')\n")
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
        self.store = device_lock.LockStore()

    def run_script(self, token):
        environment = dict(os.environ, AUTANA_DEVICE_LOCK_TOKEN=token, AUTANA_BOARD=BOARD_A,
                           PYTHONPATH=str(self.stubs),
                           PATH=str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"])
        return subprocess.run([self.bash, str(self.tree / "launcher/tools/build/build_flash.sh"),
                               str(self.export)], env=environment, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=120)

    def idf_calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_a_foreign_token_stops_after_the_build_and_before_the_flash(self):
        held = self.store.acquire(BOARD_A, "agent", "flash")
        self.addCleanup(self.store.release, BOARD_A, held["token"])
        result = self.run_script("foreign")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("device lock token is not active for board " + BOARD_A, result.stderr)
        self.assertTrue(any(call.endswith(" build") for call in self.idf_calls()),
                        result.stdout[-2000:])
        self.assertFalse(any("flash" in call.split() for call in self.idf_calls()))

    def test_the_live_token_flashes_the_port_the_board_has_now(self):
        held = self.store.acquire(BOARD_A, "agent", "flash")
        self.addCleanup(self.store.release, BOARD_A, held["token"])
        result = self.run_script(held["token"])
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])
        self.assertIn("BUILD_ID=stub-build", result.stdout)
        self.assertEqual(self.idf_calls()[-1], "-B build -p COM42 flash")


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

    def test_history_needs_three_successes_and_uses_the_last_thirty(self):
        root = self.store.root
        for seconds in (2, 4):
            device_lock.record_duration("send", seconds, root=root)
        device_lock.record_duration("send", 999, "failed", root=root)
        self.assertEqual(device_lock.duration_history(root), {})
        device_lock.record_duration("send", 6, root=root)
        self.assertEqual(device_lock.duration_history(root), {"send": 4})
        for seconds in range(35):
            device_lock.record_duration("flash", seconds, root=root)
        self.assertEqual(device_lock.duration_history(root)["flash"], 19.5)

    def test_the_file_stays_bounded(self):
        root = self.store.root
        for index in range(device_lock.DURATIONS_TRIM_LINES + 50):
            device_lock.record_duration("flash" if index % 2 else "send", index, root=root)
        lines = (root / device_lock.DURATIONS_FILE).read_text(encoding="utf-8").splitlines()
        self.assertLessEqual(len(lines), device_lock.DURATIONS_TRIM_LINES)
        history = device_lock.duration_history(root)
        self.assertEqual(set(history), {"flash", "send"})
        self.assertGreater(history["flash"], device_lock.DURATIONS_TRIM_LINES - 30)

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


if __name__ == "__main__":
    unittest.main()
