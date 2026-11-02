"""Lock-aware commands for the shared USB Serial/JTAG boards, each named by its
USB serial number."""

import argparse
import collections
import contextlib
import errno
import gzip
from functools import partial
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import uuid
import time
from datetime import datetime
from pathlib import Path

import device_lock
from device_lock import board_label, short_id
from device_report import records_root
import device_report
import lock_scope

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import autana_config  # noqa: E402
import board_serial
from console_protocol import FRAMED_ENDINGS, replies_to, require_development, reply_prefix
from device_capture import (BOOTS_FROM_RESET, BOOTS_ON_RUNNING, BUILD_ID_BYTES_RE as BUILD_ID, BUILD_ID_REQUEST,
                            CAPTURE_COMPLETE, ERROR_LINE_RE, TEST_NAME, capture_ended_line, crash_signs,
                            suite_failure_signs, test_matches)

from process_tree import (stop_process_tree, launch_process_tree, close_process_tree,
                          install_console_interrupt_handler)
SUITE_RESULT = re.compile(rb":\d+:.*:(PASS|FAIL)(?:\r?$|:)", re.MULTILINE)
BOARD_ENV = autana_config.BOARD_ENV
TOKEN_ENV = autana_config.TOKEN_ENV
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher" / "tools" / "build"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher" / "tools" / "device"))
from espressif import espressif_tools_root, idf_path, idf_python  # noqa: E402  (path must be set up first)
from build_paths import BUILD_LOCK  # noqa: E402


COMMAND_ENV = autana_config.COMMAND_ENV


def command_label(kind):
    """What the lock says its holder is doing: the autana command that started
    this process, else this script called directly."""
    named = os.environ.get(COMMAND_ENV)
    return f"autana {named}" if named else f"device.py {kind}"


EXIT_INTERRUPTED = autana_config.EXIT_INTERRUPTED
BAUD = 115200
ESPRESSIF_VID = 0x303A

# Suite/listen captures run 13-131 KB and a flash log ~270 KB; only a capture
# that lands on the default path (not an explicit --out) is ever gzipped, and
# only once it clears this, so the common case stays plain-text and greppable.
COMPRESS_ABOVE_BYTES = 200_000
SLUG_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]+")
BOOT_ERROR = ERROR_LINE_RE
MAX_PRINTED_FAILURES = 10


def python_with_pyserial():
    return idf_python()


def rerun_under_idf_python(argv, script=__file__):
    """The exit status of script (this file unless given) run again with argv
    under ESP-IDF's Python, or None when this interpreter can already reach the
    board. pyserial lives in that environment, not in whichever `python` a
    caller found first, so no caller has to know which one to pick."""
    try:
        import serial  # noqa: F401
        return None
    except ImportError:
        pass
    python = idf_python()
    if os.path.normcase(os.path.abspath(python)) == os.path.normcase(os.path.abspath(sys.executable)):
        return None
    return subprocess.call([python, str(Path(script).resolve()), *argv])


def git_bash():
    """From a native Windows shell, `bash` on PATH is WSL's launcher, which
    hands the script path to Linux bash to unescape and cannot run ESP-IDF."""
    if os.name != "nt":
        return "bash"
    candidates = []
    git = shutil.which("git")
    if git:
        git_root = Path(git).resolve().parent.parent
        candidates += [git_root / "bin" / "bash.exe", git_root / "usr" / "bin" / "bash.exe"]
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432")):
        if base:
            candidates.append(Path(base) / "Git" / "bin" / "bash.exe")
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("Git Bash not found; install Git for Windows")


class NoBoard(RuntimeError):
    """The board asked for is not on USB, as it is not for a moment after a reset."""


class LockLost(RuntimeError):
    def __init__(self, message="device lock was lost"):
        super().__init__(message)


class TokenRefused(RuntimeError):
    pass


class LockBusy(RuntimeError):
    """The board was not free and this command would not, or could not, wait."""


class PortUnavailable(RuntimeError):
    """The board's port did not open within the wait it was given."""


class BatchEnding(RuntimeError):
    """An error that ends a batch: the next run would meet the same board."""


class BoardNotReading(BatchEnding):
    """The board stopped taking console input: a write did not drain within
    the port's write timeout."""


Board = collections.namedtuple("Board", "serial port")


def plugged_boards():
    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("pyserial is required; run this with the ESP-IDF Python") from error
    return [Board(device_lock.normalise_board(port.serial_number), port.device)
            for port in list_ports.comports()
            if port.vid == ESPRESSIF_VID and port.serial_number]


BOARD_SERIAL_PATTERN = r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}"


def known_boards(store=None):
    store = store if store is not None else device_lock.LockStore()
    return sorted(set(found.serial for found in plugged_boards())
                  | set(store.boards()) | set(store.seen_boards()))


def board_selector(value):
    named = device_lock.normalise_board(value)
    if (re.fullmatch(BOARD_SERIAL_PATTERN, named)
            or re.fullmatch(r"[0-9A-F]{%d}" % device_lock.SHORT_ID_DIGITS, named)):
        return named
    return None


def resolve_board(value, store=None):
    named = board_selector(value)
    if named is None:
        raise RuntimeError("board needs a full USB serial or four-digit short id")
    if re.fullmatch(BOARD_SERIAL_PATTERN, named):
        return named
    known = known_boards(store)
    matches = [serial for serial in known if short_id(serial) == named]
    if len(matches) == 1:
        return matches[0]
    if matches:
        raise RuntimeError("ambiguous board " + named + ": " + ", ".join(matches)
                           + "; use the full serial")
    raise NoBoard("no board matches " + named + "; known boards: "
                  + (", ".join(known) or "none"))


def chosen_board(board=None, store=None, token=None):
    named = board or os.environ.get(BOARD_ENV)
    if named:
        return resolve_board(named, store)
    token = token or os.environ.get(TOKEN_ENV)
    if token:
        store = store if store is not None else device_lock.LockStore()
        board = store.board_of_token(token)
        if board is None:
            raise TokenRefused("lock token is not live on any board (released, lapsed or wrong)")
        return board
    return None


def find_board(board=None):
    """The board named by `board` or autana's global --board, else the only one plugged in."""
    wanted = chosen_board(board)
    present = plugged_boards()
    if wanted:
        for candidate in present:
            if candidate.serial == wanted:
                return candidate
        raise NoBoard("board " + wanted + " is not on USB")
    if not present:
        raise NoBoard("no USB Serial/JTAG board found (VID 0x303A)")
    if len(present) > 1:
        raise RuntimeError("several boards are plugged in - name one with --board / -b / use: "
                           + ", ".join(sorted(board_label(b.serial) for b in present)))
    return present[0]


def board_for_lock(store, named=None, remembered=False, auto_pick=False, token=None):
    """The board a command queues for. A lock outlives the board's USB
    presence, so while a holder's reset has the only board off USB a waiter
    still finds it through the lock records; only opening its port needs USB.
    `remembered`, for `hand-to-human`/`take-back` only, falls back further
    still, to a board merely seen on USB before, with nothing active
    against it now, since those are the two commands a board dropped off
    USB (and idle) needs."""
    wanted = chosen_board(named, store, token)
    if wanted:
        return wanted, None
    present = sorted(found.serial for found in plugged_boards())
    for serial in present:
        store.note_seen(serial)
    if auto_pick and len(present) > 1:
        entries = [entry for entry in board_statuses(store) if entry["port"]]
        board, reason = pick_board(entries)
        print(f"picked {board_label(board)}: {reason}", file=sys.stderr)
        return board, reason
    candidates, source = present, "plugged in"
    if not candidates:
        candidates, source = store.boards(), "known to the lock"
    if not candidates and remembered:
        candidates, source = store.seen_boards(), "known to this machine"
    if len(candidates) == 1:
        return candidates[0], None
    if not candidates:
        raise NoBoard("no USB Serial/JTAG board found (VID 0x303A)")
    raise RuntimeError("several boards are " + source
                       + " - name one with --board / -b / use: " + ", ".join(board_label(serial) for serial in candidates))


# A reset's boot takes the USB Serial/JTAG port off the bus while it tunes
# PSRAM, about 0.3 s after the reset, and lists it again about 0.4 s later. The
# reappear bound covers a slow host and still fails fast; a reset that keeps
# the port up costs only RESET_LEAVE_SECONDS.
RESET_LEAVE_SECONDS = 2
USB_POLL_SECONDS = 0.05
PORT_REAPPEAR_SECONDS = 20


def reconnect_after_reset(seconds=PORT_REAPPEAR_SECONDS, sleep=time.sleep, now=time.monotonic):
    """The one wait after any reset of the locked board: until the board's
    USB serial number leaves the port list, then until it is listed again
    and its port opens. Opening before it left gets the dying handle, and
    the open fails or reads nothing; the list is polled, not the open, so a
    capture that follows loses as little of the boot as it can. Raises
    PortUnavailable at `seconds`."""
    board = ACTIVE_LOCK.held.board
    deadline = now() + seconds
    leave_by = min(now() + RESET_LEAVE_SECONDS, deadline)

    def listed():
        require_live_lock()
        return any(found.serial == board for found in plugged_boards())

    while now() < leave_by and listed():
        sleep(min(USB_POLL_SECONDS, max(0, leave_by - now())))
    while not listed():
        if now() >= deadline:
            raise PortUnavailable(f"board {board} did not come back on USB within "
                                  f"{seconds:g}s of its reset")
        sleep(min(USB_POLL_SECONDS, max(0, deadline - now())))
    return open_when_free(max(0, deadline - now()), reason="re-enumerating after reset",
                          sleep=sleep, now=now)


FLUSH_POLL_SECONDS = 0.05


def open_serial():
    """Opens the locked board's port for this process alone. Windows refuses
    a second open by itself; POSIX needs `exclusive`, an advisory flock that
    keeps out our readers and esptool but not screen or ModemManager, else two
    readers silently split the byte stream. A refused open is an OSError,
    which open_when_free treats as busy."""
    port = locked_port()
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required; run this with the ESP-IDF Python") from error

    class BoardSerial(serial.Serial):
        """Every wait to send is bounded by write_timeout. pyserial's own
        flush has no limit (tcdrain on POSIX, a poll on Windows), and its
        write timeout says only "Write timeout"."""

        def not_reading(self):
            return BoardNotReading(
                f"the board stopped reading its console: a write did not drain within "
                f"{self.write_timeout:g}s (a half-written image or a wedged app) - "
                f"autana flash, or autana reset")

        def write(self, data):
            try:
                return super().write(data)
            except serial.SerialTimeoutException as error:
                raise self.not_reading() from error

        def flush(self):
            deadline = time.monotonic() + self.write_timeout
            while self.out_waiting:
                if time.monotonic() >= deadline:
                    raise self.not_reading()
                time.sleep(FLUSH_POLL_SECONDS)

    connection = BoardSerial()
    connection.port = port
    connection.baudrate = BAUD
    connection.timeout = 0.2
    # A board that stops reading its console would otherwise block a write
    # in the OS forever, holding the lock with it. A reading board drains a
    # line at once; the longest a healthy one goes without reading is its boot.
    connection.write_timeout = BOOT_WAIT_SECONDS
    board_serial.before_open(connection)
    try:
        connection.open()
        board_serial.after_open(connection)
    except BaseException:
        connection.close()
        raise
    return connection


def now():
    return datetime.now()


def slug(text):
    """Sanitise one manifest field for use inside a filename: collapse any
    run of characters unsafe on either Windows or POSIX to a single dash."""
    text = SLUG_UNSAFE.sub("-", text.strip()).strip("-")
    return text or "unknown"


def resolve_capture_path(out, kind, owner, started_at, root=None):
    if out:
        return Path(out), False
    root = root or records_root()
    day = started_at.strftime("%Y%m%d")
    name = started_at.strftime("%H%M%S") + "_" + slug(kind) + "_" + slug(owner) + ".log"
    path = root / day / name
    path.parent.mkdir(parents=True, exist_ok=True)
    return path, True


def git_commit(cwd=None):
    try:
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=cwd, check=True,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


def compress_if_large(path):
    try:
        size = path.stat().st_size
    except OSError:
        return path
    if size <= COMPRESS_ABOVE_BYTES:
        return path
    gz_path = path.with_name(path.name + ".gz")
    with open(path, "rb") as source, gzip.open(gz_path, "wb") as dest:
        shutil.copyfileobj(source, dest)
    path.unlink()
    return gz_path


def append_manifest(entry, root=None):
    root = root or records_root()
    root.mkdir(parents=True, exist_ok=True)
    with open(root / "index.jsonl", "a", encoding="utf-8") as stream:
        stream.write(json.dumps(entry, sort_keys=True) + "\n")


