"""The lock and the serial port are two resources; the port is the real one.
Nothing a lock holder started may keep the port after the holder is gone,
however it went: on Windows the holder joins a kill-on-close job object, on
Linux what it starts is tagged through the environment and a watchdog kills
the tagged processes if it dies.

Real processes throughout. The port is stood in for by a localhost TCP port
that one child binds and every other bind is refused. On Windows the holders
run under the ESP-IDF venv python.exe, which is itself a launcher that puts
the real interpreter in a job of its own. AUTANA_TEST_DEVICE_DIR points the
holders at another copy of the device scripts, to watch these fail against an
older one."""

import isolation  # (first: keeps the suite out of real records)
import port_guard
import os
import signal
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from unittest import mock

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
sys.path.insert(0, str(DEVICE.parents[1] / "launcher" / "tools" / "build"))
BOARD = "90:70:69:FE:A3:08"
WINDOWS = os.name == "nt"
LINUX = sys.platform.startswith("linux")
PYTHON = sys.executable
if WINDOWS:
    try:
        from espressif import idf_python
        PYTHON = idf_python()
    except Exception:  # no ESP-IDF install on this machine
        PYTHON = None
SUPPORTED = bool(PYTHON) and (WINDOWS or LINUX)

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
            import lock_scope
            listed = child.pid in lock_scope.members()
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

# Two locks in one process, and a child started between them: the second
# lock's release must leave that one alone.
SEQUENCE = textwrap.dedent("""
    import os, subprocess, sys
    sys.path.insert(0, {device!r})
    import device, device_lock
    store = device_lock.LockStore({root!r})
    holder = {holder!r}
    def start(port):
        child = subprocess.Popen([sys.executable, holder, port], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True)
        child.stdout.readline()
        return child.pid
    with device.HeldLock(store, {board!r}, "job-test", "first", 0):
        pass
    before = start(sys.argv[1])
    with device.HeldLock(store, {board!r}, "job-test", "second", 0):
        inside = start(sys.argv[2])
    import socket
    def free(port):
        with socket.socket() as sock:
            try:
                sock.bind(("127.0.0.1", int(port)))
                return True
            except OSError:
                return False
    print("state", free(sys.argv[1]), free(sys.argv[2]), flush=True)
""")

# lock_job on its own, for what one call does.
DIRECT = textwrap.dedent("""
    import os, subprocess, sys
    sys.path.insert(0, {device!r})
    import lock_job
    holder = {holder!r}
    def start(*flags, **options):
        child = subprocess.Popen([sys.executable, holder, sys.argv[2]], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True, **options)
        child.stdout.readline()
        return child
    mode = sys.argv[1]
    if mode == "handle":
        lock_job.enter()
        child = start()
        print("result", lock_job.terminate_member(os.getppid()),
              lock_job.terminate_member(child.pid), flush=True)
    elif mode == "breakaway":
        lock_job.enter()
        print("pid", start(creationflags=0x01000000).pid, flush=True)
    elif mode == "overflow":
        lock_job.MAX_MEMBERS = 2
        lock_job.enter()
        for _ in range(3):
            subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        lock_job.members()
        print("done", flush=True)
""")

# What a normal exit leaves: the token and the watchdog's pid, then the exit.
LEFTOVER = textwrap.dedent("""
    import os, subprocess, sys
    sys.path.insert(0, {device!r})
    import device, device_lock, lock_group
    store = device_lock.LockStore({root!r})
    with device.HeldLock(store, {board!r}, "job-test", "leftover", 0):
        child = subprocess.Popen([sys.executable, {holder!r}, sys.argv[1]], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True)
        child.stdout.readline()
        print("state", os.environ[lock_group.TOKEN_VARIABLE], lock_group._stack[-1][2].pid,
              flush=True)
""")

