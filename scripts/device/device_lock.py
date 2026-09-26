"""Cooperative, FIFO lock state for a shared development board."""

import argparse
import contextlib
import datetime
import errno
import json
import math
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
    """One directory per machine, shared by every checkout and session."""
    return Path(os.environ.get("AUTANA_DEVICE_LOCK_ROOT") or
                Path(tempfile.gettempdir()) / "autana-device")


def normalise_board(serial):
    """A board's USB serial number as every lock file and record spells it."""
    return serial.strip().upper()


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
    """Every method takes `board`, the board's USB serial number: the one key
    that survives the COM renumbering a reset can cause."""

    def __init__(self, root=None, now=time.time, is_alive=process_alive):
        self.root = Path(root) if root else default_root()
        self.now = now
        self.is_alive = is_alive

    def stem(self, board):
        return board.replace("/", "_").replace("\\", "_").replace(":", "_")

    def lock_path(self, board):
        return self.root / (self.stem(board) + ".json")

    def human_path(self, board):
        return self.root / (self.stem(board) + ".human.json")

    def queue_dir(self, board):
        return self.root / (self.stem(board) + ".queue")

    def guard_path(self, board):
        return self.root / (self.stem(board) + ".guard")

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

    def boards(self):
        """Every board a lock, reservation or waiter in this root names."""
        paths = list(self.root.glob("*.json")) + list(self.root.glob("*.queue/*.json"))
        return sorted({record["board"] for record in map(self.read_json, paths)
                       if isinstance(record, dict) and isinstance(record.get("board"), str)})

    @contextlib.contextmanager
    def guard(self, board):
        path = self.guard_path(board)
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

    def enqueue(self, board, owner, purpose, pid=None, kind=None):
        with self.guard(board):
            directory = self.queue_dir(board)
            directory.mkdir(parents=True, exist_ok=True)
            sequence_path = directory / "sequence"
            try:
                sequence = int(sequence_path.read_text(encoding="ascii")) + 1
            except (FileNotFoundError, ValueError):
                sequence = 1
            sequence_path.write_text(str(sequence), encoding="ascii")
            ticket = uuid.uuid4().hex
            self.write_json(directory / (ticket + ".json"), {
                "board": board,
                "created_at": self.now(),
                "owner": owner,
                "pid": os.getpid() if pid is None else pid,
                "purpose": purpose,
                "kind": kind or purpose,
                "sequence": sequence,
                "ticket": ticket,
            })
            return ticket

    def tickets(self, board):
        directory = self.queue_dir(board)
        result = []
        for path in directory.glob("*.json") if directory.exists() else ():
            ticket = self.read_json(path)
            if ticket:
                result.append(ticket)
        return sorted(result, key=lambda ticket: ticket["sequence"])

    def prune_crashed_waiters(self, board):
        for ticket in self.tickets(board):
            if ticket["pid"] != os.getpid() and not self.is_alive(ticket["pid"]):
                try:
                    (self.queue_dir(board) / (ticket["ticket"] + ".json")).unlink()
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

    def claim(self, board, ticket, expected_build_id, stale_seconds=DEFAULT_STALE_SECONDS):
        held, reclaimed, reason = self._claim(board, ticket, expected_build_id, stale_seconds)
        if held:
            if reclaimed:
                device_hook.emit("lost", board, reclaimed["owner"], reclaimed["purpose"],
                                 note=reason)
            device_hook.emit("acquired", board, held["owner"], held["purpose"])
        return held

    def _claim(self, board, ticket, expected_build_id, stale_seconds):
        with self.guard(board):
            self.prune_crashed_waiters(board)
            pending = self.tickets(board)
            if not pending or pending[0]["ticket"] != ticket:
                return None, None, ""
            if self.read_json(self.human_path(board)):
                return None, None, ""
            current = self.read_json(self.lock_path(board))
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
                self.lock_path(board).unlink(missing_ok=True)
            now = self.now()
            held = {
                "acquired_at": now,
                "board": board,
                "expected_build_id": expected_build_id,
                "heartbeat_at": now,
                "host": socket.gethostname(),
                "log": reclaimed,
                "owner": pending[0]["owner"],
                "pid": pending[0]["pid"],
                "purpose": pending[0]["purpose"],
                "kind": pending[0]["kind"],
                "token": uuid.uuid4().hex,
            }
            self.write_json(self.lock_path(board), held)
            (self.queue_dir(board) / (ticket + ".json")).unlink(missing_ok=True)
            return held, evicted, reason

    def acquire(self, board, owner, purpose, expected_build_id="", wait=0,
                stale_seconds=DEFAULT_STALE_SECONDS, kind=None, on_wait=None):
        ticket = self.enqueue(board, owner, purpose, kind=kind)
        deadline = self.now() + wait
        waiting = False
        while True:
            held = self.claim(board, ticket, expected_build_id, stale_seconds)
            if held:
                return held
            if self.now() >= deadline:
                self.cancel(board, ticket)
                if waiting:
                    device_hook.emit("gave-up", board, owner, purpose)
                return None
            if not self.read_json(self.queue_dir(board) / (ticket + ".json")):
                if waiting:
                    device_hook.emit("gave-up", board, owner, purpose)
                return None
            if not waiting:
                device_hook.emit("waiting", board, owner, purpose)
                waiting = True
            if on_wait:
                on_wait(ticket)
            time.sleep(min(0.1, max(0, deadline - self.now())))

    def cancel(self, board, ticket):
        with self.guard(board):
            (self.queue_dir(board) / (ticket + ".json")).unlink(missing_ok=True)

    def heartbeat(self, board, token):
        """Refuses a lock the next claim() may reclaim: a holder that stalled
        past the stale window has to find out it lost the board, not renew it."""
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock["token"] != token or self.reclaim_reason(lock, DEFAULT_STALE_SECONDS):
                return False
            lock["heartbeat_at"] = self.now()
            self.write_json(self.lock_path(board), lock)
            return True

    def set_expected_build_id(self, board, token, expected_build_id):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock["token"] != token:
                return False
            lock["expected_build_id"] = expected_build_id
            self.write_json(self.lock_path(board), lock)
            return True

    def check_token(self, board, token, stale_seconds=DEFAULT_STALE_SECONDS):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            return bool(lock and lock["token"] == token and
                        not self.reclaim_reason(lock, stale_seconds))

    def release(self, board, token):
        lock = self._release(board, token)
        if lock:
            device_hook.emit("released", board, lock["owner"], lock["purpose"])
        return bool(lock)

    def _release(self, board, token):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock["token"] != token:
                return None
            self.lock_path(board).unlink(missing_ok=True)
            return lock

    def set_human(self, board, owner, note):
        with self.guard(board):
            reservation_id = uuid.uuid4().hex
            self.write_json(self.human_path(board), {
                "board": board,
                "id": reservation_id,
                "note": note,
                "owner": owner,
                "since_at": self.now(),
            })
        device_hook.emit("human-reserved", board, owner, note=note)
        return reservation_id

    def clear_human(self, board):
        with self.guard(board):
            human = self.read_json(self.human_path(board))
            self.human_path(board).unlink(missing_ok=True)
        if human:
            device_hook.emit("human-cleared", board, human["owner"], note=human["note"])

    def status(self, board, stale_seconds=DEFAULT_STALE_SECONDS):
        """A lock the next claim() would reclaim is reported under
        "reclaimable", not "lock": nothing holds the board, but the file stays
        for claim() to replace, with its record of whom it took it from."""
        with self.guard(board):
            self.prune_crashed_waiters(board)
            lock = self.read_json(self.lock_path(board))
            reason = self.reclaim_reason(lock, stale_seconds) if lock else ""
            return {
                "human": self.read_json(self.human_path(board)),
                "lock": None if reason else lock,
                "reclaimable": dict(lock, reason=reason) if reason else None,
                "queue": self.tickets(board),
            }