def record_capture(path, managed, *, started_at, board, owner, purpose, command, commit,
                    suite=None, build_id=None, worktree=None, reason=None, error=None, root=None,
                    acquired_at=None, test_filter=None, console_send=None, image_flags=None):
    """The permanent trail: written for every flash/run-suite/listen call, so
    even a capture left behind in a doomed worktree still has metadata here."""
    path = Path(path)
    if managed:
        path = compress_if_large(path)
    try:
        capture_bytes = path.stat().st_size
    except OSError:
        capture_bytes = None
    entry = {
        "started_at": started_at.isoformat(),
        "acquired_at": datetime.fromtimestamp(acquired_at).isoformat() if acquired_at else None,
        "board": board,
        "owner": owner,
        "purpose": purpose,
        "command": command,
        "suite": suite,
        "build_id": build_id,
        "worktree": worktree,
        "commit": commit,
        "reason": reason,
        "error": error,
        "capture_path": str(path),
        "capture_bytes": capture_bytes,
    }
    if test_filter:
        entry["test_filter"] = test_filter
    if console_send:
        entry["console_send"] = console_send
    if image_flags is not None:
        # What a flash built, so readers can tell a perf-scoped or layout-seeded image by its build id.
        entry["image_flags"] = image_flags
    append_manifest(entry, root)
    return path


# As long as the lock's own stale window: a legitimate perf capture runs for
# minutes with the port open, so a wait shorter than that makes a task that
# queued correctly give up on a holder that is still working.
PORT_WAIT_SECONDS = 600
OPEN_FIRST_PAUSE_SECONDS = 0.1
OPEN_MAX_PAUSE_SECONDS = 1.0


def open_when_free(seconds=PORT_WAIT_SECONDS, opener=None, sleep=time.sleep,
                   now=time.monotonic, reason="held by another process"):
    """Opens the port once the OS lets go of it and returns the open connection;
    the caller closes it.

    The lock arbitrates intent; the OS owns the port, and the two disagree
    whenever a previous holder's reader outlives its lock: a caller that
    queued fairly then fails on a port it was promised, which reads as a flaky
    board. Waiting is the right answer: this caller already won its turn, a
    straggler drains in seconds, and a port nobody ever frees still reports
    itself at the deadline."""
    open_port = opener or open_serial  # resolved per call, so a patched opener is honoured
    deadline = now() + seconds
    waited = False
    pause = OPEN_FIRST_PAUSE_SECONDS
    while True:
        try:
            connection = open_port()
            if waited:
                print("board port came free", file=sys.stderr)
            return connection
        except (OSError, NoBoard) as error:
            require_live_lock()
            remaining = deadline - now()
            if remaining <= 0:
                raise PortUnavailable("board port still " + reason + " when the wait ran out: "
                                      + str(error) + previous_holder_note()) from error
            if not waited:
                print("waiting for the board port, " + reason, file=sys.stderr)
                waited = True
            sleep(min(pause, remaining))
            pause = min(2 * pause, OPEN_MAX_PAUSE_SECONDS)


def previous_holder_note():
    """Who plausibly still has the port, appended to the error of a command
    that holds the lock but cannot open it; empty outside a lock."""
    active = getattr(ACTIVE_LOCK, "held", None)
    if active is None:
        return ""
    return ". " + device_lock.previous_holder_text(active.store, active.board,
                                                   lock_scope)


def build_id_from_bytes(data):
    match = BUILD_ID.search(data)
    return match.group(1).decode("ascii", "replace") if match else None


def latest_build_id_from_bytes(data):
    matches = list(BUILD_ID.finditer(data))
    return matches[-1].group(1).decode("ascii", "replace") if matches else None



def count_suite_results(data):
    results = SUITE_RESULT.findall(data)
    return results.count(b"PASS"), results.count(b"FAIL")


def failures_by_suite(text):
    """(suite file stem, FAIL count), most failures first."""
    counts = {}
    for line in text.splitlines():
        match = device_report.RESULT_RE.match(line.strip())
        if match and match.group("status") == "FAIL":
            stem = re.split(r"[\\/]", match.group("file"))[-1].removesuffix(".c")
            counts[stem] = counts.get(stem, 0) + 1
    return sorted(counts.items(), key=lambda item: (-item[1], item[0]))


def print_crash_signs(data, boots_allowed):
    """Every capture command's crash verdict: each sign on its own line, and
    how many there were."""
    signs = crash_signs(data.decode("utf-8", errors="replace"), boots_allowed)
    for sign in signs:
        print(sign)
    if signs:
        keep_crashed_elf(data)
    return len(signs)


def print_report_summary(report_path):
    """The report's path, then its errors section: what to read before the capture."""
    if report_path is None:
        return
    print("report: " + str(report_path))
    for line in device_report.errors_section(Path(report_path).read_text(encoding="utf-8")):
        print("  " + line)


def print_suite_output(data, record_path, command, reason, verbose, boots_allowed, report_path=None):
    """Failures, crash signs and missing completion, after printing the verdict,
    then the report path and its errors section last, where a run is read."""
    text = data.decode("utf-8", errors="replace")
    if verbose and text:
        print(text, end="" if text.endswith("\n") else "\n")
    passed, failed = count_suite_results(data)
    _, _, failures = device_report.parse_suite_results(text)
    print(f"{command} results: {passed} PASS, {failed} FAIL")
    for name, message in failures[:MAX_PRINTED_FAILURES]:
        print(f"{name}: {message}" if message else name)
    if len(failures) > MAX_PRINTED_FAILURES:
        print(f"{len(failures) - MAX_PRINTED_FAILURES} more in the capture; by suite:")
        for suite_name, count in failures_by_suite(text):
            print(f"  {suite_name}: {count} FAIL")
    crashed = print_crash_signs(data, boots_allowed)
    signs = suite_failure_signs(text, failed)
    for sign in signs:
        print(sign)
    print(f"{command} capture: {record_path}")
    print(capture_ended_line(command, reason))
    print_report_summary(report_path)
    if reason != CAPTURE_COMPLETE:
        raise RuntimeError(f"no completion line arrived: the capture ended {reason}")
    return failed + crashed + len(signs)


def print_reset_output(data, verbose):
    text = data.decode("utf-8", errors="replace")
    if verbose:
        if text:
            print(text, end="" if text.endswith("\n") else "\n")
    else:
        for line in text.splitlines():
            if BOOT_ERROR.search(line):
                print(line)


ACTIVE_LOCK = threading.local()


def locked_port():
    """The COM port the locked board enumerates as now: a reset can renumber it."""
    active = getattr(ACTIVE_LOCK, "held", None)
    if active is None:
        raise RuntimeError("serial port access requires the device lock")
    require_live_lock()
    return find_board(active.board).port


def require_live_lock():
    active = getattr(ACTIVE_LOCK, "held", None)
    if active and (active.lost.is_set() or
                   not active.store.check_token(active.board, active.held["token"])):
        raise LockLost()


def require_unlost():
    """The read loops' check: the heartbeat reads the lock file, so a read
    only has to look at what the heartbeat found."""
    active = getattr(ACTIVE_LOCK, "held", None)
    if active and active.lost.is_set():
        raise LockLost()


WAIT_NOTICE_SECONDS = 30


def wait_notice(store, board, state, ticket):
    now = store.resource_now(board)
    if state.get("last_notice") is not None and now - state["last_notice"] < WAIT_NOTICE_SECONDS:
        return
    state["last_notice"] = now
    status = store.status(board)
    if status["lock"]:
        lock = status["lock"]
        if board == device_lock.HEAVY:
            print(f"heavy held by {lock['owner']} for {lock.get('purpose', 'unknown')} "
                  f"since {device_lock.format_estimate(lock.get('acquired_at'))} - waiting", file=sys.stderr)
        else:
            print(f"board held by {lock['owner']} "
                  f"{device_lock.holder_version_text(lock)} - waiting", file=sys.stderr)
    elif status["human"]:
        human = status["human"]
        left = device_lock.human_left_text(human, now)
        print(f"board reserved by {human['owner']}: {human.get('note')} - waiting "
              f"({left + ', unless renewed; ' if left else ''}`autana status`, "
              "`autana lock take-back`)", file=sys.stderr)
    place = next((index for index, item in enumerate(status["queue"], 1)
                  if item["ticket"] == ticket), None)
    if place:
        estimate = device_lock.queue_estimates(
            status, now, device_lock.duration_history(store.key_root(board), board=board)).get(ticket)
        print(f"waiting for {board if board == device_lock.HEAVY else 'board'}: queue place {place}; estimated start "
              f"{device_lock.format_estimate(estimate)}", file=sys.stderr)


class HeldLock:
    """One key held for one command. The command's duration is recorded
    when it ends, with an error when it raised, set `error`, or lost the lock.
    A borrower uses a validated snapshot and reaps only its own descendants."""

    HEARTBEAT_SECONDS = 5

    def __init__(self, store, board, owner, kind, wait, announce_waiters=False, estimate=None):
        self.store = store
        self.announce_waiters = announce_waiters
        self.board = board
        self.notice_state = {}
        store.check_order(board)
        self.token_env = autana_config.HEAVY_TOKEN_ENV if board == device_lock.HEAVY else TOKEN_ENV
        token = os.environ.get(self.token_env)
        self.borrowed = bool(token)
        self.keep_seconds = None
        if token:
            record = store.live_lock(board, token)
            if record is None:
                raise TokenRefused("lock token is not live on this board (released, lapsed or wrong)")
            self.held = device_lock.Held(record, "")
        else:
            self.held = store.acquire(board, owner, kind, wait=wait, on_wait=self.wait_notice,
                                      purpose=command_label(kind), estimate=estimate)
        if not self.held:
            raise LockBusy(f"{'heavy' if board == device_lock.HEAVY else 'device'} lock was not acquired: "
                           + device_lock.busy_text(store.status(board), store.resource_now(board)))
        if self.held.log:
            print(self.held.log, file=sys.stderr)
        self.stop = threading.Event()
        self.lost = threading.Event()
        self.notified = set()
        self.thread = threading.Thread(target=self.keep_alive, daemon=True)
        self.started = time.monotonic()
        self.kind = kind
        self.error = None
        self.renewed_at = time.monotonic()

    def wait_notice(self, ticket):
        wait_notice(self.store, self.board, self.notice_state, ticket)

    def keep_alive(self):
        if self.borrowed:
            while not self.stop.wait(self.HEARTBEAT_SECONDS):
                live = self.renews(self.store.check_token)
                if live is None:
                    continue
                if not live:
                    self.lost.set()
                    return
            return
        # Only a holder the person can end with Ctrl+C invites them to.
        poll = 1 if self.announce_waiters else self.HEARTBEAT_SECONDS
        next_heartbeat = time.monotonic() + self.HEARTBEAT_SECONDS
        while not self.stop.wait(poll):
            try:
                tickets = self.store.tickets(self.board) if self.announce_waiters else ()
            except OSError:
                tickets = ()
            for ticket in tickets:
                if ticket["ticket"] not in self.notified:
                    self.notified.add(ticket["ticket"])
                    print(f'{ticket["owner"]} is waiting for the board '
                          f'({ticket.get("purpose", "unknown")}) - Ctrl+C to hand it over', file=sys.stderr)
            if time.monotonic() >= next_heartbeat:
                renewed = self.renews(self.store.heartbeat)
                if renewed is None:
                    continue
                if not renewed:
                    self.lost.set()
                    print("device lock was lost", file=sys.stderr)
                    return
                next_heartbeat = time.monotonic() + self.HEARTBEAT_SECONDS

    def renews(self, renew):
        """renew(board, token): True or False, or None for one that raised and is
        retried on the next poll. The lock lapses DEFAULT_STALE_SECONDS after
        its last renewal, so failing until then is a loss."""
        try:
            renewed = renew(self.board, self.held["token"])
        except OSError as error:
            if time.monotonic() - self.renewed_at < device_lock.DEFAULT_STALE_SECONDS:
                print(f"device lock renewal failed, retrying: {error}", file=sys.stderr)
                return None
            renewed = False
        if renewed:
            self.renewed_at = time.monotonic()
        return renewed

    def __enter__(self):
        self.previous_lock = getattr(ACTIVE_LOCK, "held", None)
        ACTIVE_LOCK.held = self
        self.previous_token = os.environ.get(self.token_env)
        scope_tag = uuid.uuid4().hex if self.borrowed else self.held["token"]
        lock_scope.enter(scope_tag)
        os.environ[self.token_env] = self.held["token"]
        self.members_before = set(lock_scope.members())
        self.thread.start()
        return self

    def __exit__(self, error_type, error_value, unused_traceback):
        self.stop.set()
        try:
            self.thread.join()
        finally:
            lost = self.lost.is_set() or not self.store.check_token(self.board, self.held["token"])
            killed, survivors = lock_scope.reap(self.members_before)
            lock_scope.leave()
            if self.previous_token is None:
                os.environ.pop(self.token_env, None)
            else:
                os.environ[self.token_env] = self.previous_token
            if killed:
                print("stopped process(es) still running under the device lock: "
                      + ", ".join(map(str, killed)), file=sys.stderr)
            if survivors:
                print("device lock released with process(es) still running that could not "
                      "be stopped: " + ", ".join(map(str, survivors)), file=sys.stderr)
            if self.borrowed:
                pass
            elif self.keep_seconds is not None and not lost:
                if self.store.hold(self.board, self.held["token"], command_label(self.kind),
                                   seconds=self.keep_seconds):
                    print(f"lock held for {self.keep_seconds:g}s; token {self.held['token']}; "
                          f"release early: autana lock release {self.held['token']}", file=sys.stderr)
            else:
                self.store.release(self.board, self.held["token"])
            ACTIVE_LOCK.held = self.previous_lock
            error = (str(error_value) if error_type else
                     str(LockLost()) if lost else self.error)
            device_lock.record_duration(self.kind, time.monotonic() - self.started, error,
                                        self.store.key_root(self.board), board=self.board)
        if lost and error_type is None:
            raise LockLost()


