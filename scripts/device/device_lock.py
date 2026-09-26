"""Cooperative, FIFO lock state for a shared development board."""

import argparse
import contextlib
import datetime
import errno
import json
import os
import socket
import statistics
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
    def __init__(self, root=None, now=time.time, is_alive=process_alive, records_root=None):
        self.root = Path(root) if root else default_root()
        self.now = now
        self.is_alive = is_alive
        self.records_root = Path(records_root or os.environ.get("AUTANA_RECORDS") or
                                 Path(__file__).resolve().parents[2] / ".records" / "device")

    def duration_history(self):
        history = {}
        try:
            with open(self.records_root / "index.jsonl", encoding="utf-8") as stream:
                for line in stream:
                    try:
                        entry = json.loads(line)
                        duration = entry.get("duration_seconds")
                        if duration is not None and duration >= 0 and not entry.get("error"):
                            history.setdefault(entry["command"], []).append(duration)
                    except (ValueError, KeyError, TypeError):
                        continue
        except FileNotFoundError:
            pass
        return {kind: statistics.median(values[-30:]) for kind, values in history.items()}

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

    def enqueue(self, port, owner, purpose, pid=None, kind=None):
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
                "kind": kind or purpose,
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

    def reclaim_reason(self, lock, stale_seconds):
        """Why `lock` may be taken from its holder, or "" while it holds."""
        same_host = lock.get("host") == socket.gethostname()
        if same_host and not self.is_alive(lock["pid"]):
            return "dead process"
        if self.is_stale(lock, stale_seconds):
            return "heartbeat expiry"
        return ""

    def claim(self, port, ticket, expected_build_id, stale_seconds=DEFAULT_STALE_SECONDS):
        held, reclaimed, reason = self._claim(port, ticket, expected_build_id, stale_seconds)
        if held:
            if reclaimed:
                device_hook.emit("lost", port, reclaimed["owner"], reclaimed["purpose"],
                                 note=reason)
            device_hook.emit("acquired", port, held["owner"], held["purpose"])
        return held

    def _claim(self, port, ticket, expected_build_id, stale_seconds):
        with self.guard(port):
            self.prune_crashed_waiters(port)
            pending = self.tickets(port)
            if not pending or pending[0]["ticket"] != ticket:
                return None, None, ""
            if self.read_json(self.human_path(port)):
                return None, None, ""
            current = self.read_json(self.lock_path(port))
            reclaimed = ""
            evicted = None
            reason = ""
            if current:
                reason = self.reclaim_reason(current, stale_seconds)
                if not reason:
                    return None, None, ""
                evicted = current
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
                "kind": pending[0].get("kind", pending[0]["purpose"]),
                "token": uuid.uuid4().hex,
            }
            self.write_json(self.lock_path(port), held)
            (self.queue_dir(port) / (ticket + ".json")).unlink(missing_ok=True)
            return held, evicted, reason

    def acquire(self, port, owner, purpose, expected_build_id="", wait=0,
                stale_seconds=DEFAULT_STALE_SECONDS, kind=None):
        ticket = self.enqueue(port, owner, purpose, kind=kind)
        deadline = self.now() + wait
        waiting = False
        while True:
            held = self.claim(port, ticket, expected_build_id, stale_seconds)
            if held:
                return held
            if self.now() >= deadline:
                self.cancel(port, ticket)
                if waiting:
                    device_hook.emit("gave-up", port, owner, purpose)
                return None
            if not self.read_json(self.queue_dir(port) / (ticket + ".json")):
                if waiting:
                    device_hook.emit("gave-up", port, owner, purpose)
                return None
            if not waiting:
                device_hook.emit("waiting", port, owner, purpose)
                waiting = True
            if not hasattr(self, "_last_wait_notice") or self.now() - self._last_wait_notice >= 30:
                status = self.status(port)
                place = next((index for index, item in enumerate(status["queue"], 1)
                              if item["ticket"] == ticket), None)
                if place:
                    estimate = queue_estimates(status, self.now()).get(ticket)
                    print(f"waiting for board: queue place {place}; estimated start "
                          f"{format_estimate(estimate)}", file=sys.stderr)
                self._last_wait_notice = self.now()
            time.sleep(min(0.1, max(0, deadline - self.now())))

    def cancel(self, port, ticket):
        with self.guard(port):
            (self.queue_dir(port) / (ticket + ".json")).unlink(missing_ok=True)

    def heartbeat(self, port, token):
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

    def check_token(self, port, token, stale_seconds=DEFAULT_STALE_SECONDS):
        with self.guard(port):
            lock = self.read_json(self.lock_path(port))
            return bool(lock and lock["token"] == token and
                        not self.reclaim_reason(lock, stale_seconds))

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
            reservation_id = uuid.uuid4().hex
            self.write_json(self.human_path(port), {
                "id": reservation_id,
                "note": note,
                "owner": owner,
                "since_at": self.now(),
            })
        device_hook.emit("human-reserved", port, owner, note=note)
        return reservation_id

    def clear_human(self, port):
        with self.guard(port):
            human = self.read_json(self.human_path(port))
            self.human_path(port).unlink(missing_ok=True)
        if human:
            device_hook.emit("human-cleared", port, human["owner"], note=human["note"])

    def status(self, port, stale_seconds=DEFAULT_STALE_SECONDS):
        """A lock the next claim() would reclaim is reported under
        "reclaimable", not "lock": nothing holds the board, but the file stays
        for claim() to replace, with its record of whom it took it from."""
        with self.guard(port):
            self.prune_crashed_waiters(port)
            lock = self.read_json(self.lock_path(port))
            reason = self.reclaim_reason(lock, stale_seconds) if lock else ""
            return {
                "human": self.read_json(self.human_path(port)),
                "lock": None if reason else lock,
                "reclaimable": dict(lock, reason=reason) if reason else None,
                "queue": self.tickets(port),
                "durations": self.duration_history(),
            }