DURATIONS_FILE = "durations.jsonl"
ESTIMATE_MINIMUM_RUNS = 3
ESTIMATE_RECENT_RUNS = 30
# The file is rewritten down to the recent successful runs of each kind once
# it passes this, so `status` and a waiter's notice never read an unbounded log.
DURATIONS_TRIM_LINES = 400


def duration_rows(root=None):
    try:
        with open(Path(root or default_root()) / DURATIONS_FILE, encoding="utf-8") as stream:
            lines = stream.readlines()
    except FileNotFoundError:
        return []
    rows = []
    for line in lines:
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if isinstance(row, dict):
            rows.append(row)
    return rows


def successful_duration(row):
    duration = row.get("duration_seconds")
    if (row.get("error") or not isinstance(row.get("command"), str)
            or not isinstance(duration, (int, float)) or isinstance(duration, bool)):
        return None
    return duration if math.isfinite(duration) and duration >= 0 else None


def record_duration(kind, seconds, error=None, root=None):
    """A failed run is recorded with its error and never shapes an estimate."""
    path = Path(root or default_root()) / DURATIONS_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps({"command": kind, "duration_seconds": seconds,
                                 "error": error}) + "\n")
    rows = duration_rows(path.parent)
    if len(rows) > DURATIONS_TRIM_LINES:
        trim_durations(path, rows)


def trim_durations(path, rows):
    """A row another process appends during the rewrite can be lost; an
    estimate is a median of thirty and does not notice."""
    kept = {}
    for row in rows:
        if successful_duration(row) is not None:
            kept.setdefault(row["command"], []).append(row)
    recent = {id(row) for runs in kept.values() for row in runs[-ESTIMATE_RECENT_RUNS:]}
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text("".join(json.dumps(row) + "\n" for row in rows if id(row) in recent),
                         encoding="utf-8")
    os.replace(temporary, path)


