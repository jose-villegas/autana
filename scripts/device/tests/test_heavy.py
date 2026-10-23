"""Host work shares the board lock protocol without taking a board."""
import isolation
import contextlib
import io
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device
import device_lock as locks
import autana_config


class HeavyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {"_AUTANA_HEAVY_LOCK_ROOT": str(self.root)})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.store = locks.LockStore(self.root, is_alive=lambda pid: pid == os.getpid())

    def test_board_observer_ignores_heavy_waiters_and_expired_holders(self):
        heavy = self.store.acquire(locks.HEAVY, "gpu", "build")
        self.assertFalse(self.store.any_board_held())
        self.store.release(locks.HEAVY, heavy["token"])
        self.store.enqueue("queued", "waiter", "suite")
        self.assertFalse(self.store.any_board_held())
        held = self.store.acquire("BOARD", "night", "suite")
        before = self.store.lock_path("BOARD").read_bytes()
        self.assertTrue(self.store.any_board_held())
        self.assertEqual(self.store.lock_path("BOARD").read_bytes(), before)
        self.store.release("BOARD", held["token"])
        self.assertFalse(self.store.any_board_held())
        locks.write_json(self.store.lock_path("BOARD"), dict(held, host="windows", heartbeat_at=0))
        self.assertFalse(self.store.any_board_held())
        locks.write_json(self.store.lock_path("BOARD"), dict(held, kind="human", expires_at=self.store.now() + 60))
        self.assertTrue(self.store.any_board_held())

    def test_wsl_board_observer_reads_windows_temp_store(self):
        with patch.dict(os.environ, {"_AUTANA_DEVICE_LOCK_ROOT": ""}), \
                patch.object(locks, "is_wsl", return_value=True), \
                patch.object(locks.subprocess, "check_output", side_effect=["C:\\Users\\runner\\Temp\r\n", str(self.root)]) as run:
            self.assertEqual(locks.board_read_root(), self.root / "autana-device")
        self.assertIn("GetTempPath", run.call_args_list[0].args[0][-1])
        self.assertEqual(run.call_args_list[1].args[0], ["wslpath", "-u", "C:\\Users\\runner\\Temp"])
        with patch.dict(os.environ, {"_AUTANA_DEVICE_LOCK_ROOT": ""}), \
                patch.object(locks, "is_wsl", return_value=True), \
                patch.object(locks.subprocess, "check_output", side_effect=["missing", str(self.root / "missing")]):
            with self.assertRaisesRegex(OSError, "unreachable"):
                locks.board_read_root()

    def test_stale_foreign_and_legacy_dead_waiters_are_pruned(self):
        for foreign in (True, False):
            with self.subTest(foreign=foreign):
                ticket = self.store.enqueue(locks.HEAVY, "dead", "build", pid=12345)
                path = self.store.queue_dir(locks.HEAVY) / (ticket + ".json")
                record = self.store.read_json(path)
                if foreign:
                    record.update(host="other-os", touched_at=0)
                else:
                    record.pop("host")
                    record["protocol"] = 3
                locks.write_json(path, record)
                self.store.prune_crashed_waiters(locks.HEAVY)
                self.assertEqual(self.store.tickets(locks.HEAVY), [])

    def test_wait_poll_refreshes_ticket_and_interrupt_cancels_it(self):
        held = self.store.acquire(locks.HEAVY, "holder", "render")
        touched = []
        def waiting(ticket):
            record = self.store.read_json(self.store.queue_dir(locks.HEAVY) / (ticket + ".json"))
            touched.append(record["touched_at"])
            if len(touched) == 2:
                raise KeyboardInterrupt()
        try:
            with self.assertRaises(KeyboardInterrupt):
                self.store.acquire(locks.HEAVY, "waiter", "build", wait=10, on_wait=waiting)
            self.assertGreater(touched[1], touched[0])
            self.assertEqual(self.store.tickets(locks.HEAVY), [])
        finally:
            self.store.release(locks.HEAVY, held["token"])

    @unittest.skipIf(os.name == "nt", "Windows SIGTERM bypasses Python handlers")
    def test_sigterm_cancels_waiter_ticket(self):
        held = self.store.acquire(locks.HEAVY, "holder", "render")
        source = ("import os, signal, sys; sys.path.insert(0, sys.argv[1]); import device_lock; "
                  "store = device_lock.LockStore(sys.argv[2]); "
                  "store.acquire('heavy', 'cancelled-ci', 'build', wait=10, "
                  "on_wait=lambda ticket: os.kill(os.getpid(), signal.SIGTERM))")
        try:
            result = subprocess.run([sys.executable, "-c", source, str(Path(locks.__file__).parent), str(self.root)],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.store.tickets(locks.HEAVY), [])
        finally:
            self.store.release(locks.HEAVY, held["token"])

    def test_board_lock_and_status_survive_unavailable_heavy_root(self):
        with patch.object(locks, "heavy_root", side_effect=OSError("missing Windows mount")):
            store = locks.LockStore()
            held = store.acquire("BOARD", "owner", "run")
            store.release("BOARD", held["token"])
            output = io.StringIO()
            with patch.object(device, "plugged_boards", return_value=[]), contextlib.redirect_stdout(output):
                self.assertEqual(device.main(["status"]), 0)
            self.assertIn("heavy: unavailable", output.getvalue())
            self.assertIn("missing Windows mount", output.getvalue())

    def test_shared_clock_ignores_wall_clock_skew_for_holder_waiter_and_guard(self):
        store = locks.LockStore(now=lambda: 0, is_alive=lambda pid: True)
        before = self.root.stat().st_mtime
        ticket = store.enqueue(locks.HEAVY, "owner", "build")
        record = store.tickets(locks.HEAVY)[0]
        self.assertGreaterEqual(record["created_at"], before)
        self.assertGreaterEqual(record["touched_at"], before)
        held = store.claim(locks.HEAVY, ticket)
        try:
            self.assertGreaterEqual(held["acquired_at"], before)
            self.assertGreaterEqual(held["heartbeat_at"], before)
            for skew in (-86400, 86400):
                store.now = lambda: held["heartbeat_at"] + skew
                self.assertEqual(store.reclaim_reason(dict(held, host="other-os"), locks.DEFAULT_STALE_SECONDS), "")
                self.assertTrue(store.heartbeat(locks.HEAVY, held["token"]))
                ticket = store.enqueue(locks.HEAVY, "foreign", "build")
                path = store.queue_dir(locks.HEAVY) / (ticket + ".json")
                record = store.read_json(path)
                record["host"] = "other-os"
                locks.write_json(path, record)
                store.prune_crashed_waiters(locks.HEAVY)
                self.assertEqual([item["ticket"] for item in store.tickets(locks.HEAVY)], [ticket])
                store.cancel(locks.HEAVY, ticket)
                path = store.guard_path(locks.HEAVY)
                path.touch()
                with patch.object(locks.time, "sleep", side_effect=KeyboardInterrupt):
                    with self.assertRaises(KeyboardInterrupt), store.guard(locks.HEAVY):
                        self.fail("live guard was removed by wall-clock skew")
                path.unlink()
        finally:
            store.release(locks.HEAVY, held["token"])

    def test_foreign_holder_and_waiter_survive_dead_pid_check_and_fifo(self):
        with patch.object(locks, "host_identity", return_value="machine"):
            holder = self.store.acquire(locks.HEAVY, "windows", "run")
            holder["pid"] = 12345
            locks.write_json(self.store.lock_path(locks.HEAVY), holder)
            first = self.store.enqueue(locks.HEAVY, "windows-next", "build", pid=12345)
        with patch.object(locks, "host_identity", return_value="machine/wsl"):
            second = self.store.enqueue(locks.HEAVY, "wsl", "build")
            self.assertIsNone(self.store.claim(locks.HEAVY, second))
            self.assertEqual([t["ticket"] for t in self.store.tickets(locks.HEAVY)], [first, second])
            self.assertEqual(self.store.reclaim_reason(holder, 600), "")
            self.assertEqual(self.store.reclaim_reason(dict(holder, kind="hold", until_released=True), 600), "")
            self.store.release(locks.HEAVY, holder["token"])
            self.assertIsNone(self.store.claim(locks.HEAVY, second))
        with patch.object(locks, "host_identity", return_value="machine"):
            self.store.is_alive = lambda pid: True
            claimed = self.store.claim(locks.HEAVY, first)
            self.assertEqual(claimed["owner"], "windows-next")

    def test_stale_foreign_holder_and_dead_local_holder_reclaimed(self):
        now = [1000]
        self.store.resource_now = lambda board: now[0]
        with patch.object(locks, "host_identity", return_value="machine"):
            held = self.store.acquire(locks.HEAVY, "windows", "build")
        with patch.object(locks, "host_identity", return_value="machine/wsl"):
            now[0] += locks.DEFAULT_STALE_SECONDS + 1
            self.assertEqual(self.store.reclaim_reason(held, locks.DEFAULT_STALE_SECONDS), "heartbeat expiry")
            replacement = self.store.acquire(locks.HEAVY, "wsl", "build")
            self.assertIsNotNone(replacement)
            replacement["pid"] = 12345
            self.assertEqual(self.store.reclaim_reason(replacement, locks.DEFAULT_STALE_SECONDS), "dead process")

    def test_direct_heavy_acquisition_rejects_board_without_an_exported_token(self):
        heavy = self.store.acquire(locks.HEAVY, "owner", "build")
        try:
            with self.assertRaisesRegex(RuntimeError, "heavy.*board"):
                self.store.acquire("BOARD", "owner", "run")
        finally:
            self.store.release(locks.HEAVY, heavy["token"])

    def test_heavy_scope_reuses_token_and_rejects_board_acquisition(self):
        with device.HeldLock(self.store, locks.HEAVY, "outer", "run", 0) as outer:
            with device.HeldLock(self.store, locks.HEAVY, "nested", "build", 0) as nested:
                self.assertTrue(nested.borrowed)
                self.assertEqual(nested.held["token"], outer.held["token"])
            with self.assertRaisesRegex(RuntimeError, "heavy.*board"):
                device.HeldLock(self.store, "BOARD", "nested", "run", 0)
            with self.assertRaisesRegex(RuntimeError, "heavy.*board"):
                self.store.acquire("BOARD", "nested", "take")
        self.assertIsNone(self.store.status(locks.HEAVY)["lock"])

    def test_board_then_heavy_keeps_both_tokens_and_releases_only_heavy(self):
        with device.HeldLock(self.store, "BOARD", "outer", "run", 0) as board:
            with device.build_policy({}):
                self.assertEqual(os.environ[autana_config.TOKEN_ENV], board.held["token"])
                self.assertIsNotNone(self.store.status(locks.HEAVY)["lock"])
            self.assertIsNone(self.store.status(locks.HEAVY)["lock"])
            self.assertTrue(self.store.check_token("BOARD", board.held["token"]))

    def test_flash_compile_wait_timeout_returns_busy(self):
        with patch.object(device, "board_for_lock", return_value=("BOARD", None)), \
                patch.object(device, "build_image", side_effect=TimeoutError("heavy wait expired")):
            self.assertEqual(device.main(["flash", "--variant", "diag", "--worktree", str(self.root)]), autana_config.EXIT_BUSY)

    def test_build_policy_holds_only_compile_and_releases_on_error(self):
        with self.assertRaisesRegex(ValueError, "compile"):
            with device.build_policy({}):
                self.assertIsNotNone(self.store.status(locks.HEAVY)["lock"])
                raise ValueError("compile")
        self.assertIsNone(self.store.status(locks.HEAVY)["lock"])

    def test_status_reports_heavy_without_a_board(self):
        output = io.StringIO()
        with patch.object(device, "plugged_boards", return_value=[]), contextlib.redirect_stdout(output):
            self.assertEqual(device.main(["status"]), 0)
        self.assertIn("heavy:", output.getvalue())

    def test_cli_heavy_hold_exports_reusable_token_without_board_discovery(self):
        child = [sys.executable, "-c", "import os; print(os.environ['" + autana_config.HEAVY_TOKEN_ENV + "'])"]
        with patch.object(device, "board_for_lock", side_effect=AssertionError("board discovery")), \
                patch.object(device, "run_to_end") as run:
            def nested(command, lost):
                token = os.environ[autana_config.HEAVY_TOKEN_ENV]
                with device.build_policy({}):
                    self.assertEqual(os.environ[autana_config.HEAVY_TOKEN_ENV], token)
            run.side_effect = nested
            self.assertEqual(device.main(["run", "--heavy", "--wait", "0", "--", *child]), 0)
        self.assertIsNone(self.store.status(locks.HEAVY)["lock"])

    def test_status_names_holder_command_since_and_waiters(self):
        with device.HeldLock(self.store, locks.HEAVY, "render-owner", "render", 0):
            self.store.enqueue(locks.HEAVY, "build-waiter", "build")
            output = io.StringIO()
            with patch.object(device, "plugged_boards", return_value=[]), contextlib.redirect_stdout(output):
                self.assertEqual(device.main(["status"]), 0)
            text = output.getvalue()
            for value in ("heavy: held by render-owner", "since", "device.py render", "build-waiter"):
                self.assertIn(value, text)

    def test_busy_notice_names_the_heavy_holder(self):
        held = self.store.acquire(locks.HEAVY, "render-owner", "render")
        notice = locks.busy_text(self.store.status(locks.HEAVY), self.store.now())
        self.assertEqual(notice, f"heavy held by render-owner for {held['purpose']} since "
                                f"{locks.local_time(held['acquired_at'])}; `autana status` shows the queue")

    def test_windows_and_reachable_wsl_root(self):
        native_path = type(self.root)
        with patch.object(locks, "Path", native_path), patch.dict(os.environ, {"ProgramData": str(self.root)}, clear=True), \
                patch.object(locks.os, "name", "nt"):
            self.assertEqual(locks.heavy_root(), self.root / "autana" / locks.HEAVY)
        with patch.object(locks, "Path", native_path), patch.dict(os.environ, {}, clear=True), \
                patch.object(locks.os, "name", "posix"), patch.object(locks, "is_wsl", return_value=True), \
                patch.object(native_path, "is_dir", return_value=True):
            self.assertEqual(locks.heavy_root(), native_path("/mnt/c/ProgramData/autana/heavy"))

    def test_root_platforms_and_host_identity(self):
        native_path = type(self.root)
        with patch.object(locks, "Path", native_path), patch.object(locks.os, "name", "posix"), \
                patch.dict(os.environ, {}, clear=True), patch.object(locks, "is_wsl", return_value=False):
            self.assertEqual(locks.heavy_root(), native_path("/tmp/autana-heavy"))
        with patch.object(locks, "Path", native_path), patch.object(locks.os, "name", "posix"), \
                patch.dict(os.environ, {}, clear=True), patch.object(locks, "is_wsl", return_value=True), \
                patch.object(native_path, "is_dir", return_value=False):
            with self.assertRaisesRegex(OSError, "WSL"):
                locks.heavy_root()
        with patch.object(locks.socket, "gethostname", return_value="machine"), \
                patch.object(locks, "is_wsl", return_value=True):
            self.assertEqual(locks.host_identity(), "machine/wsl")
