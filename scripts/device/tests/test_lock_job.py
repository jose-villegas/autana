"""The lock and the serial port are two resources; the port is the real one.
Nothing a lock holder started may keep the port after the holder is gone,
however it went: on Windows the holder joins a kill-on-close job object.

Real processes throughout. The port is stood in for by a localhost TCP port
that one child binds and every other bind is refused. The holders run under
the ESP-IDF venv python.exe, which is itself a launcher that puts the real
interpreter in a job of its own. AUTANA_TEST_DEVICE_DIR points the holders at
another copy of the device scripts, to watch these fail against an older one."""

import isolation  # noqa: F401  (first: keeps the suite out of real records)
import os
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
sys.path.insert(0, str(DEVICE.parents[1] / "launcher" / "tools" / "build"))
BOARD = "90:70:69:FE:A3:08"
WINDOWS = os.name == "nt"

try:
    from espressif import idf_python
    IDF_PYTHON = idf_python() if WINDOWS else sys.executable
except Exception:  # no ESP-IDF install on this machine
    IDF_PYTHON = None

PORT_HOLDER = textwrap.dedent("""
    import socket, sys, time
    sock = socket.socket()
    sock.bind(("127.0.0.1", int(sys.argv[1])))
    sock.listen()
    print("ready", flush=True)
    time.sleep(120)
""")

# Starts the port holder and leaves it behind: its parent is gone before the
# holder command is.
INTERMEDIATE = textwrap.dedent("""
    import subprocess, sys
    child = subprocess.Popen([sys.executable, sys.argv[1], sys.argv[2]], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True)
    child.stdout.readline()
    print("ready", flush=True)
""")

# A command that holds the board under a real HeldLock and starts something
# that owns the port, the way esptool or a monitor would.
HOLDER = textwrap.dedent("""
    import os, subprocess, sys, time
    sys.path.insert(0, {device!r})
    import device, device_lock
    store = device_lock.LockStore({root!r})
    with device.HeldLock(store, {board!r}, "job-test", "job", 0):
        command = [sys.executable] + {command!r}
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 text=True)
        child.stdout.readline()
        try:
            import lock_job
            listed = child.pid in lock_job.members()
        except ImportError:
            listed = False
        print("holder", os.getpid(), child.pid, listed, flush=True)
        time.sleep(float(sys.argv[1]))
""")

# The job this harness or a launcher may already have put a process in: this
# process joins one first, then runs the holder inside it.
OUTER = textwrap.dedent("""
    import subprocess, sys
    sys.path.insert(0, {device!r})
    import device_lock, lock_job
    kernel32 = device_lock.windows_kernel32()
    from ctypes import wintypes
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    outer = lock_job.create_job(kernel32)
    assert kernel32.AssignProcessToJobObject(outer, kernel32.GetCurrentProcess())
    sys.exit(subprocess.call([sys.executable] + sys.argv[1:]))
""")

# Runs at the lock's `released` event and records whether the port was free.
HOOK = textwrap.dedent("""
    import os, socket, sys
    if os.environ["AUTANA_LOCK_EVENT"] == "released":
        sock = socket.socket()
        try:
            sock.bind(("127.0.0.1", int(sys.argv[1])))
            seen = "free"
        except OSError:
            seen = "held"
        open(sys.argv[2], "w").write(seen)
""")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def port_is_free(port):
    with socket.socket() as sock:
        try:
            sock.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def kill_tree(pid):
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


@unittest.skipUnless(WINDOWS and IDF_PYTHON, "the kill-on-close job is Windows-only")
class JobTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.port = free_port()
        for name, text in (("port_holder.py", PORT_HOLDER), ("hook.py", HOOK),
                           ("intermediate.py", INTERMEDIATE)):
            (self.dir / name).write_text(text)
        under_test = os.environ.get("AUTANA_TEST_DEVICE_DIR", str(DEVICE))
        (self.dir / "outer.py").write_text(OUTER.format(device=str(DEVICE)))
        self.holder_script = self.dir / "run.py"
        self.holder_script.write_text(HOLDER.format(
            device=under_test, root=str(self.dir / "locks"), board=BOARD,
            command=[str(self.dir / "port_holder.py"), str(self.port)]))
        self.chain_script = self.dir / "chain.py"
        self.chain_script.write_text(HOLDER.format(
            device=under_test, root=str(self.dir / "locks"), board=BOARD,
            command=[str(self.dir / "intermediate.py"), str(self.dir / "port_holder.py"),
                     str(self.port)]))

    def start(self, script, seconds, wrap=(), **environment):
        command = [IDF_PYTHON, *wrap, str(script), str(seconds)]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=dict(os.environ, **environment))
        self.addCleanup(process.communicate)
        self.addCleanup(process.kill)
        words = process.stdout.readline().split()
        self.assertEqual(words[:1], ["holder"], process.stderr.read() if process.poll() else "")
        holder = int(words[1])
        self.addCleanup(kill_tree, holder)
        return process, holder, words[3] == "True"

    def assert_port_free_within(self, seconds):
        deadline = time.monotonic() + seconds
        while not port_is_free(self.port) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(port_is_free(self.port), "the port is still held")

    def kill_only(self, pid):
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], check=True,
                       stdout=subprocess.DEVNULL)

    def test_killing_only_the_holder_frees_the_port(self):
        _, holder, _ = self.start(self.holder_script, 60)
        self.assertFalse(port_is_free(self.port))
        self.kill_only(holder)
        self.assert_port_free_within(1.0)

    def test_a_grandchild_of_a_parent_that_already_exited_is_stopped_too(self):
        _, holder, _ = self.start(self.chain_script, 60)
        self.assertFalse(port_is_free(self.port))
        self.kill_only(holder)
        self.assert_port_free_within(1.0)

    def test_a_normal_exit_stops_a_lingering_child_before_the_lock_is_released(self):
        seen = self.dir / "seen.txt"
        hook = f'"{sys.executable}" "{self.dir / "hook.py"}" {self.port} "{seen}"'
        process, _, _ = self.start(self.holder_script, 0.3, AUTANA_LOCK_HOOK=hook)
        _, errors = process.communicate(timeout=60)
        self.assertEqual(seen.read_text(), "free", "the lock was released while the port was held")
        self.assertTrue(port_is_free(self.port))
        self.assertIn("stopped process", errors)

    def test_the_holder_joins_its_own_job_inside_another_job_and_its_children_land_in_it(self):
        _, holder, listed = self.start(self.holder_script, 60, wrap=[str(self.dir / "outer.py")])
        self.assertTrue(listed, "the child is not a member of the holder's job")
        self.kill_only(holder)
        self.assert_port_free_within(1.0)


if __name__ == "__main__":
    unittest.main()