class NestedCommand:
    """A command run under a lock an enclosing `batch` or `selftest` already
    holds: the enclosing lock's board and token, this command's own duration."""

    def __init__(self, lock, kind):
        self.lock = lock
        self.store = lock.store
        self.board = lock.board
        self.held = lock.held
        self.lost = lock.lost
        self.kind = kind
        self.started = time.monotonic()
        self.error = None


def tests_done(data):
    return device_report.SELFTEST_COMPLETE_RE.search(data.decode("utf-8", errors="replace")) is not None


def capture(connection, output, max_seconds, idle_seconds, expected_build_id=None,
            suite_name=None, append=False, echo=None, complete=tests_done,
            first_byte_seconds=None):
    data = bytearray()
    pending = b""
    seen_build_id = None
    last_non_shell = time.monotonic()
    deadline = last_non_shell + max_seconds if max_seconds is not None else None
    suite_complete = None
    if suite_name:
        suite_complete = b"RUNSUITE_COMPLETE name=" + suite_name.encode("ascii") + b" "
    with open(output, "ab" if append else "wb") as stream:
        while deadline is None or time.monotonic() < deadline:
            require_unlost()
            try:
                chunk = connection.read(4096)
            except OSError:
                require_live_lock()
                # The USB serial port re-enumerates under the reader now and
                # then; what was read is still a capture worth reporting.
                return bytes(data), "port lost"
            if not chunk:
                quiet = time.monotonic() - last_non_shell
                if not data and first_byte_seconds is not None and quiet >= first_byte_seconds:
                    return bytes(data), "silent"
                if idle_seconds is not None and quiet >= idle_seconds:
                    return bytes(data), "idle"
                continue
            data.extend(chunk)
            stream.write(chunk)
            stream.flush()
            if echo is not None:
                echo.write(chunk)
                echo.flush()
            pending += chunk
            lines = pending.split(b"\n")
            pending = lines.pop()
            for line in lines:
                line_build_id = build_id_from_bytes(line)
                if line_build_id:
                    seen_build_id = line_build_id
                    if expected_build_id and seen_build_id != expected_build_id:
                        raise RuntimeError("device changed build during capture: expected " +
                                           expected_build_id + ", got " + seen_build_id)
                if line.strip() and b"shell:" not in line:
                    last_non_shell = time.monotonic()
                text = line.strip()
                if suite_name:
                    if b"ignoring line: 'RUNSUITE" in text:
                        raise RuntimeError("this build has no test suites - autana flash diag")
                    if b"console line too long" in text:
                        raise TestFilterError("the device dropped the request as too long for its "
                                              "console line: " + text.decode("ascii", "replace"))
                    if (b"no suite named '" + suite_name.encode("ascii") + b"'") in text:
                        raise RuntimeError("no suite named " + suite_name + " on this build")
                    if text.startswith(suite_complete):
                        return bytes(data), CAPTURE_COMPLETE
            if complete is not None and complete(data):
                return bytes(data), CAPTURE_COMPLETE
    return bytes(data), "timeout"


def run_resetting(run):
    """Runs `run`, a step that resets the board and raises on failure, then
    reconnects whether or not it succeeded, and returns the new boot's open
    port. A failure stays the failure, even when the port never returns."""
    try:
        run()
    except LockLost:
        raise
    except (OSError, RuntimeError, subprocess.CalledProcessError):
        with contextlib.suppress(PortUnavailable):
            reconnect_after_reset().close()
        raise
    return reconnect_after_reset()


def reset(after="hard_reset"):
    """Restarts through esptool and returns the new boot's open port. RTS cannot
    restart a chip in download mode, which watchdog_reset can."""
    with open_when_free():
        pass
    command = [python_with_pyserial(), "-m", "esptool", "--chip", "esp32s3",
               "-p", locked_port(), "--after", after, "chip_id"]
    return run_resetting(lambda: subprocess.run(command, check=True))


def reset_and_capture(output, seconds, idle_seconds, expected_build_id=None,
                      complete=tests_done):
    """The console after an RTS reset, with the download-mode fallback of
    capture_with_watchdog_fallback."""
    return capture_with_watchdog_fallback(output, seconds, idle_seconds, expected_build_id,
                                          complete, connection=reset())


def capture_with_watchdog_fallback(output, seconds, idle_seconds, expected_build_id=None,
                                   complete=tests_done, *, connection):
    """The console after a reset whose port `connection` is. A board that says
    nothing at all is taken for a chip in download mode, the one case RTS
    cannot start, and gets the watchdog. One that spoke, even without a
    BUILD_ID, is left alone."""
    data, reason = capture_after_reset(output, seconds, idle_seconds, expected_build_id,
                                       complete, connection=connection)
    if reason != "silent":
        return data, reason
    print("board silent after an RTS reset, as from download mode; "
          "restarting it through the watchdog", file=sys.stderr)
    return capture_after_reset(output, seconds, idle_seconds, expected_build_id,
                               complete, connection=reset(after="watchdog_reset"))


RESET_FIRST_BYTE_SECONDS = 2


def capture_after_reset(output, seconds, idle_seconds, expected_build_id=None,
                        complete=tests_done, *, connection):
    """The console after a reset whose port reconnect_after_reset saw come
    back. A board that says nothing within RESET_FIRST_BYTE_SECONDS ends as
    "silent". One that reboots again mid-capture drops the port, which is
    reconnected and read on; what it printed in that gap is lost."""
    deadline = time.monotonic() + seconds
    data = bytearray()
    append = False

    def ended(reason):
        return bytes(data), reason if data else "silent"

    while True:
        with connection:
            part, reason = capture(connection, output, deadline - time.monotonic(),
                                   idle_seconds, expected_build_id, append=append,
                                   complete=complete, first_byte_seconds=None if append
                                   else RESET_FIRST_BYTE_SECONDS)
        data.extend(part)
        append = True
        if reason != "port lost":
            return ended(reason)
        print("reset capture lost the port; reconnecting", file=sys.stderr)
        try:
            connection = reconnect_after_reset(max(0, deadline - time.monotonic()))
        except PortUnavailable:
            return ended("port lost")


# What a development image logs once its console reads lines
# (console_start()); the frame loop, which takes a RUNSUITE, follows it.
CONSOLE_UP = b"listening for 'screenshot'"
BOOT_WAIT_SECONDS = 30


def console_up(build_id):
    def complete(data):
        return CONSOLE_UP in data and b"BUILD_ID=" + build_id.encode("ascii") in data
    return complete


def await_console(args, board, build_id, acquired_at, connection):
    """Waits under the caller's lock, after the flash's own reset, for
    `build_id` to boot to its console: a request sent before then is lost.
    A board that never gets there is reported, not refused,
    since the capture that follows says what it did instead."""
    started_at = now()
    try:
        output, managed = resolve_capture_path(None, "boot-" + args.variant, args.owner, started_at)
    except BaseException:
        connection.close()
        raise
    data, reason = b"", None
    try:
        data, reason = capture_with_watchdog_fallback(output, BOOT_WAIT_SECONDS, None, build_id,
                                                      console_up(build_id), connection=connection)
    finally:
        path = record_capture(output, managed, started_at=started_at, board=board,
                              owner=args.owner, purpose=args.purpose, command="boot",
                              build_id=latest_build_id_from_bytes(data), reason=reason,
                              worktree=str(Path(args.worktree).resolve()),
                              commit=git_commit(args.worktree), acquired_at=acquired_at)
    if reason == CAPTURE_COMPLETE:
        print("booted BUILD_ID=" + build_id + " to its console")
    else:
        print(f"no console from BUILD_ID={build_id} within {BOOT_WAIT_SECONDS}s ({reason}) - "
              f"sending anyway; the boot: {path}", file=sys.stderr)


def reset_device(args, store, board):
    """Reboot under the device lock, optionally keeping the post-reset
    console output as a capture record."""
    if not args.capture:
        with HeldLock(store, board, args.owner, "reset", args.wait):
            reset().close()
        print("board reset; USB serial port is back")
        return 0

    started_at = now()
    output, managed = resolve_capture_path(args.out, "reset", args.owner, started_at)
    data = b""
    reason = None
    error = None
    held = None
    try:
        with HeldLock(store, board, args.owner, "reset", args.wait) as held:
            data, reason = reset_and_capture(output, args.seconds, None)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
        error = str(caught)
        raise
    finally:
        final_path = record_capture(
            output, managed, started_at=started_at, board=board, owner=args.owner,
            purpose=args.purpose, command="reset", build_id=latest_build_id_from_bytes(data),
            worktree=str(Path.cwd()), commit=git_commit(), reason=reason, error=error,
            acquired_at=held.held["acquired_at"] if held else None)
    print_reset_output(data, getattr(args, "verbose", False))
    print("reset capture: " + str(final_path))
    print("reset capture ended: " + reason + "; bytes may have been lost in reset gap")
    return 0


CRASH_ADDRESS_RE = re.compile(rb"0x4[0-9a-fA-F]{7}")


def toolchain_tool(package, tool):
    """`tool` from the ESP-IDF tools `package` that ships it, found under
    espressif_tools_root(), or None when it is not installed. Sorted
    reverse-alphabetically so the newest of several installed versions wins."""
    root = espressif_tools_root() / "tools" / package
    for bin_dir in sorted(root.glob("*/*/bin"), reverse=True):
        for name in (tool + ".exe", tool):
            candidate = bin_dir / name
            if candidate.is_file():
                return candidate
    return None


def toolchain_addr2line():
    return toolchain_tool("xtensa-esp-elf", "xtensa-esp32s3-elf-addr2line")


def decode_crash_addresses(data, elf):
    """Every address on a `Backtrace:`/`PC` line in a capture, resolved
    against `elf` to a file and line number. Returns [] when nothing looks
    like a crash, or when no addr2line is installed to ask."""
    addresses = []
    for line in data.split(b"\n"):
        if b"Backtrace" in line or b"PC      :" in line or b"PC :" in line:
            addresses += CRASH_ADDRESS_RE.findall(line)
    return decode_addresses([address.decode("ascii") for address in addresses], elf)


# The firmware's frame watch warning (launcher/main/profile/frame_watch.c):
# `FRAME_WATCH <kind> in <n> of <window> frames at 0x<site>`, a log line's
# format after it. tests/test_device.py holds this to that file's own text.
FRAME_WATCH_LINE_RE = re.compile(r"FRAME_WATCH (alloc|free|console) in (\d+) of (\d+) frames at (0x[0-9a-fA-F]{8})")


def decode_frame_watch_sites(data, elf):
    """Every heap site a FRAME_WATCH warning names, resolved against `elf`.
    A log line's site is its format string, already printed beside it."""
    text = data.decode("utf-8", errors="replace")
    sites = [match.group(4) for match in FRAME_WATCH_LINE_RE.finditer(text) if match.group(1) != "console"]
    return decode_addresses(sites, elf)


def decode_addresses(addresses, elf):
    if not addresses:
        return []
    addr2line = toolchain_addr2line()
    if addr2line is None:
        return []
    unique = list(dict.fromkeys(addresses))
    result = subprocess.run([str(addr2line), "-pfiaC", "-e", str(elf), *unique],
                            capture_output=True, text=True)
    return [line for line in result.stdout.splitlines() if line.strip()]


