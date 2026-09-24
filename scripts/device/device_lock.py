"""Cooperative, FIFO lock state for a shared development board."""

import argparse
import contextlib
import errno
import json
import os
import socket
import sys
import tempfile
import time
import uuid
from pathlib import Path

import device_hook


DEFAULT_STALE_SECONDS = 600
GUARD_STALE_SECONDS = 30


def default_root():
    return Path(tempfile.gettempdir()) / "autana-device"


def process_alive(pid):
    """Only a pid that demonstrably does not exist counts as dead. Every other
    outcome answers alive: this decides whether one owner may take the board
    from another, and a wrongly reclaimed lock corrupts somebody's capture
    while a wrongly held one only waits out its heartbeat."""
    if pid == os.getpid():
        return True
    if pid <= 0:
        return False
    if os.name == "nt":
        return windows_process_alive(pid)
    return posix_process_alive(pid)


def posix_process_alive(pid, kill=None):
    kill = kill or os.kill
    try:
        kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True
    return True


ERROR_ACCESS_DENIED = 5
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259


def windows_process_alive(pid, kernel32=None):
    """Never os.kill(pid, 0) on Windows: signal 0 there is CTRL_C_EVENT, so
    CPython calls GenerateConsoleCtrlEvent and treats the pid as a console
    process group. For a process on another console that fails with
    ERROR_INVALID_PARAMETER - a live holder reads as dead and its lock is
    taken - and for one sharing the caller's console it delivers Ctrl+C.
    Asks the process table instead: a pid that cannot be opened for any
    reason but access denied is gone, and an opened one is alive until it
    reports an exit code."""
    import ctypes
    from ctypes import wintypes

    if kernel32 is None:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return ctypes.get_last_error() == ERROR_ACCESS_DENIED
    try:
        code = wintypes.DWORD()
        if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return True
        return code.value == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


class LockStore:
    def __init__(self, root=None, now=time.time, is_alive=process_alive):
        self.root = Path(root) if root else default_root()
        self.now = now
        self.is_alive = is_alive

    def stem(self, port):
        return port.replace("/", "_").replace("\\", "_").replace(":", "_")

    def lock_path(self, port):
        return self.root / (self.stem(port) + ".json")

    def human_path(self, port):
        return self.root / (self.stem(port) + ".human.json")

    def queue_dir(self, port):
        return self.root / (self.stem(port) + ".queue")

    def guard_path(self, port):
        return self.root / (self.stem(port) + ".guard")

    def read_json(self, path):
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError):
            return None

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
        temporary.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary, path)

    @contextlib.contextmanager
    def guard(self, port):
        path = self.guard_path(port)
        path.parent.mkdir(parents=True, exist_ok=True)
        while True:
            try:
                descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.close(descriptor)
                break
            except FileExistsError:
                try:
                    age = self.now() - path.stat().st_mtime
                    if age > GUARD_STALE_SECONDS:
                        path.unlink()
                        continue
                except FileNotFoundError:
                    continue
                time.sleep(0.02)
        try:
            yield
        finally:
            try:
                path.unlink()
            except FileNotFoundError:
                pass

    def enqueue(self, port, owner, purpose, pid=None):
        with self.guard(port):
            directory = self.queue_dir(port)
            directory.mkdir(parents=True, exist_ok=True)
            sequence_path = directory / "sequence"
            try:
                sequence = int(sequence_path.read_text(encoding="ascii")) + 1
            except (FileNotFoundError, ValueError):
                sequence = 1
            sequence_path.write_text(str(sequence), encoding="ascii")
            ticket = uuid.uuid4().hex
            self.write_json(directory / (ticket + ".json"), {
                "created_at": self.now(),
                "owner": owner,
                "pid": os.getpid() if pid is None else pid,
                "purpose": purpose,
                "sequence": sequence,
                "ticket": ticket,
            })
            return ticket

    def tickets(self, port):
        directory = self.queue_dir(port)
        result = []
        for path in directory.glob("*.json") if directory.exists() else ():
            ticket = self.read_json(path)
            if ticket:
                result.append(ticket)
        return sorted(result, key=lambda ticket: ticket["sequence"])

    def prune_crashed_waiters(self, port):
        for ticket in self.tickets(port):
            if ticket["pid"] != os.getpid() and not self.is_alive(ticket["pid"]):
                try:
                    (self.queue_dir(port) / (ticket["ticket"] + ".json")).unlink()
                except FileNotFoundError:
                    pass

    def is_stale(self, lock, stale_seconds):
        return self.now() - lock["heartbeat_at"] > stale_seconds

    def claim(self, port, ticket, expected_build_id, stale_seconds=DEFAULT_STALE_SECONDS):
        held = self._claim(port, ticket, expected_build_id, stale_seconds)
        if held:
            device_hook.emit("acquired", port, held["owner"], held["purpose"])
        return held

    def _claim(self, port, ticket, expected_build_id, stale_seconds):
        with self.guard(port):
            self.prune_crashed_waiters(port)
            pending = self.tickets(port)
            if not pending or pending[0]["ticket"] != ticket:
                return None
            if self.read_json(self.human_path(port)):
                return None
            current = self.read_json(self.lock_path(port))
            reclaimed = ""
            if current:
                same_host = current.get("host") == socket.gethostname()
                dead = same_host and not self.is_alive(current["pid"])
                if not dead and not self.is_stale(current, stale_seconds):
                    return None
                reason = "dead process" if dead else "heartbeat expiry"
                reclaimed = ("reclaimed lock from {owner} for {purpose} "
                             "({reason})".format(reason=reason, **current))
                self.lock_path(port).unlink(missing_ok=True)
            now = self.now()
            held = {
                "acquired_at": now,
                "expected_build_id": expected_build_id,
                "heartbeat_at": now,
                "host": socket.gethostname(),
                "log": reclaimed,
                "owner": pending[0]["owner"],
                "pid": pending[0]["pid"],
                "port": port,
                "purpose": pending[0]["purpose"],
                "token": uuid.uuid4().hex,
            }
            self.write_json(self.lock_path(port), held)
            (self.queue_dir(port) / (ticket + ".json")).unlink(missing_ok=True)
            return held

    def acquire(self, port, owner, purpose, expected_build_id="", wait=0,
                stale_seconds=DEFAULT_STALE_SECONDS):
        ticket = self.enqueue(port, owner, purpose)
        deadline = self.now() + wait
        waiting = False
        while True:
            held = self.claim(port, ticket, expected_build_id, stale_seconds)
            if held:
                return held
            if not waiting:
                device_hook.emit("waiting", port, owner, purpose)
                waiting = True
            if self.now() >= deadline:
                self.cancel(port, ticket)
                return None
            time.sleep(min(0.1, max(0, deadline - self.now())))

    def cancel(self, port, ticket):
        with self.guard(port):
            (self.queue_dir(port) / (ticket + ".json")).unlink(missing_ok=True)

    def heartbeat(self, port, token, owner="", purpose=""):
        alive = self._heartbeat(port, token)
        if not alive and owner:
            device_hook.emit("lost", port, owner, purpose)
        return alive

    def _heartbeat(self, port, token):
        with self.guard(port):
            lock = self.read_json(self.lock_path(port))
            if not lock or lock["token"] != token:
                return False
            lock["heartbeat_at"] = self.now()
            self.write_json(self.lock_path(port), lock)
            return True

    def set_expected_build_id(self, port, token, expected_build_id):
        with self.guard(port):
            lock = self.read_json(self.lock_path(port))
            if not lock or lock["token"] != token:
                return False
            lock["expected_build_id"] = expected_build_id
            self.write_json(self.lock_path(port), lock)
            return True

    def release(self, port, token):
        lock = self._release(port, token)
        if lock:
            device_hook.emit("released", port, lock["owner"], lock["purpose"])
        return bool(lock)

    def _release(self, port, token):
        with self.guard(port):
            lock = self.read_json(self.lock_path(port))
            if not lock or lock["token"] != token:
                return None
            self.lock_path(port).unlink(missing_ok=True)
            return lock

    def set_human(self, port, owner, note):
        with self.guard(port):
            self.write_json(self.human_path(port), {
                "note": note,
                "owner": owner,
                "since_at": self.now(),
            })
        device_hook.emit("human-reserved", port, owner, note=note)

    def clear_human(self, port):
        with self.guard(port):
            human = self.read_json(self.human_path(port))
            self.human_path(port).unlink(missing_ok=True)
        if human:
            device_hook.emit("human-cleared", port, human["owner"], note=human["note"])

    def status(self, port):
        with self.guard(port):
            self.prune_crashed_waiters(port)
            return {
                "human": self.read_json(self.human_path(port)),
                "lock": self.read_json(self.lock_path(port)),
                "queue": self.tickets(port),
            }


