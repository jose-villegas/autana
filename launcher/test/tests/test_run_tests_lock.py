"""run_tests.sh takes one run at a time per build directory.

    python -m unittest discover -s launcher/test/tests -p test_run_tests_lock.py

Two runs sharing a build dir shared its binary and host_run.log, so one run's
result lines landed in the other's log and its check reported a test name
twice. Each test holds a scratch directory's lock here and starts the real
script or build_lock.py against it.
"""
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
# Long enough for the script to reach its lock on a loaded machine.
SETTLE_SECONDS = 120
EXIT_STATUS = 3


class BuildDirLockTest(unittest.TestCase):
    def setUp(self):
        self.build = Path(tempfile.mkdtemp(prefix="host-lock-"))
        self.addCleanup(shutil.rmtree, self.build, ignore_errors=True)
        self.held = open(self.build / build_lock.BUILD_LOCK, "a+b")
        self.addCleanup(self.held.close)
        self.assertTrue(build_lock.try_lock_file(self.held))

    def start(self, command, env=None):
        # As one tree: Git Bash runs the native Python it execs as a child, which a plain kill would orphan.
        process = launch_process_tree(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop, process)
        return process

    def stop(self, process):
        stop_process_tree(process)
        return process.communicate()

    def wait_for_waiting(self, process):
        """Fails unless the process's first stderr line says it waits."""
        lines = queue.Queue()
        threading.Thread(target=lambda: lines.put(process.stderr.readline()), daemon=True).start()
        try:
            line = lines.get(timeout=SETTLE_SECONDS).strip()
        except queue.Empty:
            self.fail("the second run never said it waits")
        prefix = build_lock.waiting_line("")
        self.assertTrue(line.startswith(prefix), line)
        # Git Bash hands a native Python the directory as C:/..., so compare the folder, not its spelling.
        self.assertTrue(os.path.samefile(line[len(prefix):], self.build), line)

    def assert_waits_and_touches_nothing(self, process):
        self.wait_for_waiting(process)
        time.sleep(1)
        self.assertIsNone(process.poll(), "the second run went ahead while the first held the dir")
        self.assertEqual([build_lock.BUILD_LOCK], sorted(path.name for path in self.build.iterdir()))

    def test_a_command_waits_for_the_holder_then_runs_with_its_status(self):
        process = self.start([sys.executable, str(LOCK_TOOL), str(self.build), sys.executable, "-c",
                              f"print('ran'); raise SystemExit({EXIT_STATUS})"])
        self.assert_waits_and_touches_nothing(process)
        self.held.close()
        out, _ = process.communicate(timeout=SETTLE_SECONDS)
        self.assertEqual("ran\n", out)
        self.assertEqual(EXIT_STATUS, process.returncode)

    def test_a_run_waits_while_another_holds_its_build_dir(self):
        process = self.start([device.git_bash(), str(TEST_DIR / "run_tests.sh"), "--build-only",
                              "--build-dir", str(self.build)])
        self.assert_waits_and_touches_nothing(process)

    def test_a_run_alone_in_its_build_dir_never_waits_on_itself(self):
        """The run re-enters itself under the lock, and so does the inner run
        it starts; neither may wait for the lock its own parent holds."""
        self.held.close()
        process = self.start([device.git_bash(), str(TEST_DIR / "run_tests.sh"), "--build-only",
                              "--build-dir", str(self.build)])
        log = self.build / "run_tests.log"
        deadline = time.monotonic() + SETTLE_SECONDS
        # The inner run's own lines follow the label quiet_run writes for it.
        while time.monotonic() < deadline and process.poll() is None:
            text = log.read_text(errors="replace") if log.exists() else ""
            if text.partition("== host-tests ==\n")[2].strip():
                break
            time.sleep(0.5)
        _, err = self.stop(process)
        text = log.read_text(errors="replace") if log.exists() else ""
        self.assertTrue(text.partition("== host-tests ==\n")[2].strip(), "the inner run never started")
        self.assertNotIn(build_lock.waiting_line(""), text + err)

    def test_the_inner_entry_the_tool_tests_use_waits_too(self):
        process = self.start([device.git_bash(), str(TEST_DIR / "run_tests.sh"), "--build-only",
                              "--build-dir", str(self.build)], env=dict(os.environ, QUIET_INNER="1"))
        self.assert_waits_and_touches_nothing(process)


if __name__ == "__main__":
    unittest.main()