ELF_ARCHIVE_KEEP = 24
# The panic handler's own line naming the crashed build (esp_system's panic.c).
PANIC_ELF_SHA = re.compile(rb"ELF file SHA256: ([0-9a-f]+)")


def elf_archive():
    """Where a capture that saw a crash keeps the crashed build's ELF as
    <BUILD_ID>.elf, beside the records, so its core dump stays decodable
    after the build directory has been built over."""
    return records_root() / "elf"


def keep_crashed_elf(data):
    """Copies the ELF of the build that crashed in capture `data` into
    elf_archive(), dropping all but the newest ELF_ARCHIVE_KEEP there: the
    build the panic names, else the capture's BUILD_ID. Nothing when this
    worktree has no ELF of that build. A convenience: a failure here is one
    stderr line, never the capture's."""
    named = PANIC_ELF_SHA.findall(data)
    build = named[-1].decode("ascii") if named else latest_build_id_from_bytes(data)
    archive = elf_archive()
    elf = find_elf_for_build_id(Path.cwd(), build)
    if elf is None or elf.parent == archive:
        return
    try:
        archive.mkdir(parents=True, exist_ok=True)
        build_id = (elf.parent / "build_id.txt").read_text(encoding="ascii").strip()
        shutil.copyfile(elf, archive / (build_id + ".elf"))
        newest_first = sorted(archive.glob("*.elf"), key=lambda path: path.stat().st_mtime, reverse=True)
        for stale in newest_first[ELF_ARCHIVE_KEEP:]:
            stale.unlink(missing_ok=True)
    except OSError as error:
        print(f"could not keep the crashed build's ELF: {error}", file=sys.stderr)


def same_build(build_id, other):
    """Whether two build names are one build: a BUILD_ID's hash before its
    first '-', or a core dump's ELF SHA-256 hex, which may run longer. The
    variant can be ignored because BUILD_ID's hash is the ELF's own SHA-256."""
    own, theirs = build_id.split("-", 1)[0], other.split("-", 1)[0]
    return bool(own and theirs) and (own.startswith(theirs) or theirs.startswith(own))


def find_elf_for_build_id(worktree, build_id):
    """The launcher.elf of build `build_id` (a BUILD_ID, or a core dump's ELF
    hash): a launcher/build*/ directory of this worktree whose build_id.txt
    names it, else the archive's copy. Matches by name, so a `--dev` build
    built later than a `--diag` one cannot stand in for it. None when
    `build_id` is empty or nothing matches."""
    if not build_id:
        return None
    for build_dir in sorted(Path(worktree).glob("launcher/build*")):
        id_file = build_dir / "build_id.txt"
        try:
            seen = id_file.read_text(encoding="ascii").strip()
        except OSError:
            continue
        if not same_build(seen, build_id):
            continue
        elf = build_dir / "launcher.elf"
        if elf.is_file():
            return elf
    for elf in sorted(elf_archive().glob("*.elf")):
        if same_build(elf.stem, build_id):
            return elf
    return None


@contextlib.contextmanager
def holding(store, board, args, held_lock, kind):
    """The lock a command runs under: a fresh one, or `held_lock` when a
    batch already holds the board for the whole sequence."""
    if held_lock is None:
        with HeldLock(store, board, args.owner, kind, args.wait) as held:
            yield held
        return
    nested = NestedCommand(held_lock, kind)
    error = None
    try:
        yield nested
    except BaseException as caught:
        error = str(caught)
        raise
    finally:
        device_lock.record_duration(kind, time.monotonic() - nested.started,
                                    error or nested.error, store.root, board=board)


FLASH_POLL_SECONDS = 0.5


def run_to_end(command, lost=None, timeout=None, **options):
    """Runs `command` to its end, stopping its whole process tree on any
    error, on Ctrl+C, or the moment `lost`, a held lock's event, None for a
    command that holds none, is set."""
    deadline = None if timeout is None else time.monotonic() + timeout
    process = launch_process_tree(command, **options)
    try:
        while True:
            try:
                code = process.wait(timeout=FLASH_POLL_SECONDS)
                break
            except subprocess.TimeoutExpired:
                if lost is not None and lost.is_set():
                    raise LockLost("device lock was lost during the command; the command was stopped")
                if deadline is not None and time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(command, timeout)
    except BaseException:
        stop_process_tree(process)
        raise
    finally:
        close_process_tree(process)
    if code:
        raise subprocess.CalledProcessError(code, command)


BUILD_SCRIPT = Path("launcher") / "tools" / "build" / "build.sh"
FLASH_SCRIPT = Path("scripts") / "device" / "flash_image.sh"
VARIANT_FLAGS = {"release": [], "dev": ["--dev"], "diag": ["--diag"]}
VARIANT_CONSOLE = {"release": False, "dev": True, "diag": True}
BUILD_DIRS = {"release": "build", "dev": "build.dev", "diag": "build.diag"}
BUILD_LOCK_POLL_SECONDS = 0.2
FLASH_ARGS_FILE = re.compile(r"^0x[0-9a-fA-F]+\s+(\S+)\s*$")


def flash_commands(bash, worktree, variant, build_flags=()):
    """A flash is a pair: `build` runs build.sh for `worktree`'s variant with
    no lock held; `write`, given an image snapshot's folder as its last
    argument, runs flash_image.sh, the only one that opens the port."""
    worktree = Path(worktree)
    return ([bash, (worktree / BUILD_SCRIPT).as_posix()] + VARIANT_FLAGS[variant]
            + list(build_flags),
            [bash, (worktree / FLASH_SCRIPT).as_posix()])


def layout_flags(args):
    """Image selection flags passed through to build.sh."""
    seed = getattr(args, "layout_seed", 0)
    flags = ["--layout-seed", str(seed)] if seed else []
    if getattr(args, "hot_tunables", False):
        flags.append("--hot-tunables")
    if getattr(args, "frame_laps", False):
        flags.append("--frame-laps")
    return flags


def build_directory(worktree, variant):
    return Path(worktree) / "launcher" / BUILD_DIRS[variant]


def try_lock_file(handle):
    try:
        if os.name == "nt":
            import msvcrt
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


@contextlib.contextmanager
def build_directory_lock(build_dir):
    """Lock one build directory through its snapshot; yield its prepared path,
    or None when the checkout has no --prepared-build support."""
    build_dir = Path(build_dir)
    prepared = None
    if (build_dir.parent / "tools/build/build_paths.py").is_file():
        from build_paths import prepare
        build_dir = prepared = prepare(build_dir.parent, build_dir.name)
    build_dir.mkdir(parents=True, exist_ok=True)
    with open(build_dir / BUILD_LOCK, "a+b") as handle:
        if not try_lock_file(handle):
            print("waiting for another build in " + str(build_dir), file=sys.stderr)
            while not try_lock_file(handle):
                time.sleep(BUILD_LOCK_POLL_SECONDS)
        yield prepared


def snapshot_image(build_dir, image_dir):
    """Copies what `build_dir`'s flash_args lists, flash_args itself and
    build_id.txt into `image_dir`, keeping their relative paths, and returns
    the build id. The write takes the snapshot, never the build directory,
    so a later build there cannot change what this flash writes."""
    build_dir, image_dir = Path(build_dir), Path(image_dir)
    try:
        flash_args = (build_dir / "flash_args").read_text(encoding="utf-8")
        build_id = (build_dir / "build_id.txt").read_text(encoding="ascii").strip()
    except OSError as error:
        raise RuntimeError(f"the build left no image to flash in {build_dir}: {error}") from error
    listed = [match.group(1) for match in map(FLASH_ARGS_FILE.match, flash_args.splitlines())
              if match]
    if not build_id or not listed:
        raise RuntimeError(f"the build in {build_dir} has no build id or no flash_args images")
    if image_dir.exists():
        shutil.rmtree(image_dir)
    for relative in ["flash_args", "build_id.txt"] + listed:
        target = image_dir / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(build_dir / relative, target)
    return build_id


def on_windows():
    return os.name == "nt"


def script_environment(environment=None):
    """Git Bash on Windows needs MSYSTEM; elsewhere it would send idf.sh
    looking for cmd."""
    environment = dict(environment or os.environ)
    if on_windows():
        environment.setdefault("MSYSTEM", "MINGW64")
    return environment


def new_image_directory():
    """A snapshot's own folder: private, uniquely named, outside the records,
    and removed by its owner once the write is done or abandoned. Made under
    the system's temporary directory."""
    return Path(tempfile.mkdtemp(prefix="autana-image-"))


def remove_image_directory(image):
    """Removes the snapshot folder itself. Windows can refuse for a moment
    while a stopped writer's handles close, so it tries again briefly."""
    for _ in range(50):
        try:
            shutil.rmtree(image)
            return
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.1)
    shutil.rmtree(image, ignore_errors=True)


BUILD_NICE_INCREMENT = 5
BUILD_WAIT_REPORT_MIN_SECONDS = 1


@contextlib.contextmanager
def build_policy(environment):
    """Private process-tree policy limits compilation, never board captures."""
    options = {}
    if environment.get(autana_config.BUILD_PRIORITY_ENV) == autana_config.BUILD_PRIORITY_BELOW_NORMAL:
        if os.name == "nt":
            options["creationflags"] = subprocess.BELOW_NORMAL_PRIORITY_CLASS
        else:
            options["preexec_fn"] = partial(os.nice, BUILD_NICE_INCREMENT)
    wait = float(
        environment.get(autana_config.WAIT_ENV, autana_config.DEFAULT_LOCK_WAIT_SECONDS))
    started = time.monotonic()
    try:
        with HeldLock(device_lock.LockStore(), device_lock.HEAVY,
                      environment.get(autana_config.OWNER_ENV) or device_lock.default_owner(), "build", wait):
            elapsed = time.monotonic() - started
            if elapsed >= BUILD_WAIT_REPORT_MIN_SECONDS:
                print(autana_config.HEAVY_WAIT_PREFIX + str(elapsed), flush=True)
            yield options
    except LockBusy as error:
        elapsed = time.monotonic() - started
        if elapsed >= BUILD_WAIT_REPORT_MIN_SECONDS:
            print(autana_config.HEAVY_WAIT_PREFIX + str(elapsed), flush=True)
        raise TimeoutError(str(error)) from error


def build_snapshot(build, build_dir, image_dir=None, **popen):
    """Compile under the process policy and directory lock; optionally snapshot."""
    environment = script_environment(popen.pop("env", None))
    with build_policy(environment) as priority:
        with build_directory_lock(build_dir) as prepared:
            environment[autana_config.HEAVY_TOKEN_ENV] = os.environ[autana_config.HEAVY_TOKEN_ENV]
            run_to_end(build + (["--prepared-build", prepared.as_posix()] if prepared else []), ACTIVE_LOCK.held.lost,
                       **dict(popen, env=environment, **priority))
            return snapshot_image(prepared or build_dir, image_dir) if image_dir else None


def build_worktree(worktree, variant, build_flags=()):
    """Build one worktree without acquiring a board; return the build's exit status."""
    worktree = Path(worktree).resolve()
    if not (worktree / BUILD_SCRIPT).is_file():
        raise RuntimeError("build tool not found: " + str(worktree / BUILD_SCRIPT))
    build, _ = flash_commands(git_bash(), worktree, variant, build_flags)
    try:
        build_snapshot(build, build_directory(worktree, variant), cwd=worktree, stdin=subprocess.DEVNULL)
    except TimeoutError:
        return autana_config.EXIT_BUSY
    except subprocess.CalledProcessError as failed:
        return failed.returncode
    return 0


def write_snapshot(held, write, image_dir, **popen):
    """Runs `write` on `image_dir` under `held`, the board's lock."""
    environment = script_environment(popen.pop("env", None))
    # Proof to flash_image.sh that this flash holds this board's lock: it
    # runs check-token with both before it opens the port.
    environment[TOKEN_ENV] = held.held["token"]
    environment[BOARD_ENV] = held.board
    run_to_end(write + [Path(image_dir).as_posix()], held.lost, env=environment, **popen)


FLASH_FAILURE = re.compile(r"fatal error|\berror:|^error\b|could not", re.I)


def flash_failure_line(text):
    """The line that says why a flash log failed: the first error line, since
    idf.py and ninja follow esptool's own reason with lines of their own."""
    lines = [line.strip() for line in text.splitlines()
             if line.strip() and not line.startswith("===") and "Press Enter" not in line]
    for line in lines:
        if FLASH_FAILURE.search(line):
            return line
    return lines[-1] if lines else "the log is empty"


