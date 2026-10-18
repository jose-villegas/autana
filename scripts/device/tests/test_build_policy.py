"""Compilation policy is shared by build-only and snapshot callers."""
import isolation
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device
import device_lock
import autana_config
import fake_flash


class BuildPolicyTests(unittest.TestCase):
    def test_both_owners_apply_priority_and_release_before_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = fake_flash.worktree(root / "engine")
            serial = root / "serial"
            env = {autana_config.BUILD_SERIAL_ENV: str(serial),
                   autana_config.BUILD_PRIORITY_ENV: autana_config.BUILD_PRIORITY_BELOW_NORMAL,
                   autana_config.BUILD_LATEST_START_ENV: ""}
            def assert_locked(command, lost=None, **options):
                with (serial / device.BUILD_LOCK).open("a+b") as handle:
                    self.assertFalse(device.try_lock_file(handle))
                if os.name == "nt":
                    self.assertTrue(options["creationflags"] & subprocess.BELOW_NORMAL_PRIORITY_CLASS)
                else:
                    self.assertIs(options["preexec_fn"].func, os.nice)
                    self.assertEqual(options["preexec_fn"].args, (device.BUILD_NICE_INCREMENT,))
                    self.assertGreater(device.BUILD_NICE_INCREMENT, 0)
            def snapshot(*args):
                with (serial / device.BUILD_LOCK).open("a+b") as handle:
                    self.assertTrue(device.try_lock_file(handle))
                return "snapshot"
            with patch.dict(os.environ, env), patch.object(device, "run_to_end", side_effect=assert_locked) as run, \
                    patch.object(device, "snapshot_image", side_effect=snapshot):
                self.assertEqual(device.build_worktree(worktree, "diag"), 0)
                self.assertEqual(device.build_snapshot(["build"], root / "build", root / "image"), "snapshot")
                self.assertEqual(run.call_count, 2)

    def test_lock_wait_is_bounded_and_budget_rechecked_after_acquisition(self):
        with tempfile.TemporaryDirectory() as directory:
            env = {autana_config.BUILD_SERIAL_ENV: directory,
                   autana_config.BUILD_LATEST_START_ENV: "100"}
            for acquired in (False, True):
                with self.subTest(acquired=acquired), patch.object(device.time, "time", side_effect=[90, 101]), \
                        patch.object(device, "try_lock_file", return_value=acquired), \
                        patch.object(device.time, "monotonic", side_effect=[0, 11]), \
                        self.assertRaises(TimeoutError):
                    with device.build_policy(env):
                        self.fail("expired policy entered compilation")
            with patch.object(device.time, "time", return_value=100):
                with device.build_policy(env):
                    pass

    def test_exception_releases_mutex_and_default_policy_is_empty(self):
        with tempfile.TemporaryDirectory() as directory:
            env = {autana_config.BUILD_SERIAL_ENV: directory}
            with self.assertRaises(RuntimeError):
                with device.build_policy(env):
                    raise RuntimeError("build failed")
            with device.build_policy(env):
                pass
            with device.build_policy({}) as options:
                self.assertEqual(options, {})

    def test_compilers_serialize_across_snapshot_and_build_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = fake_flash.worktree(root / "engine")
            entered, waiting = threading.Event(), threading.Event()
            errors = []
            original = device.try_lock_file
            def lock(handle):
                acquired = original(handle)
                if not acquired:
                    waiting.set()
                return acquired
            def compile(command, *args, **kwargs):
                if command == ["snapshot"]:
                    entered.set()
                    self.assertTrue(waiting.wait(5))
            def call(function):
                try:
                    function()
                except BaseException as error:
                    errors.append(error)
            with patch.dict(os.environ, {autana_config.BUILD_SERIAL_ENV: str(root / "serial"),
                                        autana_config.BUILD_LATEST_START_ENV: ""}), \
                    patch.object(device, "try_lock_file", side_effect=lock), \
                    patch.object(device, "run_to_end", side_effect=compile), \
                    patch.object(device, "snapshot_image", return_value="id"):
                first = threading.Thread(target=call, args=(lambda: device.build_snapshot(["snapshot"], root / "build", root / "image"),))
                second = threading.Thread(target=call, args=(lambda: device.build_worktree(worktree, "diag"),))
                first.start()
                ready = entered.wait(5)
                second.start()
                first.join(10)
                second.join(10)
                self.assertTrue(ready)
                self.assertFalse(first.is_alive() or second.is_alive())
            self.assertEqual(errors, [])

    def test_posix_priority_increases_niceness(self):
        with patch.object(device.os, "name", "posix"), patch.object(device.os, "nice", create=True) as nice:
            with device.build_policy({autana_config.BUILD_PRIORITY_ENV: autana_config.BUILD_PRIORITY_BELOW_NORMAL}) as options:
                self.assertIs(options["preexec_fn"].func, nice)
                self.assertEqual(options["preexec_fn"].args, (device.BUILD_NICE_INCREMENT,))
                options["preexec_fn"]()
                nice.assert_called_once_with(device.BUILD_NICE_INCREMENT)
                self.assertGreater(options["preexec_fn"].args[0], 0)

    def test_json_indentation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "record.json"
            device_lock.write_json(path, {"row": {"value": 1}}, indent=2)
            self.assertIn('\n  "row": {\n    "value": 1\n  }', path.read_text())


if __name__ == "__main__":
    unittest.main()