# A holder whose watchdog dies first must still release, and say so.
NO_WATCHDOG = textwrap.dedent("""
    import sys
    sys.path.insert(0, {device!r})
    import device, device_lock, lock_group
    store = device_lock.LockStore({root!r})
    with device.HeldLock(store, {board!r}, "job-test", "nowatch", 0):
        watchdog = lock_group._stack[-1][2]
        watchdog.kill()
        watchdog.wait()
    print("released", store.status({board!r})["lock"] is None, flush=True)
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


def kill_only(pid):
    """Just `pid`, with no tree: what a killed holder leaves behind."""
    if WINDOWS:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], stdout=subprocess.DEVNULL)
    else:
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def kill_tree(pid):
    if WINDOWS:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        kill_only(pid)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.port = free_port()
        self.ports = [free_port(), free_port()]
        for name, text in (("port_holder.py", PORT_HOLDER), ("hook.py", HOOK),
                           ("intermediate.py", INTERMEDIATE)):
            (self.dir / name).write_text(text)
        # Test only: a copy of the device tools to run these scenarios against.
        under_test = os.environ.get("AUTANA_TEST_DEVICE_DIR", str(DEVICE))
        (self.dir / "outer.py").write_text(OUTER.format(device=str(DEVICE)))
        fields = dict(device=under_test, root=str(self.dir / "locks"), board=BOARD,
                      holder=str(self.dir / "port_holder.py"))
        (self.dir / "sequence.py").write_text(SEQUENCE.format(**fields))
        (self.dir / "direct.py").write_text(DIRECT.format(**fields))
        (self.dir / "leftover.py").write_text(LEFTOVER.format(**fields))
        (self.dir / "nowatch.py").write_text(NO_WATCHDOG.format(**fields))
        holder = str(self.dir / "port_holder.py")
        self.holder_script = self.dir / "run.py"
        self.holder_script.write_text(HOLDER.format(
            device=under_test, root=str(self.dir / "locks"), board=BOARD,
            command=[holder, str(self.port)]))
        self.chain_script = self.dir / "chain.py"
        self.chain_script.write_text(HOLDER.format(
            device=under_test, root=str(self.dir / "locks"), board=BOARD,
            command=[str(self.dir / "intermediate.py"), holder, str(self.port)]))

    def start(self, script, seconds, wrap=(), **environment):
        command = [PYTHON, *wrap, str(script), str(seconds)]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=dict(os.environ, **environment))
        self.addCleanup(process.communicate)
        self.addCleanup(process.kill)
        words = process.stdout.readline().split()
        self.assertEqual(words[:1], ["holder"], process.stderr.read() if process.poll() else "")
        holder = int(words[1])
        self.addCleanup(kill_tree, holder)
        return process, holder, words[3] == "True"

    def run_script(self, name, *args):
        done = subprocess.run([PYTHON, str(self.dir / name), *map(str, args)],
                              capture_output=True, text=True, timeout=60)
        return done.stdout, done.stderr

    def assert_port_free_within(self, seconds):
        deadline = time.monotonic() + seconds
        while not port_is_free(self.port) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(port_is_free(self.port), "the port is still held")


@unittest.skipUnless(SUPPORTED, "needs a job object (Windows) or /proc (Linux)")
class HolderTests(Fixture):
    def test_killing_only_the_holder_frees_the_port(self):
        _, holder, _ = self.start(self.holder_script, 60)
        self.assertFalse(port_is_free(self.port))
        kill_only(holder)
        self.assert_port_free_within(1.0)

    def test_a_grandchild_of_a_parent_that_already_exited_is_stopped_too(self):
        _, holder, _ = self.start(self.chain_script, 60)
        self.assertFalse(port_is_free(self.port))
        kill_only(holder)
        self.assert_port_free_within(1.0)

    def test_a_normal_exit_stops_a_lingering_child_before_the_lock_is_released(self):
        seen = self.dir / "seen.txt"
        hook = f'"{sys.executable}" "{self.dir / "hook.py"}" {self.port} "{seen}"'
        isolation.write_config(self.dir / "project", lock_hook=hook)
        process, _, listed = self.start(self.holder_script, 0.3,
                                        _AUTANA_PROJECT=str(self.dir / "project"))
        self.assertTrue(listed, "the child is not among the lock's members")
        _, errors = process.communicate(timeout=60)
        self.assertEqual(seen.read_text(), "free", "the lock was released while the port was held")
        self.assertTrue(port_is_free(self.port))
        self.assertIn("stopped process", errors)

    def test_a_release_leaves_alone_what_was_running_before_its_lock(self):
        out, errors = self.run_script("sequence.py", *self.ports)
        self.assertEqual(out.split()[:1], ["state"], errors)
        earlier_free, inside_free = out.split()[1:]
        self.assertEqual(inside_free, "True", errors)
        self.assertEqual(earlier_free, "False", "work started before the lock was killed")


@unittest.skipUnless(LINUX, "the token and watchdog are Linux-only")
class LinuxScopeTests(Fixture):
    def test_a_normal_exit_leaves_no_tagged_process_and_no_watchdog(self):
        import lock_group
        out, errors = self.run_script("leftover.py", self.port)
        self.assertEqual(out.split()[:1], ["state"], errors)
        token, watchdog = out.split()[1], int(out.split()[2])
        self.assertEqual(lock_group.tagged_pids(token), [])
        self.assertFalse(os.path.exists(f"/proc/{watchdog}"), "the watchdog is still running")
        self.assertTrue(port_is_free(self.port))

    def test_stop_leaves_a_live_process_that_does_not_carry_the_token(self):
        import lock_group
        other = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        self.addCleanup(other.wait)
        self.addCleanup(other.kill)
        self.assertFalse(lock_group.stop(other.pid, "not-its-token"))
        time.sleep(0.2)
        self.assertIsNone(other.poll(), "an untagged process was killed")

    def test_a_holder_whose_watchdog_died_still_releases_and_says_so(self):
        out, errors = self.run_script("nowatch.py")
        self.assertEqual(out.split(), ["released", "True"], errors)
        self.assertIn("watchdog is not running", errors)


@unittest.skipUnless(WINDOWS and PYTHON, "the kill-on-close job is Windows-only")
class JobTests(Fixture):
    def test_the_holder_joins_its_own_job_inside_another_job_and_its_children_land_in_it(self):
        _, holder, listed = self.start(self.holder_script, 60, wrap=[str(self.dir / "outer.py")])
        self.assertTrue(listed, "the child is not a member of the holder's job")
        kill_only(holder)
        self.assert_port_free_within(1.0)

    def test_only_a_member_of_the_job_is_stopped_and_by_handle(self):
        out, errors = self.run_script("direct.py", "handle", self.ports[0])
        self.assertEqual(out.split()[1:], ["False", "True"], errors)

    def test_a_process_that_asks_to_break_away_can_leave_the_job(self):
        out, errors = self.run_script("direct.py", "breakaway", self.ports[0])
        self.assertTrue(out.startswith("pid"), errors)
        self.addCleanup(kill_tree, int(out.split()[1]))
        self.assertFalse(port_is_free(self.ports[0]), "the breakaway child died with its job")

    def test_a_job_too_big_to_list_says_so(self):
        out, errors = self.run_script("direct.py", "overflow", self.ports[0])
        self.assertIn("done", out, errors)
        self.assertIn("can be listed", errors)


class ExclusiveOpenTests(unittest.TestCase):
    """A leftover holder must make the next open fail rather than split the
    byte stream, and open_when_free must read that as busy."""

    def setUp(self):
        try:
            import serial  # noqa: F401
        except ImportError:
            self.skipTest("pyserial is not installed")
        if WINDOWS:
            self.skipTest("Windows refuses a second open on its own")
        import device
        self.device = device
        unused, self.path = self.enterContext(port_guard.fake_port())
        self.enterContext(mock.patch.object(device, "locked_port", lambda: self.path))

    def test_a_second_open_fails_while_the_first_is_held(self):
        with self.device.open_serial():
            with self.assertRaises(OSError):
                self.device.open_serial()
        self.device.open_serial().close()

    def test_a_holder_in_another_process_makes_the_open_fail(self):
        script = ("import sys, time; sys.path.insert(0, %r); import device; "
                  "device.locked_port = lambda: %r; c = device.open_serial(); "
                  "print('open', flush=True); time.sleep(60)" % (str(DEVICE), self.path))
        other = subprocess.Popen([sys.executable, "-c", script], stdout=subprocess.PIPE, text=True)
        self.addCleanup(other.communicate)
        self.addCleanup(other.kill)
        self.assertEqual(other.stdout.readline().strip(), "open")
        with self.assertRaises(OSError):
            self.device.open_serial()

    def test_open_when_free_waits_for_it_and_then_names_the_port_busy(self):
        with self.device.open_serial():
            with self.assertRaises(self.device.PortUnavailable):
                self.device.open_when_free(0.3, opener=lambda: self.device.open_serial(),
                                           sleep=lambda _: time.sleep(0.05))


if __name__ == "__main__":
    unittest.main()
