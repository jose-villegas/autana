"""Compilation policy is shared by build-only and snapshot callers."""
import isolation
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device
import device_lock
import autana_config
import fake_flash


class BuildPolicyTests(unittest.TestCase):
    def test_both_owners_apply_priority_and_hold_through_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = fake_flash.worktree(root / "engine")
            env = {"_AUTANA_HEAVY_LOCK_ROOT": str(root / "heavy"),
                   autana_config.BUILD_PRIORITY_ENV: autana_config.BUILD_PRIORITY_BELOW_NORMAL}
            store = device_lock.LockStore(root / "heavy")

            def assert_locked(command, lost=None, **options):
                self.assertIsNotNone(store.status(device_lock.HEAVY)["lock"])
                self.assertEqual(options["env"][autana_config.HEAVY_TOKEN_ENV],
                                 store.status(device_lock.HEAVY)["lock"]["token"])
                if os.name == "nt":
                    self.assertTrue(options["creationflags"] & subprocess.BELOW_NORMAL_PRIORITY_CLASS)
                else:
                    self.assertIs(options["preexec_fn"].func, os.nice)
                    self.assertEqual(options["preexec_fn"].args, (device.BUILD_NICE_INCREMENT,))

            def snapshot(*args):
                self.assertIsNotNone(store.status(device_lock.HEAVY)["lock"])
                return "snapshot"

            with patch.dict(os.environ, env), patch.object(device, "run_to_end", side_effect=assert_locked) as run, \
                    patch.object(device, "snapshot_image", side_effect=snapshot):
                self.assertEqual(device.build_worktree(worktree, "diag"), 0)
                self.assertEqual(device.build_snapshot(["build"], root / "build", root / "image"), "snapshot")
                self.assertEqual(run.call_count, 2)

    def test_heavy_is_acquired_before_build_directory(self):
        @contextlib.contextmanager
        def directory(*args):
            self.assertIsNotNone(store.status(device_lock.HEAVY)["lock"])
            yield None
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            store = device_lock.LockStore(root)
            with patch.dict(os.environ, {"_AUTANA_HEAVY_LOCK_ROOT": str(root)}), \
                    patch.object(device, "build_directory_lock", side_effect=directory), \
                    patch.object(device, "run_to_end"):
                device.build_snapshot(["build"], root / "build")

    def test_busy_lock_refuses_compilation(self):
        with patch.object(device, "HeldLock", side_effect=device.LockBusy("busy")), \
                self.assertRaises(TimeoutError):
            with device.build_policy({autana_config.WAIT_ENV: "0"}):
                self.fail("busy policy entered compilation")

    def test_exception_releases_mutex_and_default_policy_is_empty(self):
        with self.assertRaisesRegex(RuntimeError, "build failed"):
            with device.build_policy({}):
                raise RuntimeError("build failed")
        with device.build_policy({}) as options:
            self.assertEqual(options, {})

    def test_posix_priority_increases_niceness(self):
        with patch.object(device, "HeldLock", return_value=contextlib.nullcontext()), \
                patch.object(device.os, "name", "posix"), patch.object(device.os, "nice", create=True) as nice:
            with device.build_policy({autana_config.BUILD_PRIORITY_ENV: autana_config.BUILD_PRIORITY_BELOW_NORMAL}) as options:
                self.assertIs(options["preexec_fn"].func, nice)
                options["preexec_fn"]()
                nice.assert_called_once_with(device.BUILD_NICE_INCREMENT)

    def test_queue_timing_is_only_printed_after_a_full_second(self):
        for elapsed in (0, device.BUILD_WAIT_REPORT_MIN_SECONDS - 0.001,
                        device.BUILD_WAIT_REPORT_MIN_SECONDS, device.BUILD_WAIT_REPORT_MIN_SECONDS + 1):
            for busy in (False, True):
                lock = patch.object(device, "HeldLock", side_effect=device.LockBusy("busy")) if busy else \
                    patch.object(device, "HeldLock", return_value=contextlib.nullcontext())
                with self.subTest(elapsed=elapsed, busy=busy), lock, \
                        patch.object(device.time, "monotonic", side_effect=[10, 10 + elapsed]), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    if busy:
                        with self.assertRaises(TimeoutError), device.build_policy({}):
                            self.fail("busy lock entered compilation")
                    else:
                        with device.build_policy({}):
                            pass
                    if elapsed >= device.BUILD_WAIT_REPORT_MIN_SECONDS:
                        self.assertEqual(float(output.getvalue().removeprefix(autana_config.HEAVY_WAIT_PREFIX)), elapsed)
                    else:
                        self.assertEqual(output.getvalue(), "")

    def test_json_indentation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "record.json"
            device_lock.write_json(path, {"row": {"value": 1}}, indent=2)
            self.assertIn('\n  "row": {\n    "value": 1\n  }', path.read_text())


if __name__ == "__main__":
    unittest.main()
