"""The lock and the serial port are two resources; the port is the real one.
A holder that starts a process which owns the port must not release the lock
while that process lives, and `status` must not read "unlocked" while any
process the holder started can still hold it.

Real processes throughout: the port is stood in for by a localhost TCP port
that one child binds and every other bind is refused, which behaves the same
on Windows, Linux and macOS."""

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
import device_lock  # noqa: E402

BOARD = "90:70:69:FE:A3:08"

PORT_HOLDER = textwrap.dedent("""
    import socket, sys, time
    sock = socket.socket()
    sock.bind(("127.0.0.1", int(sys.argv[1])))
    sock.listen()
    print("ready", flush=True)
    time.sleep(120)
""")

# A command that uses the board under a real HeldLock and starts a process
# that owns the port, the way esptool or a monitor would.
HOLDER = textwrap.dedent("""
    import subprocess, sys, time
    sys.path.insert(0, {device!r})
    import device, device_lock
    device.HeldLock.HEARTBEAT_SECONDS = 0.2
    store = device_lock.LockStore({root!r})
    with device.HeldLock(store, {board!r}, "tree-test", "tree", 0):
        child = subprocess.Popen([sys.executable, {holder!r}, {port!r}],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 text=True)
        child.stdout.readline()
        print("child", child.pid, flush=True)
        time.sleep(float(sys.argv[1]))
""")

# Runs at the lock's `released` event and records whether the port was free
# at that moment.
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


def kill_pid(pid):
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            os.kill(pid, 9)
    except OSError:
        pass


class HolderFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.port = free_port()
        self.children = []
        for name, text in (("holder.py", PORT_HOLDER), ("hook.py", HOOK)):
            (self.dir / name).write_text(text)
        (self.dir / "run.py").write_text(HOLDER.format(
            device=str(DEVICE), root=str(self.dir / "locks"), board=BOARD,
            holder=str(self.dir / "holder.py"), port=str(self.port)))
        self.store = device_lock.LockStore(self.dir / "locks")

    def start_holder(self, seconds, **environment):
        process = subprocess.Popen(
            [sys.executable, str(self.dir / "run.py"), str(seconds)], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=dict(os.environ, **environment))
        # Cleanups run last-in first: the child dies before the pipes drain,
        # since it holds the holder's stderr open.
        self.addCleanup(process.communicate)
        self.addCleanup(process.kill)
        line = process.stdout.readline().split()
        self.assertEqual(line[:1], ["child"], process.stderr.read() if process.poll() else "")
        self.children.append(int(line[1]))
        self.addCleanup(kill_pid, self.children[-1])
        return process


class LiveHolderTests(HolderFixture):
    def test_a_child_owning_the_port_is_gone_before_the_lock_is_released(self):
        seen = self.dir / "seen.txt"
        hook = f'"{sys.executable}" "{self.dir / "hook.py"}" {self.port} "{seen}"'
        process = self.start_holder(0.5, AUTANA_LOCK_HOOK=hook)
        _, errors = process.communicate(timeout=60)
        self.assertEqual(seen.read_text(), "free", "the lock was released while the port was held")
        self.assertTrue(port_is_free(self.port))
        self.assertIn(str(self.children[0]), errors, "the reaped child is reported")
        self.assertIsNone(self.store.status(BOARD)["lock"])


class HardKilledHolderTests(HolderFixture):
    def killed_holder(self):
        process = self.start_holder(60)
        time.sleep(1.0)  # past a heartbeat, which is what records the tree
        process.kill()
        process.wait()

    def test_status_never_says_unlocked_while_a_killed_holders_child_owns_the_port(self):
        self.killed_holder()
        end = time.monotonic() + 2.0
        while time.monotonic() < end:
            entry = device_lock.status_entry(self.store, BOARD, durations={})
            free = port_is_free(self.port)
            self.assertFalse(entry["state"] == "unlocked" and not free,
                             "status said unlocked while the port was held")
            time.sleep(0.05)

    def test_the_lock_is_free_again_once_the_orphan_is_gone(self):
        self.killed_holder()
        kill_pid(self.children[0])
        deadline = time.monotonic() + 10
        while (time.monotonic() < deadline and
               device_lock.status_entry(self.store, BOARD, durations={})["state"] != "unlocked"):
            time.sleep(0.1)
        entry = device_lock.status_entry(self.store, BOARD, durations={})
        self.assertEqual(entry["state"], "unlocked")
        ticket = self.store.enqueue(BOARD, "next", "flash")
        self.assertTrue(self.store.claim(BOARD, ticket, ""))


class TreeRecordTests(unittest.TestCase):
    """The rule itself, with the liveness answers injected."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.alive = {1}
        self.clock = [1000.0]
        self.store = device_lock.LockStore(self.temp.name, lambda: self.clock[0],
                                           lambda pid: pid in self.alive)

    def write(self, pid=99, tree=(), heartbeat_at=1000):
        self.store.write_json(self.store.lock_path(BOARD), {
            "board": BOARD, "owner": "holder", "purpose": "capture", "pid": pid,
            "acquired_at": 1000,
            "host": device_lock.socket.gethostname(), "heartbeat_at": heartbeat_at,
            "token": "t", "tree": list(tree), "protocol": device_lock.LOCK_PROTOCOL})
        return self.store.read_json(self.store.lock_path(BOARD))

    def test_a_dead_holder_with_a_live_child_still_holds(self):
        self.alive.add(200)
        self.assertEqual(self.store.reclaim_reason(self.write(tree=[200]), 600), "")

    def test_a_dead_holder_whose_children_are_gone_is_reclaimed(self):
        self.assertEqual(self.store.reclaim_reason(self.write(tree=[200]), 600), "dead process")

    def test_a_record_without_a_tree_is_judged_as_before(self):
        record = self.write()
        del record["tree"]
        self.assertEqual(self.store.reclaim_reason(record, 600), "dead process")

    def test_a_child_that_never_ends_is_still_bounded_by_the_heartbeat_window(self):
        self.alive.add(200)
        record = self.write(tree=[200])
        self.clock[0] += 601
        self.assertEqual(self.store.reclaim_reason(record, 600), "heartbeat expiry")

    def test_status_names_the_children_a_gone_holder_left(self):
        self.alive.add(200)
        self.write(tree=[200])
        entry = device_lock.status_entry(self.store, BOARD, durations={})
        self.assertEqual(entry["state"], "held")
        text = "\n".join(device_lock.status_lines(entry))
        self.assertIn("holder exited", text)
        self.assertIn("200", text)

    def test_a_heartbeat_records_the_tree(self):
        self.alive.add(99)
        self.write(pid=99)
        self.assertTrue(self.store.heartbeat(BOARD, "t", tree=[7, 8]))
        self.assertEqual(self.store.read_json(self.store.lock_path(BOARD))["tree"], [7, 8])


if __name__ == "__main__":
    unittest.main()