def duration_history(root=None, minimum=ESTIMATE_MINIMUM_RUNS):
    history = {}
    for row in duration_rows(root):
        duration = successful_duration(row)
        if duration is not None:
            history.setdefault(row["command"], []).append(duration)
    return {kind: statistics.median(values[-ESTIMATE_RECENT_RUNS:])
            for kind, values in history.items() if len(values) >= minimum}


def local_time(timestamp):
    return datetime.datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M:%S")


def format_estimate(timestamp):
    return local_time(timestamp) if timestamp is not None else "unknown (no duration history)"


def queue_estimates(status, now, durations):
    """Each waiter's estimated start, by ticket: the holder's own kind sets
    when the board frees, and each waiter's kind how long it keeps it."""
    lock = status.get("lock")
    if status.get("human"):
        start = None
    elif lock:
        duration = durations.get(lock.get("kind"))
        start = max(now, lock["acquired_at"] + duration) if duration is not None else None
    else:
        start = now
    estimates = {}
    for ticket in status["queue"]:
        estimates[ticket["ticket"]] = start
        duration = durations.get(ticket.get("kind"))
        start = start + duration if start is not None and duration is not None else None
    return estimates


def status_entry(store, board, port=None, now=None, durations=None):
    """One board's state as `status --json` prints it. Times are epoch
    seconds; an estimate with too little history is None."""
    now = store.now() if now is None else now
    durations = duration_history(store.root) if durations is None else durations
    status = store.status(board)
    entry = {"board": board, "port": port, "state": "unlocked", "holder": None,
             "since": None, "elapsed_seconds": None, "estimated_free": None,
             "stale": None, "waiting": []}
    if status["human"]:
        human = status["human"]
        entry.update(state="human", holder={"owner": human["owner"], "purpose": human["note"]},
                     since=human["since_at"],
                     elapsed_seconds=round(max(0, now - human["since_at"])))
    elif status["lock"]:
        lock = status["lock"]
        duration = durations.get(lock.get("kind"))
        entry.update(state="held", holder={"owner": lock["owner"], "purpose": lock["purpose"]},
                     since=lock["acquired_at"],
                     elapsed_seconds=round(max(0, now - lock["acquired_at"])),
                     estimated_free=(max(now, lock["acquired_at"] + duration)
                                     if duration is not None else None))
    elif status["reclaimable"]:
        stale = status["reclaimable"]
        entry["stale"] = {"owner": stale["owner"], "purpose": stale["purpose"],
                          "reason": stale["reason"]}
    estimates = queue_estimates(status, now, durations)
    entry["waiting"] = [{"owner": ticket["owner"], "purpose": ticket["purpose"],
                         "estimated_start": estimates[ticket["ticket"]]}
                        for ticket in status["queue"]]
    return entry


def status_lines(entry):
    holder = entry["holder"]
    if entry["state"] == "human":
        lines = [f"human reservation: {holder['owner']}: {holder['purpose']} "
                 f"(since {local_time(entry['since'])}; {entry['elapsed_seconds']}s ago)"]
    elif entry["state"] == "held":
        lines = [f"held by {holder['owner']} for {holder['purpose']} since "
                 f"{local_time(entry['since'])} (elapsed {entry['elapsed_seconds']}s; "
                 f"estimated free {format_estimate(entry['estimated_free'])})"]
    elif entry["stale"]:
        lines = ["unlocked - stale lock from {owner} for {purpose} ({reason})".format(
            **entry["stale"])]
    else:
        lines = ["unlocked"]
    if entry["waiting"]:
        lines.append("waiting:")
    for place, waiter in enumerate(entry["waiting"], 1):
        lines.append(f"  {place}. {waiter['owner']} for {waiter['purpose']}; estimated start "
                     f"{format_estimate(waiter['estimated_start'])}")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=default_root())
    parser.add_argument("--board", required=True, help="the board's USB serial number")
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
    board = normalise_board(args.board)
    store = LockStore(args.root)
    if args.command == "status":
        print("\n".join(status_lines(status_entry(store, board))))
        return 0
    if args.command == "acquire":
        held = store.acquire(board, args.owner, args.purpose,
                             args.expected_build_id, args.wait, args.stale_seconds)
        if not held:
            print("lock not acquired", file=sys.stderr)
            return 1
        if held["log"]:
            print(held["log"], file=sys.stderr)
        print(json.dumps(held, sort_keys=True))
        return 0
    if args.command == "heartbeat":
        return 0 if store.heartbeat(board, args.token) else 1
    if args.command == "release":
        return 0 if store.release(board, args.token) else 1
    if args.command == "check-token":
        return 0 if store.check_token(board, args.token) else 1
    if args.command == "human":
        store.set_human(board, args.owner, args.note)
        return 0
    store.clear_human(board)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
