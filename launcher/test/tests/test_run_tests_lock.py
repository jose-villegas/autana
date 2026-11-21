"""run_tests.sh takes one run at a time per build directory.

    python -m unittest discover -s launcher/test/tests -p test_run_tests_lock.py

Two runs sharing a build dir shared its binary and host_run.log, so one run's
result lines landed in the other's log and its check reported a test name
twice. Each test holds a scratch directory's lock here, or in a child it can
kill, and starts the real script or build_lock.py against it.
"""
import contextlib
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parents[1]
REPO = TEST_DIR.parents[1]
sys.path.insert(0, str(REPO / "launcher" / "tools" / "build"))
sys.path.insert(0, str(REPO / "scripts" / "lib"))
sys.path.insert(0, str(REPO / "scripts" / "device"))
sys.path.insert(0, str(REPO / "scripts" / "device" / "tests"))
import build_lock  # noqa: E402
from process_tree import launch_process_tree, stop_process_tree  # noqa: E402
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402

LOCK_TOOL = REPO / "launcher" / "tools" / "build" / "build_lock.py"
# How long a step may take on a loaded machine before the test calls it stuck.
SETTLE_SECONDS = 120
EXIT_STATUS = 3
# The label quiet_run writes before the inner run's own lines.
INNER_LABEL = "== host-tests ==\n"
# Logs a run writes in its build dir, which a waiting run must leave alone.
RUN_LOGS = ("run_tests.log", "host_run.log")


class BuildDirLockTest(unittest.TestCase):
    def setUp(self):
        self.build = Path(tempfile.mkdtemp(prefix="host-lock-"))
        self.addCleanup(shutil.rmtree, self.build, ignore_errors=True)
        self.holding = contextlib.ExitStack()
        self.addCleanup(self.holding.close)
        self.holding.enter_context(build_lock.hold(self.build))

    def start(self, command, env=None):
        # As one tree: Git Bash runs the native Python it execs as a child, which a plain kill would orphan.
        process = launch_process_tree(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop, process)
        return process

    def stop(self, process):
        stop_process_tree(process)
        return process.communicate()

    def run_tests(self, *flags, env=None):
        return self.start([device.git_bash(), str(TEST_DIR / "run_tests.sh"), "--build-only",
                           "--build-dir", str(self.build), *flags], env=env)

    def wait_for_waiting(self, process, holder=os.getpid()):
        """Fails unless the process's first stderr line says it waits for `holder`."""
        lines = queue.Queue()
        threading.Thread(target=lambda: lines.put(process.stderr.readline()), daemon=True).start()
        try:
            line = lines.get(timeout=SETTLE_SECONDS).strip()
        except queue.Empty:
            self.fail("the second run never said it waits")
        prefix, suffix = build_lock.waiting_line(""), f" (held by pid {holder})"
        self.assertTrue(line.startswith(prefix) and line.endswith(suffix), line)
        # Git Bash hands a native Python the directory as C:/..., so compare the folder, not its spelling.
        self.assertTrue(os.path.samefile(line[len(prefix):-len(suffix)], self.build), line)

    def snapshot(self):
        """The build dir's files and contents; the lock file by name only, as
        Windows refuses reads of its locked byte."""
        return {path.name: None if path.name == build_lock.BUILD_LOCK else path.read_bytes()
                for path in self.build.iterdir()}

    def assert_waits_and_touches_nothing(self, process):
        before = self.snapshot()
        self.wait_for_waiting(process)
        time.sleep(1)
        self.assertIsNone(process.poll(), "the second run went ahead while the first held the dir")
        self.assertEqual(before, self.snapshot())

    def wait_for_inner_run(self, process):
        """The run's log once the inner run has written to it, or as it stood
        when SETTLE_SECONDS ran out or the run ended."""
        log = self.build / "run_tests.log"
        deadline = time.monotonic() + SETTLE_SECONDS
        while True:
            text = log.read_text(errors="replace") if log.exists() else ""
            if text.partition(INNER_LABEL)[2].strip() or time.monotonic() > deadline or process.poll() is not None:
                return text
            time.sleep(0.5)

    def test_a_command_waits_for_the_holder_then_runs_with_its_status(self):
        process = self.start([sys.executable, str(LOCK_TOOL), str(self.build), sys.executable, "-c",
                              f"print('ran'); raise SystemExit({EXIT_STATUS})"])
        self.assert_waits_and_touches_nothing(process)
        self.holding.close()
        out, _ = process.communicate(timeout=SETTLE_SECONDS)
        self.assertEqual("ran\n", out)
        self.assertEqual(EXIT_STATUS, process.returncode)

    def test_a_holder_killed_mid_run_leaves_no_lock(self):
        self.holding.close()
        holder = self.start([sys.executable, str(LOCK_TOOL), str(self.build), sys.executable, "-c",
                             "import time; print('holding', flush=True); time.sleep(600)"])
        self.assertEqual("holding\n", holder.stdout.readline())
        with open(self.build / build_lock.BUILD_LOCK, "r+b") as probe:
            self.assertFalse(build_lock.try_lock_file(probe), "the live holder lost its lock")
        self.stop(holder)
        with open(self.build / build_lock.BUILD_LOCK, "r+b") as probe:
            self.assertTrue(build_lock.try_lock_file(probe), "a killed holder left its lock behind")

    def test_a_run_waits_and_leaves_the_holders_logs_alone(self):
        for name in RUN_LOGS:
            (self.build / name).write_text(f"the holder's {name}\n")
        self.assert_waits_and_touches_nothing(self.run_tests())

    def test_a_sanitized_run_waits_too(self):
        self.assert_waits_and_touches_nothing(self.run_tests("--sanitize"))

    def test_the_inner_entry_the_tool_tests_use_waits_too(self):
        self.assert_waits_and_touches_nothing(self.run_tests(env=dict(os.environ, QUIET_INNER="1")))

    def test_a_held_mark_for_another_dir_does_not_skip_the_wait(self):
        other = Path(tempfile.mkdtemp(prefix="host-lock-other-"))
        self.addCleanup(shutil.rmtree, other, ignore_errors=True)
        self.assert_waits_and_touches_nothing(self.run_tests(env=dict(os.environ, HOST_TESTS_HOLD=str(other))))

    def test_a_waiting_run_goes_ahead_once_released_and_never_waits_on_itself(self):
        """The run re-enters itself under the lock, and so does the inner run
        it starts: once the holder lets go they run, neither waiting for the
        lock its own parent holds."""
        process = self.run_tests()
        self.wait_for_waiting(process)
        self.holding.close()
        text = self.wait_for_inner_run(process)
        _, err = self.stop(process)
        self.assertTrue(text.partition(INNER_LABEL)[2].strip(), "the inner run never started")
        self.assertNotIn(build_lock.waiting_line(""), text + err)


if __name__ == "__main__":
    unittest.main()
