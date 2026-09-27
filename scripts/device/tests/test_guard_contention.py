"""The lock's guard file under contention. On Windows, creating or deleting a
file another process is deleting or has open fails with PermissionError, so
the guard and the lock-file write must read that as busy and retry."""

import os
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
            self.store.write_json(path, {"board": BOARD})
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
                store.write_json(store.lock_path({board!r}), {{"board": {board!r}, "by": role}})
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


if __name__ == "__main__":
    unittest.main()
