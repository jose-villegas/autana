"""A failed launcher/tools/build/build.sh, launched for real the ways it is
launched, waits for Enter only in a window opened for it alone. Each
command sends the build's output to a file; stdin, the terminal or not,
is what decides the pause.

The build fails at setup (IDF_PATH names no ESP-IDF), on every machine and
in seconds, through the same exit path as a failed compile."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "launcher" / "tools" / "build" / "build.sh"
sys.path.insert(0, str(ROOT / "scripts" / "device"))
sys.path.insert(0, str(ROOT / "scripts" / "device" / "tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402

PAUSE = "Press Enter to close..."
FAILED = "=== FAILED (exit"
CALLER_DONE = "caller-done"
# A failed setup takes seconds; a run that paused never ends on its own.
EXIT_SECONDS = 120
# How long a paused run must still be there after printing the prompt.
PAUSE_HOLD_SECONDS = 1


def wait_for(predicate, seconds=EXIT_SECONDS):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.2)
    return False


class FailedBuildPause(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.dir = Path(temp.name)
        self.log = self.dir / "build.out"
        self.env = dict(os.environ, IDF_PATH=(self.dir / "no-idf").as_posix())

    def start(self, args, **options):
        process = subprocess.Popen(args, env=self.env, **options)
        self.addCleanup(self.kill, process)
        return process

    @staticmethod
    def kill(process):
        if process.poll() is None:
            if os.name == "nt":
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)],
                               capture_output=True, timeout=60)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=60)

    def output(self):
        try:
            return self.log.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def caller_done(self):
        return CALLER_DONE in self.output()

    def assert_exits_without_pause(self, done):
        self.assertTrue(wait_for(done), f"still running after {EXIT_SECONDS} s:\n{self.output()}")
        self.assertIn(FAILED, self.output())
        self.assertNotIn(PAUSE, self.output())

    def assert_pauses(self, alive):
        self.assertTrue(wait_for(lambda: PAUSE in self.output()),
                        f"no pause after {EXIT_SECONDS} s:\n{self.output()}")
        time.sleep(PAUSE_HOLD_SECONDS)
        self.assertTrue(alive(), "printed the pause but did not wait")


@unittest.skipUnless(os.name == "nt", "Windows consoles and mintty")
class WindowsLaunches(FailedBuildPause):
    """Start-Process -WindowStyle Hidden, as a detached queued run is
    launched, gives the run a console it can never read Enter from."""

    @classmethod
    def setUpClass(cls):
        cls.bash = device.git_bash()
        cls.mintty = Path(cls.bash).resolve().parents[1] / "usr" / "bin" / "mintty.exe"

    def hidden_console(self, command):
        info = subprocess.STARTUPINFO()
        info.dwFlags = subprocess.STARTF_USESHOWWINDOW
        info.wShowWindow = 0  # SW_HIDE
        # No handle redirected, so stdin is the new console, a terminal to [ -t 0 ].
        return self.start([self.bash, "-c", command], startupinfo=info,
                          creationflags=subprocess.CREATE_NEW_CONSOLE)

    def in_mintty(self, command):
        """mintty hands its window to a process of its own and exits, so the
        run is followed by its MSYS pid, which exec keeps."""
        if not self.mintty.is_file():
            self.skipTest(f"no mintty beside {self.bash}")
        pid = self.dir / "pid"
        self.start([str(self.mintty), "-w", "hide", "/usr/bin/bash", "-c",
                    f'echo $$ > "{pid.as_posix()}"; {command}'])
        self.assertTrue(wait_for(pid.exists), "mintty never ran the command")
        send = f'kill -{{}} "$(cat "{pid.as_posix()}")" 2>/dev/null'
        self.addCleanup(subprocess.run, [self.bash, "-c", send.format("KILL")], timeout=60)
        return lambda: subprocess.run([self.bash, "-c", send.format(0)], timeout=60).returncode == 0

    def test_detached_script_that_runs_the_build_does_not_pause(self):
        process = self.hidden_console(
            f'bash "{BUILD.as_posix()}" > "{self.log.as_posix()}" 2>&1; echo {CALLER_DONE}')
        self.assert_exits_without_pause(lambda: process.poll() is not None)

    def test_detached_build_with_its_own_hidden_console_does_not_pause(self):
        process = self.hidden_console(f'exec bash "{BUILD.as_posix()}" > "{self.log.as_posix()}" 2>&1')
        self.assert_exits_without_pause(lambda: process.poll() is not None)
        self.assertNotEqual(process.returncode, 0)

    def test_build_run_from_a_shell_in_a_terminal_does_not_pause(self):
        self.in_mintty(f'{{ bash "{BUILD.as_posix()}"; echo {CALLER_DONE}; }} > "{self.log.as_posix()}" 2>&1')
        self.assert_exits_without_pause(self.caller_done)

    def test_build_in_a_window_opened_for_it_pauses(self):
        """What a double-click runs: git-bash.exe opens mintty on the script."""
        self.assert_pauses(self.in_mintty(f'exec bash "{BUILD.as_posix()}" > "{self.log.as_posix()}" 2>&1'))


@unittest.skipIf(os.name == "nt", "Linux terminals")
class LinuxLaunches(FailedBuildPause):
    @classmethod
    def setUpClass(cls):
        cls.script = shutil.which("script")

    def in_terminal(self, command):
        """`script` runs its command as the session leader of a new pty, as a
        terminal emulator does; stdin stays open so a pause would wait."""
        if not self.script:
            self.skipTest("util-linux script not installed")
        process = self.start([self.script, "-qec", command, "/dev/null"], stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, start_new_session=True)
        self.addCleanup(process.stdin.close)
        return process

    def test_build_detached_from_a_terminal_does_not_pause(self):
        """setsid leaves the terminal on stdin, but it already belongs to the
        caller's session: a session leader with no window."""
        self.in_terminal(f'setsid -w bash "{BUILD}" > "{self.log}" 2>&1; echo {CALLER_DONE} >> "{self.log}"')
        self.assert_exits_without_pause(self.caller_done)

    def test_build_run_from_a_shell_in_a_terminal_does_not_pause(self):
        self.in_terminal(f'{{ bash "{BUILD}"; echo {CALLER_DONE}; }} > "{self.log}" 2>&1')
        self.assert_exits_without_pause(self.caller_done)

    def test_build_in_a_terminal_opened_for_it_pauses(self):
        process = self.in_terminal(f'exec bash "{BUILD}" > "{self.log}" 2>&1')
        self.assert_pauses(lambda: process.poll() is None)


if __name__ == "__main__":
    unittest.main()
