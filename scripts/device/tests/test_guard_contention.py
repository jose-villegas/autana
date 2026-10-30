"""The lock's guard file under contention. On Windows, creating or deleting a
file another process is deleting or has open fails with PermissionError (and,
from WSL on a Windows drive, creating it fails with FileNotFoundError), so the
guard and the lock-file write must read that as busy and retry."""

import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import isolation  # noqa: F401  (keeps the suite out of real records)
import device_lock

BOARD = "90:70:69:FE:A3:08"
REAL_OPEN = os.open
REAL_REPLACE = os.replace


def refuse_once(real, error=PermissionError):
    calls = []

    def call(*args, **kwargs):
        calls.append(args)
        if len(calls) == 1:
            raise error(13, "Permission denied")
        return real(*args, **kwargs)
    return call


class GuardRetryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.store = device_lock.LockStore(self.directory.name)

    def test_a_create_refused_as_busy_is_retried(self):
        with mock.patch.object(device_lock.os, "open", side_effect=refuse_once(REAL_OPEN)):
            with self.store.guard(BOARD):
                self.assertTrue(self.store.guard_path(BOARD).exists())
        self.assertFalse(self.store.guard_path(BOARD).exists())

    def test_a_create_refused_as_missing_is_retried(self):
        with mock.patch.object(device_lock.os, "open", side_effect=refuse_once(REAL_OPEN, FileNotFoundError)):
            with self.store.guard(BOARD):
                self.assertTrue(self.store.guard_path(BOARD).exists())

    def test_a_guard_folder_removed_under_a_taker_is_made_again(self):
        folder = self.store.guard_path(BOARD).parent
        calls = []

        def remove_folder_once(*args, **kwargs):
            calls.append(args)
            if len(calls) == 1:
                shutil.rmtree(folder)
            return REAL_OPEN(*args, **kwargs)
        with mock.patch.object(device_lock.os, "open", side_effect=remove_folder_once):
            with self.store.guard(BOARD):
                self.assertTrue(self.store.guard_path(BOARD).exists())

    def test_a_delete_refused_as_busy_is_retried(self):
        with self.store.guard(BOARD):
            path = self.store.guard_path(BOARD)
            real_unlink = Path.unlink
            attempts = []

            def unlink(self_path, *args, **kwargs):
                attempts.append(self_path)
                if len(attempts) == 1:
                    raise PermissionError(13, "Permission denied")
                return real_unlink(self_path, *args, **kwargs)
            patch = mock.patch.object(Path, "unlink", unlink)
            patch.start()
            self.addCleanup(patch.stop)
        patch.stop()
        self.assertEqual(len(attempts), 2)
        self.assertFalse(path.exists())

    def test_a_replace_refused_as_busy_is_retried(self):
        path = Path(self.directory.name) / "lock.json"
        with mock.patch.object(device_lock.os, "replace", side_effect=refuse_once(REAL_REPLACE)):
            device_lock.write_json(path, {"board": BOARD})
        self.assertEqual(self.store.read_json(path), {"board": BOARD})


WORKER = textwrap.dedent("""
    import sys, time
    from pathlib import Path
    sys.path.insert(0, {device!r})
    import device_lock
    store = device_lock.LockStore({root!r})
    role = sys.argv[1]
    deadline = time.monotonic() + {seconds}
    while time.monotonic() < deadline:
        if role == "stat":
            try:
                store.guard_path({board!r}).stat()
            except (FileNotFoundError, PermissionError):
                pass
            store.boards()
        else:
            with store.guard({board!r}):
                device_lock.write_json(store.lock_path({board!r}), {{"board": {board!r}, "by": role}})
""")


class GuardContentionTests(unittest.TestCase):
    """Real processes on the real filesystem: the failure is an OS behaviour,
    so a mock cannot stand in for it."""

    def test_writers_and_a_prober_never_see_permission_errors(self):
        with tempfile.TemporaryDirectory() as root:
            script = Path(root) / "worker.py"
            script.write_text(WORKER.format(device=str(Path(__file__).resolve().parents[1]),
                                            root=str(Path(root) / "locks"), board=BOARD,
                                            seconds=6))
            workers = [subprocess.Popen([sys.executable, str(script), role],
                                        stderr=subprocess.PIPE, text=True)
                       for role in ("a", "b", "c", "stat", "stat")]
            failures = []
            for worker in workers:
                _, stderr = worker.communicate(timeout=60)
                if worker.returncode:
                    failures.append(stderr.strip().splitlines()[-1] if stderr.strip() else "exit "
                                    + str(worker.returncode))
            self.assertEqual(failures, [])

    @unittest.skipUnless(os.name == "nt" and shutil.which("wsl.exe"), "needs Windows with WSL")
    def test_windows_and_wsl_takers_share_a_guard(self):
        with tempfile.TemporaryDirectory() as root:
            script = Path(root) / "worker.py"
            wsl_path = subprocess.run(["wsl.exe", "-e", "wslpath", "-u", str(Path(__file__).resolve().parents[1])],
                                      capture_output=True, text=True).stdout.strip()
            wsl_root = subprocess.run(["wsl.exe", "-e", "wslpath", "-u", root],
                                      capture_output=True, text=True).stdout.strip()
            if not wsl_path or not wsl_root:
                self.skipTest("WSL has no distribution to run in")
            for name, device, locks in (("windows.py", str(Path(__file__).resolve().parents[1]), str(Path(root) / "locks")),
                                        ("wsl.py", wsl_path, wsl_root + "/locks")):
                (Path(root) / name).write_text(WORKER.format(device=device, root=locks, board=BOARD, seconds=6))
            workers = [subprocess.Popen([sys.executable, str(Path(root) / "windows.py"), "windows"],
                                        stderr=subprocess.PIPE, text=True),
                       subprocess.Popen(["wsl.exe", "-e", "python3", wsl_root + "/wsl.py", "wsl"],
                                        stderr=subprocess.PIPE, text=True)]
            failures = []
            for worker in workers:
                _, stderr = worker.communicate(timeout=60)
                if worker.returncode:
                    failures.append(stderr.strip().splitlines()[-1] if stderr.strip() else "exit "
                                    + str(worker.returncode))
            self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