def print_status(status):
    if status["human"]:
        human = status["human"]
        age = max(0, time.time() - human["since_at"])
        print("human reservation: {owner}: {note} ({age:.0f}s ago)".format(age=age, **human))
    elif status["lock"]:
        lock = status["lock"]
        print("held by {owner} for {purpose} since {acquired_at:.0f}".format(**lock))
    else:
        print("unlocked")
    if status["queue"]:
        print("waiting: " + ", ".join(ticket["owner"] for ticket in status["queue"]))


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=default_root())
    parser.add_argument("--port", required=True)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("status")
    acquire = subparsers.add_parser("acquire")
    acquire.add_argument("--owner", default=os.environ.get("AUTANA_DEVICE_OWNER", "unknown"))
    acquire.add_argument("--purpose", required=True)
    acquire.add_argument("--expected-build-id", default="")
    acquire.add_argument("--wait", type=float, default=0)
    acquire.add_argument("--stale-seconds", type=float, default=DEFAULT_STALE_SECONDS)
    heartbeat = subparsers.add_parser("heartbeat")
    heartbeat.add_argument("--token", required=True)
    release = subparsers.add_parser("release")
    release.add_argument("--token", required=True)
    human = subparsers.add_parser("human")
    human.add_argument("--owner", required=True)
    human.add_argument("--note", required=True)
    subparsers.add_parser("clear-human")
    args = parser.parse_args(argv)
    store = LockStore(args.root)
    if args.command == "status":
        print_status(store.status(args.port))
        return 0
    if args.command == "acquire":
        held = store.acquire(args.port, args.owner, args.purpose,
                             args.expected_build_id, args.wait, args.stale_seconds)
        if not held:
            print("lock not acquired", file=sys.stderr)
            return 1
        if held["log"]:
            print(held["log"], file=sys.stderr)
        print(json.dumps(held, sort_keys=True))
        return 0
    if args.command == "heartbeat":
        return 0 if store.heartbeat(args.port, args.token) else 1
    if args.command == "release":
        return 0 if store.release(args.port, args.token) else 1
    if args.command == "human":
        store.set_human(args.port, args.owner, args.note)
        return 0
    store.clear_human(args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