def local_time(timestamp):
    return datetime.datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M:%S")


def format_estimate(timestamp):
    return local_time(timestamp) if timestamp is not None else "unknown (no duration history)"


def queue_estimates(status, now):
    durations = status.get("durations", {})
    lock = status.get("lock")
    if status.get("human"):
        start = None
    elif lock:
        duration = durations.get(lock.get("kind", lock["purpose"]))
        start = max(now, lock["acquired_at"] + duration) if duration is not None else None
    else:
        start = now
    estimates = {}
    for ticket in status["queue"]:
        estimates[ticket["ticket"]] = start
        duration = durations.get(ticket.get("kind", ticket["purpose"]))
        start = start + duration if start is not None and duration is not None else None
    return estimates


def print_status(status, now=None):
    now = time.time() if now is None else now
    durations = status.get("durations", {})
    if status["human"]:
        human = status["human"]
        age = max(0, now - human["since_at"])
        print("human reservation: {owner}: {note} ({age:.0f}s ago; since {since})".format(
            age=age, since=local_time(human["since_at"]), **human))
    elif status["lock"]:
        lock = status["lock"]
        duration = durations.get(lock.get("kind", lock["purpose"]))
        free = (max(now, lock["acquired_at"] + duration)
                if duration is not None else None)
        print("held by {owner} for {purpose} since {acquired_at:.0f} "
              "(local {local}; elapsed {elapsed:.0f}s; estimated free {free})".format(
                  local=local_time(lock["acquired_at"]),
                  elapsed=max(0, now - lock["acquired_at"]),
                  free=format_estimate(free), **lock))
    elif status.get("reclaimable"):
        print("unlocked - stale lock from {owner} for {purpose} ({reason})".format(**status["reclaimable"]))
    else:
        print("unlocked")
    if status["queue"]:
        print("waiting: " + ", ".join(ticket["owner"] for ticket in status["queue"]))
        estimates = queue_estimates(status, now)
        for index, ticket in enumerate(status["queue"], 1):
            print(f"  {index}. {ticket['owner']} for {ticket['purpose']}; estimated start "
                  f"{format_estimate(estimates[ticket['ticket']])}")


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
    check_token = subparsers.add_parser("check-token")
    check_token.add_argument("--token", required=True)
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
    if args.command == "check-token":
        return 0 if store.check_token(args.port, args.token) else 1
    if args.command == "human":
        store.set_human(args.port, args.owner, args.note)
        return 0
    store.clear_human(args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
