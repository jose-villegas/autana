"""Host work shares the board lock protocol without taking a board."""
import isolation
import contextlib
import io
import os
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
        self.store.now = lambda: now[0]
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
