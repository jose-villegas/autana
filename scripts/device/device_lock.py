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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "autana"))
from version import __version__  # noqa: E402  (path must be set up first)


DEFAULT_STALE_SECONDS = 600
GUARD_STALE_SECONDS = 30
# The mutex is guard() below (an O_CREAT|O_EXCL file), not an OS byte-range
# lock - the JSON files are the state it protects, not locks themselves. This
# numbers THAT state's shape, purely as a bump reminder pinned by the golden
# key-snapshot test in test_device_lock.py: bump it whenever a record's
# fields change in a way an older reader would misinterpret. Nothing here
# ever refuses a record over its value - two machines with different-aged
# autana installs still have to work the same board, so a mismatch is shown
# as information (status, a wait notice), never a reason to stop.
LOCK_PROTOCOL = 1
# The fields that decide something - who a record is, whether a lock is live,
# stale or dead, or a waiter's place in the FIFO - and so must be readable
# off ANY record, of any age or protocol, without guessing: boards() reads
# board; reclaim_reason()/is_stale() read pid/host/heartbeat_at; tickets()
# sorts on sequence and _claim() matches on ticket. Everything else (purpose,
# acquired_at, since_at, note, kind, ...) is display-only and read with
# .get() wherever it might come from a record this autana did not just write.
PROTOCOL_CORE_FIELDS = ("owner", "pid", "host", "heartbeat_at", "ticket", "sequence", "board", "protocol")


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

    def seen_path(self, board):
        return self.root / (self.stem(board) + ".seen.json")

    def read_json(self, path):
        # A file mid-replace on Windows refuses to open with PermissionError.
        for _ in range(250):
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except (FileNotFoundError, json.JSONDecodeError):
                return None
            except PermissionError:
                time.sleep(0.02)
        return None

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
        temporary.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")
        # Windows refuses to replace a file another process has open, and
        # boards() reads without the guard.
        for _ in range(250):
            try:
                os.replace(temporary, path)
                return
            except PermissionError:
                time.sleep(0.02)
        os.replace(temporary, path)

    def boards(self):
        """Every board a lock, reservation or waiter in this root names."""
        paths = ([path for path in self.root.glob("*.json") if not path.name.endswith(".seen.json")]
                 + list(self.root.glob("*.queue/*.json")))
        return sorted({record["board"] for record in map(self.read_json, paths)
                       if isinstance(record, dict) and isinstance(record.get("board"), str)})

    def seen_boards(self):
        """Every board this machine has ever found on USB, kept past its own
        lock, reservation or waiter - only for hand-to-human/take-back to
        recall a board that is now idle and unplugged."""
        return sorted({record["board"] for record in map(self.read_json, self.root.glob("*.seen.json"))
                       if isinstance(record, dict) and isinstance(record.get("board"), str)})

    def note_seen(self, board):
        """Records a board found on USB, once - nothing reads this again
        until seen_boards() needs it, so a repeat sighting is a no-op."""
        path = self.seen_path(board)
        if not self.read_json(path):
            self.write_json(path, {"board": board})

    @contextlib.contextmanager
    def guard(self, board):
        path = self.guard_path(board)
        path.parent.mkdir(parents=True, exist_ok=True)
        # On Windows a guard another process is still deleting, or merely
        # stat()ing, refuses create and unlink with PermissionError: busy, not
        # an error.
        while True:
            try:
                descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.close(descriptor)
                break
            except (FileExistsError, PermissionError):
                try:
                    age = self.now() - path.stat().st_mtime
                    if age > GUARD_STALE_SECONDS:
                        path.unlink()
                        continue
                except FileNotFoundError:
                    continue
                except PermissionError:
                    pass
                time.sleep(0.02)
        try:
            yield
        finally:
            self.drop_guard(path)

    def drop_guard(self, path):
        for _ in range(250):
            try:
                path.unlink()
                return
            except FileNotFoundError:
                return
            except PermissionError:
                time.sleep(0.02)
        path.unlink(missing_ok=True)

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
                "protocol": LOCK_PROTOCOL,
                "autana_version": __version__,
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
                device_hook.emit("lost", board, reclaimed["owner"], reclaimed.get("purpose", ""),
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
                # Judged from PROTOCOL_CORE_FIELDS alone (pid, host,
                # heartbeat_at), so this never depends on a protocol match: a
                # live holder, of any protocol, simply keeps the board until
                # it is dead or stale - never refused, never a reason to stop.
                reason = self.reclaim_reason(current, stale_seconds)
                if not reason:
                    return None, None, ""
                evicted = current
                reclaimed = "reclaimed lock from {owner} for {purpose} ({reason})".format(
                    owner=current.get("owner", "unknown"), purpose=current.get("purpose", "unknown"),
                    reason=reason)
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
                "protocol": LOCK_PROTOCOL,
                "autana_version": __version__,
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
            if not self.read_json(self.queue_dir(board) / (ticket + ".json")):
                if waiting:
                    device_hook.emit("gave-up", board, owner, purpose)
                return None
            # A caller that asked to wait hears it is queued even when a slow
            # first claim has already used up its whole wait.
            if wait > 0:
                if not waiting:
                    device_hook.emit("waiting", board, owner, purpose)
                    waiting = True
                if on_wait:
                    on_wait(ticket)
            if self.now() >= deadline:
                self.cancel(board, ticket)
                if waiting:
                    device_hook.emit("gave-up", board, owner, purpose)
                return None
            time.sleep(min(0.1, max(0, deadline - self.now())))

    def cancel(self, board, ticket):
        with self.guard(board):
            (self.queue_dir(board) / (ticket + ".json")).unlink(missing_ok=True)

    def heartbeat(self, board, token):
        """Refuses a lock the next claim() may reclaim: a holder that stalled
        past the stale window has to find out it lost the board, not renew it.
        `token` is never a foreign lock's - a mismatch (missing or not ours)
        just means this is not our lock to touch, protocol notwithstanding."""
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock.get("token") != token or self.reclaim_reason(lock, DEFAULT_STALE_SECONDS):
                return False
            lock["heartbeat_at"] = self.now()
            self.write_json(self.lock_path(board), lock)
            return True

    def set_expected_build_id(self, board, token, expected_build_id):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock.get("token") != token:
                return False
            lock["expected_build_id"] = expected_build_id
            self.write_json(self.lock_path(board), lock)
            return True

    def check_token(self, board, token, stale_seconds=DEFAULT_STALE_SECONDS):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            return bool(lock and lock.get("token") == token and
                        not self.reclaim_reason(lock, stale_seconds))

    def release(self, board, token):
        lock = self._release(board, token)
        if lock:
            device_hook.emit("released", board, lock["owner"], lock["purpose"])
        return bool(lock)

    def _release(self, board, token):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock.get("token") != token:
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
                "protocol": LOCK_PROTOCOL,
                "autana_version": __version__,
            })
        device_hook.emit("human-reserved", board, owner, note=note)
        return reservation_id

    def clear_human(self, board):
        with self.guard(board):
            human = self.read_json(self.human_path(board))
            self.human_path(board).unlink(missing_ok=True)
        if human:
            device_hook.emit("human-cleared", board, human["owner"], note=human.get("note", ""))

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
# Past this many lines the file is trimmed back (trim_durations()), so
# `status` and a waiter's notice never read an unbounded log.
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
    """Keeps each kind's last ESTIMATE_RECENT_RUNS successful rows, then the
    newest DURATIONS_TRIM_LINES of those, however many kinds there are. A row
    another process appends during the rewrite can be lost; a median over
    that many runs does not notice."""
    by_kind = {}
    for index, row in enumerate(rows):
        if successful_duration(row) is not None:
            by_kind.setdefault(row["command"], []).append(index)
    recent = sorted(index for runs in by_kind.values() for index in runs[-ESTIMATE_RECENT_RUNS:])
    kept = recent[-DURATIONS_TRIM_LINES:]
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text("".join(json.dumps(rows[index]) + "\n" for index in kept),
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
        acquired = lock.get("acquired_at")
        start = (max(now, acquired + duration)
                if duration is not None and acquired is not None else None)
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
        entry.update(state="human", holder={"owner": human["owner"], "purpose": human.get("note")},
                     since=human.get("since_at"),
                     elapsed_seconds=round(max(0, now - human["since_at"]))
                     if human.get("since_at") is not None else None)
    elif status["lock"]:
        lock = status["lock"]
        duration = durations.get(lock.get("kind"))
        acquired = lock.get("acquired_at")
        entry.update(state="held",
                     holder={"owner": lock["owner"], "purpose": lock.get("purpose"),
                             "protocol": lock.get("protocol", 0),
                             "autana_version": lock.get("autana_version")},
                     since=acquired,
                     elapsed_seconds=round(max(0, now - acquired)) if acquired is not None else None,
                     estimated_free=(max(now, acquired + duration)
                                     if duration is not None and acquired is not None else None))
    elif status["reclaimable"]:
        stale = status["reclaimable"]
        entry["stale"] = {"owner": stale["owner"], "purpose": stale.get("purpose"),
                          "reason": stale["reason"]}
    estimates = queue_estimates(status, now, durations)
    entry["waiting"] = [{"owner": ticket["owner"], "purpose": ticket.get("purpose"),
                         "estimated_start": estimates[ticket["ticket"]]}
                        for ticket in status["queue"]]
    return entry


def holder_version_text(holder):
    """'(autana <version>, lock protocol <n>)' - the information a differently
    versioned holder's record carries, shown while waiting and in `status`,
    never a reason to refuse it."""
    version = holder.get("autana_version") or "unknown"
    return f"(autana {version}, lock protocol {holder.get('protocol', 0)})"


def status_lines(entry):
    holder = entry["holder"]
    if entry["state"] == "human":
        lines = [f"human reservation: {holder['owner']}: {holder['purpose']} "
                 f"(since {local_time(entry['since'])}; {entry['elapsed_seconds']}s ago)"]
    elif entry["state"] == "held":
        lines = [f"held by {holder['owner']} for {holder['purpose']} since "
                 f"{local_time(entry['since'])} (elapsed {entry['elapsed_seconds']}s; "
                 f"estimated free {format_estimate(entry['estimated_free'])}; "
                 f"{holder_version_text(holder)})"]
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