class Built:
    """One flash's image, log and index entry: build_image() makes it with no
    lock held, write_image() writes its snapshot under the lock. A context
    manager: leaving it removes the snapshot if nothing wrote it, so an
    abandoned flash leaves none behind."""

    def __init__(self, args, board, extra_flags):
        self.args = args
        self.board = board
        self.worktree = Path(args.worktree).resolve()
        for script in (BUILD_SCRIPT, FLASH_SCRIPT):
            if not (self.worktree / script).is_file():
                raise RuntimeError("build tool not found: " + str(self.worktree / script))
        self.started_at = now()
        self.log, self.managed = resolve_capture_path(args.out, "flash-" + args.variant,
                                                      args.owner, self.started_at)
        self.image = new_image_directory()
        self.image_flags = list(extra_flags)
        self.build_id = None
        self.build_dir = build_directory(self.worktree, args.variant)
        self.build, self.write = flash_commands(git_bash(), self.worktree, args.variant,
                                                extra_flags)
        print("flash log: " + str(self.log))

    def run(self, step, mode):
        try:
            with open(self.log, mode) as stream:
                return step(cwd=self.worktree, stdin=subprocess.DEVNULL, stdout=stream,
                            stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as failed:
            text = Path(self.log).read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(f"{Path(failed.cmd[1]).name} failed (exit {failed.returncode}): "
                               f"{flash_failure_line(text)} - flash log: {self.log}") from failed

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        remove_image_directory(self.image)
        return False

    def record(self, build_id=None, error=None, acquired_at=None):
        record_capture(self.log, self.managed, started_at=self.started_at, board=self.board,
                       owner=self.args.owner, purpose=self.args.purpose, command="flash",
                       build_id=build_id, worktree=str(self.worktree),
                       commit=git_commit(self.worktree), error=error, acquired_at=acquired_at,
                       image_flags=self.image_flags)


def build_image(args, board, extra_flags=()):
    """The first half of a flash, run before its lock is taken: builds into a
    fresh log and snapshots the image. Returns the Built for its caller to
    enter; a build that does not finish is recorded as the flash's own end,
    its snapshot removed, and raised."""
    built = Built(args, board, extra_flags)
    error = None
    try:
        built.build_id = built.run(
            lambda **popen: build_snapshot(built.build, built.build_dir, built.image, **popen),
            "wb")
    except BaseException as caught:
        if isinstance(caught, (OSError, RuntimeError)):
            error = str(caught)
        built.record(error=error)
        built.__exit__(None, None, None)
        raise
    return built


def write_image(built, store, board, held_lock):
    """Writes `built`'s snapshot under the caller's lock and returns its build
    id and the new boot's open port. The snapshot is gone afterwards,
    written or not; the caller closes the port before releasing the lock."""
    build_id = error = held = None
    with contextlib.ExitStack() as cleanup:
        try:
            with (contextlib.nullcontext(held_lock) if held_lock.kind == "flash" else
                  holding(store, board, built.args, held_lock, "flash")) as held:
                with open_when_free():
                    pass
                connection = run_resetting(lambda: built.run(lambda **popen:
                    write_snapshot(held, built.write, built.image, **popen), "ab"))
                cleanup.callback(connection.close)
                build_id = built.build_id
                print("flashed BUILD_ID=" + build_id + " (esptool hash verified; boot not verified)")
        except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
            error = str(caught)
            raise
        finally:
            built.__exit__(None, None, None)
            built.record(build_id=build_id, error=error,
                         acquired_at=held.held["acquired_at"] if held else None)
        cleanup.pop_all()
    return build_id, connection


def test_patterns(values):
    """The patterns of repeated `--test` values, each a comma list. A pattern
    has a test name's shape (device_capture.TEST_NAME); the device matches it
    per segment (test_matches) and refuses one past its limits
    (SUITE_FILTER_MAX and SUITE_FILTER_LEN in launcher/test/suites.h)."""
    patterns = []
    for value in values or []:
        for pattern in value.split(","):
            pattern = pattern.strip()
            if not re.fullmatch(TEST_NAME, pattern):
                raise TestFilterError(f"--test {pattern!r}: a pattern is segments of letters, digits, "
                                      "'_' and '-', split by '/'")
            if pattern not in patterns:
                patterns.append(pattern)
    return patterns


def suite_request(suite, patterns):
    """The console line that runs `suite`, narrowed to `patterns` when given."""
    return ("\nRUNSUITE " + " ".join([suite, ",".join(patterns)] if patterns else [suite])
            + "\n").encode("ascii")


class TestFilterError(BatchEnding):
    """The board cannot or will not run the filter asked for."""

    __test__ = False


class NoTestMatched(TestFilterError):
    """A --test pattern selected nothing."""


def check_test_filter(data, suite, patterns):
    """A filtered run that did not filter, or had a pattern that matched
    nothing, is an error and not a result: the first would report every row
    of the suite as if it had been asked for, the second lists the names to
    choose from. A completed capture without filter counts cannot verify
    the requested selection."""
    text = data.decode("utf-8", errors="replace")
    complete = None
    for complete in device_report.SUITE_COMPLETE_RE.finditer(text):
        pass
    refused = re.findall(r"SUITE_FILTER_REFUSED pattern=(\S*)", text)
    if refused:
        raise TestFilterError(f"the device refused --test {refused[0]!r}: a pattern is one to "
                              "SUITE_FILTER_LEN - 1 characters, and there are at most "
                              "SUITE_FILTER_MAX (launcher/test/suites.h)")
    if complete is None:
        if patterns:
            raise TestFilterError("no RUNSUITE_COMPLETE line arrived: an image that predates --test "
                                  "drops a request this long")
        return
    if complete.group("selected") is None:
        raise TestFilterError("this image predates --test - autana flash diag")
    if int(complete.group("unmatched")):
        names = list(dict.fromkeys(re.findall(r"SUITE_TEST name=(\S+)", text)))
        missed = [pattern for pattern in patterns if not any(test_matches(pattern, name) for name in names)]
        raise NoTestMatched(
            f"--test {','.join(missed or patterns)} matches no test of {suite}. Its tests:\n  " + "\n  ".join(names))


def report_recorded_capture(path):
    """The report's path, or None when it could not be written."""
    if not path.is_file():
        print("report skipped: capture log was not created", file=sys.stderr)
        return None
    try:
        return device_report.write_report_for_capture(path, records_root() / "index.jsonl")
    except Exception as report_error:  # A report is a convenience, never fails the capture.
        print("report generation failed (capture is unaffected): " + str(report_error), file=sys.stderr)
        return None


def run_suite(args, store, board, held_lock=None, worktree=None, commit=None):
    """`worktree`/`commit` name the checkout the flashed build came from. A
    standalone `run-suite` has no `--worktree` of its own, so it keeps
    recording the ambient cwd; a `batch` call passes its own `--worktree`
    and that worktree's HEAD, since that is what was actually flashed."""
    started_at = now()
    patterns = getattr(args, "test_filter", None) or []
    output, managed = resolve_capture_path(args.out, "runsuite-" + args.suite, args.owner,
                                           started_at)
    data = b""
    reason = None
    error = None
    held = None
    try:
        with holding(store, board, args, held_lock, "run-suite") as held:
            with open_when_free() as connection:
                apply_tunables(connection, getattr(args, "tune_set", None) or [])
                apply_console_lines(connection, getattr(args, "console_send", None) or [])
                connection.write(suite_request(args.suite, patterns))
                data, reason = capture(connection, output, args.max_seconds, args.idle_seconds,
                                       args.expect_build_id, args.suite)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
        error = str(caught)
        raise
    finally:
        final_path = record_capture(
            output, managed, started_at=started_at, board=board, owner=args.owner,
            purpose=args.purpose, command="run-suite", suite=args.suite,
            build_id=latest_build_id_from_bytes(data) or args.expect_build_id,
            worktree=worktree if worktree is not None else str(Path.cwd()),
            commit=commit if commit is not None else git_commit(), reason=reason, error=error,
            acquired_at=held.held["acquired_at"] if held else None,
            test_filter=patterns or None,
            console_send=getattr(args, "console_send", None))
        report_path = report_recorded_capture(final_path)
    failed = print_suite_output(data, final_path, "suite", reason, getattr(args, "verbose", False), BOOTS_ON_RUNNING,
                                report_path)
    if patterns:
        check_test_filter(data, args.suite, patterns)
    return 1 if failed else 0


def selftest(args, store, board):
    """Build the diagnostics+autorun image, then flash it and, under that
    same lock, reset and capture the boot-time run of every registered suite
    until SELFTEST_COMPLETE (or a timeout), the way to run every suite this
    worktree registers on the device, and device_report.sh's own
    build+capture step for a report with no single named suite
    (report_test_results.sh and the frame-budget reports). A report scoped
    to one suite (report_boot_anim_perf.sh) uses `batch` instead, the same
    shape with a suite name to send."""
    worktree = str(Path(args.worktree).resolve())
    started_at = now()
    extra_flags = ["--autorun"] + layout_flags(args)
    if args.perf_scope:
        extra_flags.append("--perf-scope")
    flash_args = argparse.Namespace(owner=args.owner, purpose=args.purpose + " (flash)",
                                    wait=args.wait, worktree=args.worktree,
                                    variant="diag", out=None)
    with build_image(flash_args, board, extra_flags) as built, \
            HeldLock(store, board, args.owner, "selftest", args.wait) as held:
        build_id, connection = write_image(built, store, board, held)
        connection.close()
        commit = git_commit(worktree)

        data = b""
        reason = None
        error = None
        output, managed = resolve_capture_path(args.out, "selftest", args.owner, started_at)
        try:
            data, reason = reset_and_capture(output, args.max_seconds, args.idle_seconds,
                                             build_id)
        except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
            error = str(caught)
            raise
        finally:
            final_path = record_capture(
                output, managed, started_at=started_at, board=board, owner=args.owner,
                purpose=args.purpose, command="selftest",
                build_id=latest_build_id_from_bytes(data) or build_id,
                worktree=worktree, commit=commit, reason=reason, error=error,
                acquired_at=held.held["acquired_at"])
            report_path = report_recorded_capture(final_path)
        failed = print_suite_output(data, final_path, "selftest", reason, getattr(args, "verbose", False), BOOTS_FROM_RESET,
                                    report_path)
        return 1 if failed else 0


def is_reported_line(text):
    """What a quiet capture still shows: errors, and frame watch warnings."""
    return bool(BOOT_ERROR.search(text) or FRAME_WATCH_LINE_RE.search(text))


class ErrorLineSink:
    def __init__(self, output):
        self.output = output
        self.pending = b""

    def write(self, chunk):
        self.pending += chunk
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace")
            if is_reported_line(text):
                self.output.write(text + "\n")
                self.output.flush()

    def flush(self):
        self.output.flush()

    def finish(self):
        text = self.pending.decode("utf-8", errors="replace")
        if is_reported_line(text):
            self.output.write(text + "\n")
        self.output.flush()


def listen(args, store, board):
    started_at = now()
    output, managed = resolve_capture_path(args.out, "listen", args.owner, started_at)
    data = b""
    reason = None
    error = None
    echo = getattr(args, "echo", False)
    sink = sys.stdout.buffer if echo else ErrorLineSink(sys.stdout)
    held = None
    try:
        with HeldLock(store, board, args.owner, "listen", args.wait,
                      announce_waiters=True) as held:
            with open_when_free() as connection:
                data, reason = capture(connection, output, args.seconds, None,
                                       echo=sink, complete=None)
    except KeyboardInterrupt:
        reason = "stopped"
        data = Path(output).read_bytes() if Path(output).exists() else b""
    except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
        error = str(caught)
        raise
    finally:
        final_path = record_capture(output, managed, started_at=started_at, board=board,
                                    owner=args.owner, purpose=args.purpose, command="listen",
                                    build_id=latest_build_id_from_bytes(data), worktree=str(Path.cwd()),
                                    commit=git_commit(), reason=reason, error=error,
                                    acquired_at=held.held["acquired_at"] if held else None)
    if not echo:
        sink.finish()
    elif data and not data.endswith(b"\n"):
        print()
    crashed = print_crash_signs(data, BOOTS_ON_RUNNING)
    print("listen capture: " + str(final_path))
    print("listen capture ended: " + reason)
    elf = Path(args.elf) if args.elf else find_elf_for_build_id(
        Path.cwd(), latest_build_id_from_bytes(data))
    if elf:
        for label, decoded in (("crash addresses", decode_crash_addresses(data, elf)),
                               ("frame watch sites", decode_frame_watch_sites(data, elf))):
            if decoded:
                print("\n" + label + " decoded against " + str(elf) + ":")
                for line in decoded:
                    print("  " + line)
    return 1 if crashed else 0


COREDUMP_PARTITION = "coredump"
# parttool runs esptool twice, the partition table and then the partition.
# A reset after the first takes the port away from the second while the new
# boot re-enumerates, so neither resets and one reset follows both.
PARTTOOL_NO_RESET = "after=no-reset"
# The dump's ESP_CORE_DUMP_INFO note (espcoredump's core_dump_elf.c): a
# 4-byte version, then the crashed app's ELF SHA-256 as NUL-padded hex in a
# 64-byte field. ELF pads a note's name to 4 bytes.
COREDUMP_INFO_NOTE = b"ESP_CORE_DUMP_INFO\0"
COREDUMP_INFO_VERSION_BYTES = 4
COREDUMP_INFO_SHA_BYTES = 64
ELF_NOTE_ALIGN = 4
# An erased partition: its first word, the dump's total length, still reads
# as flash's erased state.
ERASED_FLASH = b"\xff"
COREDUMP_ERASED_HEADER_BYTES = 4


def coredump_build(raw):
    """The build a raw core dump came from: the hex ELF SHA-256 its app
    wrote into the dump, as many digits as the app kept. None when `raw`
    holds no dump."""
    at = raw.find(COREDUMP_INFO_NOTE)
    if at < 0:
        return None
    name_end = at + -(-len(COREDUMP_INFO_NOTE) // ELF_NOTE_ALIGN) * ELF_NOTE_ALIGN
    sha_at = name_end + COREDUMP_INFO_VERSION_BYTES
    sha = raw[sha_at:sha_at + COREDUMP_INFO_SHA_BYTES].split(b"\0", 1)[0]
    return sha.decode("ascii") if re.fullmatch(rb"[0-9a-f]+", sha) else None


def coredump(args, store, board):
    """The last panic's core dump, read raw from the board's coredump
    partition under the lock into the records, then decoded with the board
    free; --erase clears the partition, so the next crash's dump is told
    from this one. Either way the board is reset once at the end."""
    with HeldLock(store, board, args.owner, "coredump", args.wait):
        parttool = Path(idf_path()) / "components" / "partition_table" / "parttool.py"
        command = [idf_python(), str(parttool), "--esptool-args", PARTTOOL_NO_RESET, "--port", locked_port()]
        if args.erase:
            command += ["erase_partition", "--partition-name=" + COREDUMP_PARTITION]
            run_then_reset(command)
            print("coredump partition erased")
            return 0
        core, _ = resolve_capture_path(None, "coredump", args.owner, now())
        core = core.with_suffix(".bin")
        command += ["read_partition", "--partition-name=" + COREDUMP_PARTITION, "--output", str(core)]
        try:
            run_then_reset(command)
        except subprocess.CalledProcessError as failure:
            return failure.returncode
    if core.read_bytes()[:COREDUMP_ERASED_HEADER_BYTES] == ERASED_FLASH * COREDUMP_ERASED_HEADER_BYTES:
        core.unlink()
        print("no core dump in flash")
        return 0
    return decode_coredump(core, args.elf)


def run_then_reset(command):
    """Runs a parttool `command` that leaves the chip in esptool's stub, then
    restarts it whether or not the command succeeded."""
    try:
        subprocess.run(command, check=True)
    finally:
        reset().close()


def decode_coredump(core, elf=None):
    """Decodes the raw dump saved at `core`, no board needed, against the
    ELF of the build that crashed, which the dump itself names, or `elf`.
    A dump outlives later flashes, so the board's running build is no guide."""
    build = coredump_build(Path(core).read_bytes())
    print(f"core dump of build {build or 'unknown'}, saved as {core}")
    elf = Path(elf) if elf else find_elf_for_build_id(Path.cwd(), build)
    if elf is None:
        print(f"no launcher.elf in this worktree or {elf_archive()} is build {build or 'unknown'}: "
              f"build that commit, then `autana coredump --core {core} --elf PATH`", file=sys.stderr)
        return 1
    command = [idf_python(), "-m", "esp_coredump", "--chip", "esp32s3", "info_corefile",
               "--core", str(core), "--core-format", "raw"]
    gdb = toolchain_tool("xtensa-esp-elf-gdb", "xtensa-esp32s3-elf-gdb")
    if gdb:
        command += ["--gdb", str(gdb)]
    return subprocess.run(command + [str(elf)]).returncode


EXCHANGE_MAX_SECONDS = 600.0
EXCHANGE_READ_BYTES = 4096


def exchange(connection, line, reply, until, seconds):
    """Write one console line; return (replies, completed, cut_off) within an idle limit.

    Only complete matching reply lines renew the idle deadline; all exchanges have
    an overall cap so a continuously talking device cannot hold the lock forever.
    """
    pending = bytearray()
    found = []
    connection.reset_input_buffer()
    connection.write(("\n" + line + "\n").encode("ascii"))
    started = time.monotonic()
    overall_deadline = started + EXCHANGE_MAX_SECONDS
    deadline = min(started + seconds, overall_deadline)
    while time.monotonic() < deadline:
        require_unlost()
        chunk = connection.read(EXCHANGE_READ_BYTES)
        if not chunk:
            continue
        pending.extend(chunk)
        end = pending.rfind(b"\n") + 1
        if not end:
            continue
        data = bytes(pending[:end])
        del pending[:end]
        new, complete = replies_to(data, reply, until)
        found.extend(new)
        if new:
            deadline = min(time.monotonic() + seconds, overall_deadline)
        if complete:
            return found, True, False
        require_development(data, line)
    partial, _ = replies_to(bytes(pending) + b"\n", reply, until)
    return found, False, bool(found or partial)


TUNE_REPLY_ENDS = ("TUNE_OK", "TUNE_ERR", "TUNE_END")
EXCHANGE_SECONDS = 3.0
EXCHANGE_ATTEMPTS = 3


class TuneRefused(BatchEnding):
    """A --set the board refused or did not echo."""


def exchange_retrying(connection, requests):
    """Retry an idempotent sequence only on silence; partial replies stop it."""
    for _ in range(EXCHANGE_ATTEMPTS):
        replies = []
        for line, prefix, endings in requests:
            found, complete, cut_off = exchange(connection, line, prefix, endings, EXCHANGE_SECONDS)
            if cut_off:
                return line, replies + found, False
            if not found:
                break
            replies.extend(found)
            if found[-1].startswith(endings[1]):
                return line, replies, False
        else:
            return line, replies, True
    return line, found, False


def apply_tunables(connection, settings):
    """SET each NAME=VALUE on the port the caller holds, so a capture runs at
    those values whoever held the board before it; refused unless the board
    echoes each value back."""
    for setting in settings:
        name, _, value = setting.partition("=")
        _, found, complete = exchange_retrying(
            connection, [(f"SET {name} {value}", "TUNE", TUNE_REPLY_ENDS)])
        if not complete or not found or not found[-1].startswith("TUNE_OK " + name + "="):
            raise TuneRefused(f"SET {name} {value}: {found[-1] if found else 'no reply'}")
        if found[-1].split("=", 1)[1] != str(int(value, 0)):
            raise TuneRefused(f"SET {name} {value}: board holds {found[-1]}")
        print(found[-1], flush=True)


class ConsoleRefused(BatchEnding):
    """A setup line failed or did not complete its reply."""


def apply_console_lines(connection, lines):
    """Apply an idempotent setup sequence; silence retries it from the start.

    A partial reply or an error refuses the sequence without resending a line.
    """
    requests = []
    for line in lines:
        try:
            prefix, endings = reply_prefix(line)
        except ValueError as error:
            raise ConsoleRefused(f"{line!r}: {error}") from error
        requests.append((line, prefix, endings))
    if not requests:
        return
    line, found, complete = exchange_retrying(connection, requests)
    if not complete:
        raise ConsoleRefused(f"{line!r}: " + ("\n".join(found) or "no reply"))
    print("\n".join(found), flush=True)


def send(args, store, board):
    """Print a console reply under the device lock, without creating a capture.

    Optional silence succeeds for commands that only report errors. Once a
    framed reply starts it must reach its terminator, even when optional.
    """
    with HeldLock(store, board, args.owner, "send", args.wait) as held:
        with open_when_free() as connection:
            found, complete, cut_off = exchange(connection, args.line, args.reply, args.until, args.seconds)
            if complete:
                print("\n".join(found))
                if found[-1].startswith(args.reply + "_ERR"):
                    held.error = found[-1]
                    return 1
                return 0
    if cut_off and (not args.optional or any(ending.endswith(FRAMED_ENDINGS) for ending in args.until)):
        print("\n".join(found))
        raise RuntimeError("reply cut off for '" + args.line + "': missing " + " or ".join(args.until))
    if args.optional:
        print("\n".join(found))
        return 0
    raise RuntimeError("no reply to '" + args.line + "' within " + str(args.seconds) +
                       " s - is a development build running?")


def screenshot(args, store, board):
    """SCREENSHOT, decoded by read_screenshot()/write_capture() in
    launcher/tools/device/screenshot.py, the one decoder autana's own
    `screenshot` shares. Under the device lock, so it queues behind
    whatever else already holds the board rather than fighting it for the
    port.

    Not a capture: nothing is written under records/, the same reasoning
    send()'s own docstring gives; a screenshot is a look at the screen, not
    evidence of a run.
    """
    import screenshot as screenshot_tool

    out = args.out or str(Path.cwd() / ("screenshot_" + now().strftime("%Y%m%d_%H%M%S") + ".png"))

    def report(message):
        print(message, file=sys.stderr, flush=True)

    with HeldLock(store, board, args.owner, "screenshot", args.wait):
        with open_when_free() as connection:
            png, state_json = screenshot_tool.read_screenshot(connection, args.timeout, on_status=report)

    if getattr(args, "framebuffer", False):
        image_turn_quarter = 0
        description = "framebuffer bytes"
    elif getattr(args, "as_shown", False):
        try:
            image_turn_quarter = json.loads(state_json)["orientation_quarter"] % 4
        except (KeyError, TypeError, json.JSONDecodeError):
            raise RuntimeError("the capture did not report orientation_quarter for --as-shown")
        description = "as shown"
    else:
        image_turn_quarter = screenshot_tool.DEFAULT_TURN_QUARTER
        description = "to match the board"
    png = screenshot_tool.turn_png(png, image_turn_quarter)
    png_path, state_path = screenshot_tool.write_capture(out, png, state_json, image_turn_quarter)
    degrees = image_turn_quarter * 90
    direction = "clockwise" if image_turn_quarter == 1 else "counter-clockwise"
    if image_turn_quarter == 0:
        print(f"wrote {png_path} ({description}; turned 0 degrees)")
    else:
        print(f"wrote {png_path} (turned {min(degrees, 360 - degrees)} degrees {direction} {description})")
    if state_path:
        print(f"wrote {state_path}")
    else:
        print("no SCREENSHOT_STATE line arrived - device state was not captured", file=sys.stderr)
    return 0


def mic_record(args, store, board):
    import mic as mic_tool
    with HeldLock(store, board, args.owner, "mic record", args.wait):
        with open_when_free() as connection:
            pcm, state = mic_tool.read_mic(connection, args.ms, args.settle, at=args.at)
    path = mic_tool.write_take(args.out, pcm, state)
    print(json.dumps(state) if args.json else f"wrote {path} and {path.with_suffix('.json')}")
    return 0


def touch_point(x, y):
    """The panel-frame point TAP, PRESS and DRAG take for pixel (x, y) of
    the default screenshot; ValueError outside that screenshot. The default
    screenshot is one fixed turn of the panel, so the board's orientation
    never enters."""
    import screenshot as screenshot_tool

    return screenshot_tool.panel_point(x, y, screenshot_tool.DEFAULT_TURN_QUARTER)


def batch(args, store, board):
    """Capture every suite `runs` times under ONE lock, then write one
    summary across all runs. `args.flash` builds and flashes first, as the
    point of one lock is nobody else flashing between two captures of that
    image; without it, every suite runs against whatever is already on the
    board. A capture that errors is recorded and the batch continues; a
    failed build or flash stops it, and so does a BatchEnding. One suite run
    once collapses to exactly what a standalone `run-suite` leaves behind:
    one capture under its own name, no `batch` summary or manifest row, since
    there is nothing across runs for either to tell apart."""
    extra_flags = (["--perf-scope"] if args.perf_scope else []) + layout_flags(args)
    patterns = test_patterns(getattr(args, "test_filter", None))
    if args.out and (len(args.suite) != 1 or args.runs != 1):
        raise RuntimeError("--out only makes sense with exactly one --suite and --runs 1 - "
                           "several captures cannot all land on one path")
    if (args.perf_scope or layout_flags(args)) and not args.flash:
        raise RuntimeError("--perf-scope, --layout-seed and --hot-tunables select the image built - "
                           "they need --flash")
    single = len(args.suite) == 1 and args.runs == 1
    worktree = str(Path(args.worktree).resolve())
    started_at = now()
    entries = []
    if args.flash:
        flash_args = argparse.Namespace(owner=args.owner, purpose=args.purpose + " (flash)",
                                        wait=args.wait, worktree=args.worktree,
                                        variant=args.variant, out=None)
        build_cm = build_image(flash_args, board, extra_flags)
    else:
        build_cm = contextlib.nullcontext(None)
    with build_cm as built, \
            HeldLock(store, board, args.owner, "batch", args.wait) as held:
        if args.flash:
            build_id, connection = write_image(built, store, board, held)
            if args.expect_build_id and build_id != args.expect_build_id:
                connection.close()
                raise RuntimeError(
                    f"build id mismatch: expected {args.expect_build_id}, flashed {build_id} - "
                    "refusing to run any suite")
            if VARIANT_CONSOLE[args.variant]:
                await_console(flash_args, board, build_id, held.held["acquired_at"], connection)
            else:
                connection.close()
        else:
            build_id = args.expect_build_id
        commit = git_commit(worktree)
        for run in range(1, args.runs + 1):
            for suite_name in args.suite:
                capture_at = now()
                if single:
                    out, purpose = args.out, args.purpose
                else:
                    resolved, _ = resolve_capture_path(
                        args.out, "runsuite-" + suite_name + "-run" + str(run), args.owner,
                        capture_at)
                    out, purpose = str(resolved), f"{args.purpose} ({suite_name} run {run}/{args.runs})"
                suite_args = argparse.Namespace(
                    owner=args.owner, wait=args.wait, suite=suite_name, out=out, purpose=purpose,
                    max_seconds=args.max_seconds, idle_seconds=args.idle_seconds,
                    expect_build_id=build_id, verbose=getattr(args, "verbose", False),
                    test_filter=patterns, tune_set=getattr(args, "tune_set", None),
                    console_send=getattr(args, "console_send", None))
                print(f"batch: {suite_name} run {run}/{args.runs}", flush=True)
                # A suite FAIL is a result, not a broken run: a perf capture always
                # carries its budget targets' FAILs, and its duration still counts.
                error = None
                failed = False
                try:
                    failed = bool(run_suite(suite_args, store, board, held_lock=held,
                                            worktree=worktree, commit=commit))
                except BatchEnding:
                    # The same board would answer the same in every remaining run.
                    raise
                except RuntimeError as caught:
                    error = str(caught)
                    print("batch: capture error, continuing: " + error, file=sys.stderr)
                entries.append({"suite": suite_name, "run": run, "capture": out,
                                "error": error, "failed": failed})
        if any(entry["error"] for entry in entries):
            held.error = "a batch capture failed"
    if single:
        entry = entries[0]
        return 1 if (entry["error"] or entry["failed"]) else 0
    meta = {"build_id": build_id, "owner": args.owner, "purpose": args.purpose,
            "runs": args.runs, "worktree": worktree, "commit": commit,
            "test_filter": patterns}
    summary_path, _ = resolve_capture_path(None, "batch", args.owner, started_at)
    summary_path = summary_path.with_suffix(".md")
    summary_path.write_text(device_report.batch_summary_markdown(entries, meta),
                            encoding="utf-8")
    append_manifest({"started_at": started_at.isoformat(), "board": board, "owner": args.owner,
                     **({"test_filter": patterns} if patterns else {}),
                     "acquired_at": datetime.fromtimestamp(held.held["acquired_at"]).isoformat(),
                     "purpose": args.purpose, "command": "batch", "suite": ",".join(args.suite),
                     "build_id": build_id, "worktree": worktree, "commit": meta["commit"],
                     "reason": None,
                     "error": "; ".join(e["error"] for e in entries if e["error"]) or None,
                     "capture_path": str(summary_path),
                     "capture_bytes": summary_path.stat().st_size})
    print("batch summary: " + str(summary_path))
    return 1 if any(e["error"] or e["failed"] for e in entries) else 0


WAIT_UNIT_SECONDS = {"s": 1, "m": 60, "h": 60 * 60}


def human_wait_seconds(value):
    try:
        unit = value[-1:]
        seconds = float(value[:-1] if unit in WAIT_UNIT_SECONDS else value) * WAIT_UNIT_SECONDS.get(unit, 1)
    except ValueError as error:
        raise argparse.ArgumentTypeError("wait must be a nonnegative number") from error
    if not math.isfinite(seconds) or seconds < 0:
        raise argparse.ArgumentTypeError("wait must be a nonnegative finite number")
    return seconds


def hold_seconds(value):
    seconds = human_wait_seconds(value)
    if seconds > device_lock.HOLD_MAX_SECONDS:
        raise argparse.ArgumentTypeError("hold exceeds one hour; use --until-released")
    return seconds


def wait_for_human_release(store, board, reservation_id, seconds):
    deadline = time.monotonic() + seconds
    try:
        while True:
            status = store.status(board)
            human = status["human"]
            if human is None:
                expired = status.get("expired_human")
                print("human reservation expired" if expired and expired.get("id") == reservation_id
                      else "human reservation released")
                return 0
            if human.get("id") != reservation_id:
                print("human reservation replaced")
                return device_lock.EXIT_BUSY
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print("human reservation wait timed out; the reservation stands")
                return device_lock.EXIT_BUSY
            time.sleep(min(1.0, remaining))
    except KeyboardInterrupt:
        print("human reservation wait interrupted")
        return EXIT_INTERRUPTED


def board_statuses(store, board=None):
    """Every board a lock record names or USB shows, or only `board`."""
    plugged = {found.serial: found.port for found in plugged_boards()}
    for serial in plugged:
        store.note_seen(serial)
    boards = [board] if board else sorted(set(store.boards()) | set(plugged))
    return [device_lock.status_entry(store, name, plugged.get(name))
            for name in boards]


def pick_board(entries):
    if not entries:
        raise NoBoard("no board available")
    free = [entry for entry in entries if device_lock.availability(entry) == "free"]
    if free:
        return min(free, key=lambda entry: entry["board"])["board"], "free, nobody queued"
    available = [entry for entry in entries if device_lock.availability(entry) != "human"] or entries
    estimated = [entry for entry in available if entry["estimated_start"] is not None]
    if estimated:
        chosen = min(estimated, key=lambda entry: (entry["estimated_start"], entry["board"]))
        return chosen["board"], "earliest estimated start time"
    chosen = min(available, key=lambda entry: (len(entry["waiting"]), entry["board"]))
    return chosen["board"], "shortest queue"


def board_heading(entry):
    return f"{short_id(entry['board'])} {entry['board']} {entry['port'] or 'not on USB'}"


def print_devices(entries):
    if not entries:
        print("no board found")
    for entry in entries:
        holder = entry["holder"]
        available = device_lock.availability(entry)
        if available == "human":
            state = f"reserved by a human ({holder['owner']}) until {device_lock.format_estimate(entry['expires_at'])}"
        elif available == "held":
            state = f"held by {holder['owner']} until {device_lock.format_estimate(entry['estimated_free'])}"
        else:
            state = available
        print(f"{board_heading(entry)}: {state}")


def print_statuses(entries, now):
    if not entries:
        print("no board found")
    for entry in entries:
        print(board_heading(entry))
        for line in device_lock.status_lines(entry, now):
            print("  " + line)


def main(argv=None):
    install_console_interrupt_handler()
    try:
        autana_config.load()
    except autana_config.ConfigError as error:
        print("device: " + str(error), file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser()
    parser.add_argument("--board", "-b", help="full USB serial or short id (default: the only "
                                        "board plugged in, else the only one "
                                        "a lock record names; hand-to-human/take-back also try "
                                        "the only board this machine has ever seen)")
    parser.add_argument("--owner", default=device_lock.default_owner())
    parser.add_argument("--wait", type=float, default=autana_config.DEFAULT_LOCK_WAIT_SECONDS)
    parser.set_defaults(auto_pick=False)
    subparsers = parser.add_subparsers(dest="command", required=True)
    status_parser = subparsers.add_parser("status")
    status_parser.add_argument("--json", action="store_true")
    devices_parser = subparsers.add_parser("devices")
    devices_parser.add_argument("--json", action="store_true")
    subparsers.add_parser("resolve-port")
    release = subparsers.add_parser("release")
    release.add_argument("--token", required=True)
    hand = subparsers.add_parser("hand-to-human")
    hand.add_argument("--note", required=True)
    hand.add_argument("--until-back", type=human_wait_seconds)
    take = subparsers.add_parser("take")
    take.set_defaults(auto_pick=True)
    duration = take.add_mutually_exclusive_group()
    duration.add_argument("--for", dest="seconds", type=hold_seconds)
    duration.add_argument("--until-released", action="store_true")
    take.add_argument("--pid", type=int)
    take.add_argument("--estimate", type=human_wait_seconds)
    take.add_argument("note", nargs="+")
    run = subparsers.add_parser("run")
    run.set_defaults(auto_pick=True)
    run.add_argument("--heavy", action="store_true")
    run.add_argument("--wait", type=float, default=argparse.SUPPRESS)
    run.add_argument("--keep-on-failure", type=hold_seconds)
    run.add_argument("--estimate", type=human_wait_seconds)
    run.add_argument("child", nargs=argparse.REMAINDER)
    wait_parser = subparsers.add_parser("wait-free")
    wait_parser.add_argument("--cap", type=human_wait_seconds)
    subparsers.add_parser("take-back")
    check_token = subparsers.add_parser(
        "check-token", help="exit 0 when --token is the live lock on the board, else 1")
    check_token.add_argument("--token", required=True)
    flash_parser = subparsers.add_parser("flash")
    flash_parser.set_defaults(auto_pick=True)
    flash_parser.add_argument("--variant", choices=("dev", "diag", "release"), required=True)
    flash_parser.add_argument("--worktree", required=True)
    flash_parser.add_argument("--out")
    flash_parser.add_argument("--hot-tunables", action="store_true",
                              help="build with live hot-path tunables")
    flash_parser.add_argument("--perf-scope", action="store_true",
                              help="with --variant diag: build the perf-scoped image")
    flash_parser.add_argument("--frame-laps", action="store_true",
                              help="with --perf-scope: time the phases inside the frame stages")
    flash_parser.add_argument("--layout-seed", type=int, default=0,
                              help="pad the layout by this seed (0 is the plain build)")
    suite = subparsers.add_parser("run-suite")
    suite.add_argument("suite")
    suite.add_argument("--out")
    suite.add_argument("--max-seconds", type=float, default=180)
    # A perf row can run silently for minutes, and a freshly flashed diag build
    # runs every suite at boot before serving RUNSUITE; an 8 s default ended a
    # perf capture after three tests and read as "the rows are missing".
    suite.add_argument("--idle-seconds", type=float, default=300)
    suite.add_argument("--expect-build-id")
    suite.add_argument("--verbose", action="store_true")
    suite.add_argument("--send", dest="console_send", action="append", metavar="LINE",
                       help="send before every capture; repeatable, sequence should be idempotent")
    suite.add_argument("--set", dest="tune_set", action="append", metavar="NAME=VALUE",
                       help="SET a tunable under the capture's lock first; repeatable")
    listen_parser = subparsers.add_parser("listen")
    listen_duration = listen_parser.add_mutually_exclusive_group(required=True)
    listen_duration.add_argument("--seconds", type=float)
    listen_duration.add_argument("--follow", action="store_true")
    listen_parser.add_argument("--echo", action="store_true")
    listen_parser.add_argument("--out")
    listen_parser.add_argument("--elf",
                               help="decode any crash addresses seen against this .elf's symbols")
    coredump_parser = subparsers.add_parser(
        "coredump", help="decode the last panic's core dump from flash, or erase it")
    coredump_source = coredump_parser.add_mutually_exclusive_group()
    coredump_source.add_argument("--erase", action="store_true")
    coredump_source.add_argument("--core", help="decode this saved raw dump; no board")
    coredump_parser.add_argument("--elf", help="decode against this .elf instead of the crashed build's")
    reset_parser = subparsers.add_parser("reset", help="reboot the board and wait for USB serial")
    reset_parser.add_argument("--capture", action="store_true",
                              help="capture the boot console after the reset")
    reset_parser.add_argument("--verbose", action="store_true")
    reset_parser.add_argument("--seconds", type=float, default=20.0,
                              help="boot capture window (default: 20)")
    reset_parser.add_argument("--out")
    selftest_parser = subparsers.add_parser(
        "selftest", help="flash the diagnostics+autorun image and capture the on-device run "
                         "of every registered suite")
    selftest_parser.set_defaults(auto_pick=True)
    selftest_parser.add_argument("--worktree", required=True)
    selftest_parser.add_argument("--out")
    selftest_parser.add_argument("--verbose", action="store_true")
    selftest_parser.add_argument("--hot-tunables", action="store_true",
                              help="build with live hot-path tunables")
    selftest_parser.add_argument("--perf-scope", action="store_true",
                                 help="build the perf-scoped image")
    # 3000 s leaves headroom over a full run's measured time, see
    # launcher/tools/quality/report_test_results.sh.
    selftest_parser.add_argument("--max-seconds", type=float, default=3000)
    selftest_parser.add_argument("--idle-seconds", type=float, default=300)
    send_parser = subparsers.add_parser(
        "send", help="write one console line and print the device's replies to it")
    send_parser.add_argument("line")
    send_parser.add_argument("--reply", default="TUNE",
                             help="what a reply line contains (default: TUNE)")
    send_parser.add_argument("--until", action="append",
                             help="a reply prefix that ends the answer; repeatable "
                                  "(default: TUNE_OK, TUNE_ERR, TUNE_END)")
    send_parser.add_argument("--seconds", type=float, default=EXCHANGE_SECONDS,
                             help=f"reply idle limit in seconds (overall cap: {EXCHANGE_MAX_SECONDS:g} s)")
    send_parser.add_argument("--optional", action="store_true",
                             help="a timeout with nothing seen is success, not an error - "
                                  "for a verb that only answers when something is wrong")
    screenshot_parser = subparsers.add_parser(
        "screenshot", help="capture what the panel shows right now, as a .png plus a .json state snapshot")
    screenshot_parser.add_argument("--out",
                                   help="output path; any extension given is replaced with .png "
                                        "(default: a timestamped name in the current directory)")
    screenshot_parser.add_argument("--timeout", type=float, default=90.0)
    screenshot_view = screenshot_parser.add_mutually_exclusive_group()
    screenshot_view.add_argument("--as-shown", action="store_true")
    screenshot_view.add_argument("--framebuffer", action="store_true")
    mic_parser = subparsers.add_parser("mic-record", help="record microphone PCM and state")
    mic_parser.add_argument("ms", type=int)
    mic_parser.add_argument("--settle", type=int)
    mic_parser.add_argument("--at", nargs="+", action="append",
                             help="offset ms from take start; negative offsets fall in settling; "
                                  "followed by reg value or tone hz duration_ms gain",
                             type=lambda word: word if word == "tone" else int(word, 0), default=[])
    mic_parser.add_argument("--out", required=True)
    mic_parser.add_argument("--json", action="store_true")
    batch_parser = subparsers.add_parser(
        "batch", help="capture suites N times under one lock, write one summary")
    batch_parser.set_defaults(auto_pick=lambda args: args.flash)
    batch_parser.add_argument("--worktree", required=True)
    batch_parser.add_argument("--variant", choices=("dev", "diag", "release"), default="diag")
    batch_parser.add_argument("--suite", action="append", required=True,
                              help="a suite to capture; repeat for several")
    batch_parser.add_argument("--runs", type=int, default=3)
    batch_parser.add_argument("--verbose", action="store_true")
    batch_parser.add_argument("--no-flash", dest="flash", action="store_false", default=True,
                              help="capture against the image already on the board")
    batch_parser.add_argument("--hot-tunables", action="store_true",
                              help="build with live hot-path tunables")
    batch_parser.add_argument("--perf-scope", action="store_true",
                              help="build the perf-scoped image (needs --flash, the default)")
    batch_parser.add_argument("--layout-seed", type=int, default=0,
                              help="pad the layout by this seed (needs --flash, the default)")
    batch_parser.add_argument("--max-seconds", type=float, default=1800)
    batch_parser.add_argument("--send", dest="console_send", action="append", metavar="LINE",
                              help="send before every capture; repeatable, sequence should be idempotent")
    batch_parser.add_argument("--set", dest="tune_set", action="append", metavar="NAME=VALUE",
                              help="SET a tunable under each capture's lock first; repeatable")
    batch_parser.add_argument("--test", dest="test_filter", action="append", metavar="PATTERN",
                              help="run only the tests whose name contains PATTERN; repeat or "
                                   "comma-separate for several")
    batch_parser.add_argument("--idle-seconds", type=float, default=300)
    batch_parser.add_argument("--out",
                              help="write the one capture here instead of the default path - "
                                   "only with exactly one --suite and --runs 1")
    batch_parser.add_argument("--expect-build-id",
                              help="refuse to run any suite unless the image just flashed "
                                   "carries this BUILD_ID")
    report_parser = subparsers.add_parser("report")
    report_parser.add_argument("capture", help="an existing capture file (.log or .log.gz)")
    report_parser.add_argument("--index", help="override index.jsonl (default: records/device)")
    args = parser.parse_args(argv)
    args.purpose = command_label(args.command)
    os.environ[autana_config.WAIT_ENV] = str(args.wait)

    # report and coredump --core touch no lock and no board; each only reads
    # a file already on disk, so they are handled before board discovery even
    # runs, unlike every command below this.
    if args.command == "coredump" and args.core:
        return decode_coredump(args.core, args.elf)
    if args.command == "report":
        index_path = Path(args.index) if args.index else records_root() / "index.jsonl"
        try:
            report_path = device_report.write_report_for_capture(Path(args.capture), index_path)
        except Exception as error:
            print("device: report generation failed: " + str(error), file=sys.stderr)
            return 1
        print("report: " + str(report_path))
        return 0

    picked_reason = None
    result = 1
    try:
        store = device_lock.LockStore()
        if args.command in ("status", "devices"):
            entries = board_statuses(store, chosen_board(args.board, store))
            try:
                heavy = device_lock.status_entry(store, device_lock.HEAVY)
            except OSError as error:
                heavy = {"board": device_lock.HEAVY, "state": "unavailable", "reason": str(error)}
            if args.json:
                print(json.dumps({"boards": entries, "heavy": heavy}))
            elif args.command == "devices":
                print_devices(entries)
            else:
                print_statuses(entries, store.now())
                lines = (["unavailable: " + heavy["reason"]] if heavy["state"] == "unavailable" else
                         device_lock.status_lines(heavy, store.resource_now(device_lock.HEAVY)))
                print("heavy: " + "\n  ".join(lines))
            result = 0
            return result
        if args.command == "resolve-port":
            print(find_board(args.board).port)
            result = 0
            return result
        if args.command == "run":
            child = args.child[1:] if args.child[:1] == ["--"] else args.child
            if not child:
                parser.error("run needs -- <command...>")
            if args.heavy:
                board = device_lock.HEAVY
                if args.keep_on_failure is not None:
                    parser.error("--heavy does not support --keep-on-failure")
            else:
                board, picked_reason = board_for_lock(store, args.board, auto_pick=True)
                if os.environ.get(TOKEN_ENV) and args.keep_on_failure is not None:
                    parser.error("--keep-on-failure is unavailable inside another hold")
            with HeldLock(store, board, args.owner, "run", args.wait, estimate=args.estimate) as held:
                try:
                    run_to_end(child, held.lost)
                    result = 0
                    return result
                except subprocess.CalledProcessError as error:
                    held.error = str(error)
                    held.keep_seconds = args.keep_on_failure
                    result = error.returncode
                    return result
                except KeyboardInterrupt:
                    result = EXIT_INTERRUPTED
                    return result
        board, picked_reason = board_for_lock(
            store, args.board, remembered=args.command in ("hand-to-human", "take-back"),
            auto_pick=args.auto_pick(args) if callable(args.auto_pick) else args.auto_pick,
            token=getattr(args, "token", None))
        token = os.environ.get(TOKEN_ENV)
        if token and args.command in ("take", "hand-to-human", "wait-free"):
            record = store.live_lock(board, token)
            if record:
                raise TokenRefused("already inside hold " + record["owner"])
            raise TokenRefused("lock token is not live on this board (released, lapsed or wrong); "
                               f"unset {TOKEN_ENV} in this shell or drop --lock-token")
        # Validate a borrowed token before any build starts.
        if token and args.command not in ("release", "check-token", "take", "hand-to-human", "take-back", "wait-free"):
            if not store.check_token(board, token):
                raise TokenRefused("lock token is not live on this board (released, lapsed or wrong)")
        if args.command == "wait-free":
            deadline = None if args.cap is None else time.monotonic() + args.cap
            while True:
                state = store.status(board)
                if not state["lock"] and not state["human"]:
                    result = 0
                    return result
                if deadline is not None and time.monotonic() >= deadline:
                    result = device_lock.EXIT_BUSY
                    return result
                time.sleep(device_lock.WAIT_POLL_SECONDS if deadline is None else
                           min(device_lock.WAIT_POLL_SECONDS, max(0, deadline - time.monotonic())))
        if args.command == "release":
            result = 0 if store.release(board, args.token) else 1
            return result
        if args.command == "check-token":
            result = 0 if store.check_token(board, args.token) else 1
            return result
        if args.command == "hand-to-human":
            notice_state = {}
            reservation_id, renewed = store.reserve_human(board, args.owner, args.note,
                                                          wait=args.wait, on_wait=lambda ticket: wait_notice(store, board, notice_state, ticket))
            if reservation_id is None:
                raise LockBusy("device lock was not acquired: " + device_lock.busy_text(store.status(board), store.resource_now(board)))
            if args.until_back is None:
                lasts = device_lock.duration_text(device_lock.HUMAN_RESERVATION_SECONDS)
                print(f"human reservation {'renewed' if renewed else 'recorded'}; it expires "
                      f"in {lasts} unless renewed with `autana lock hand`")
                result = 0
                return result
            result = wait_for_human_release(store, board, reservation_id, args.until_back)
            return result
        if args.command == "take-back":
            store.end_human(board)
            print_statuses(board_statuses(store, board), store.now())
            result = 0
            return result
        if args.command == "take":
            if args.pid is not None and (not args.until_released or args.pid <= 0):
                parser.error("--pid needs --until-released and a positive pid")
            if args.until_released and args.pid is None:
                parser.error("--until-released requires --pid")
            notice_state = {}
            held = store.acquire(board, args.owner, "take", wait=args.wait,
                                 on_wait=lambda ticket: wait_notice(store, board, notice_state, ticket), purpose=" ".join(args.note))
            if not held:
                raise LockBusy("device lock was not acquired: " + device_lock.busy_text(store.status(board), store.resource_now(board)))
            pid = args.pid if args.until_released else None
            seconds = None if args.until_released else (args.seconds if args.seconds is not None
                                                       else device_lock.HOLD_MAX_SECONDS)
            if not store.hold(board, held["token"], " ".join(args.note), seconds=seconds, pid=pid, estimate=args.estimate):
                raise LockBusy("device lock was lost before the hold was recorded")
            print(held["token"])
            result = 0
            return result
        if args.command == "flash":
            extra_flags = (["--perf-scope"] if args.perf_scope else []) + layout_flags(args)
            with build_image(args, board, extra_flags) as built, \
                    HeldLock(store, board, args.owner, "flash", args.wait) as held:
                build_id, connection = write_image(built, store, board, held)
                if VARIANT_CONSOLE[args.variant]:
                    await_console(args, board, build_id, held.held["acquired_at"], connection)
                else:
                    connection.close()
        elif args.command == "send":
            if not args.until:
                args.until = [args.reply + "_OK", args.reply + "_ERR", args.reply + "_END"]
            result = send(args, store, board)
            return result
        else:
            # Looked up per call, so a test's patch of one of them is the one run.
            board_commands = {"run-suite": run_suite, "selftest": selftest, "coredump": coredump,
                              "reset": reset_device, "batch": batch, "screenshot": screenshot, "mic-record": mic_record}
            result = board_commands.get(args.command, listen)(args, store, board)
            return result
        result = 0
        return result
    except (LockBusy, TimeoutError) as error:
        print("device: " + str(error), file=sys.stderr)
        result = device_lock.EXIT_BUSY
        return result
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("device: " + str(error), file=sys.stderr)
        result = 1
        return result
    finally:
        if picked_reason:
            verdict = "PASS" if result == 0 else f"FAIL ({result})"
            print(f"{verdict}: board {board_label(board)}", file=sys.stderr)


if __name__ == "__main__":
    install_console_interrupt_handler()
    rerun = rerun_under_idf_python(sys.argv[1:])
    raise SystemExit(main() if rerun is None else rerun)
