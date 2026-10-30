"""Shared FIFO lock state for boards and heavy host work."""

import contextlib
import datetime
import errno
import getpass
import json
import math
import os
import re
import socket
import weakref
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

import device_hook

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import autana_config  # noqa: E402  (path must be set up first)
from process_tree import terminate_unwinds  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "autana"))
from version import __version__  # noqa: E402  (path must be set up first)


# A holder is reclaimed when its heartbeat is this old; the holder renews it
# every HeldLock.HEARTBEAT_SECONDS in device.py.
DEFAULT_STALE_SECONDS = 600
EXIT_BUSY = autana_config.EXIT_BUSY
# guard() is an O_CREAT|O_EXCL file created and deleted around each
# read-modify-write, so a crash inside one leaves it behind: past this age
# the next taker removes it instead of waiting for it forever.
GUARD_STALE_SECONDS = 30
# The mutex is guard() below (an O_CREAT|O_EXCL file), not an OS byte-range
# lock; the JSON files are the state it protects, not locks themselves. This
# numbers the state shape. Differences are shown, never refused for running
# commands. Readers older than protocol 4 cannot distinguish Windows and WSL
# pid namespaces and may reclaim a live cross-OS holder or prune its waiters.
# Update every copy sharing a root.
LOCK_PROTOCOL = 5
HEAVY = "heavy"
HELD_HEAVY = weakref.WeakValueDictionary()
HOLD_MAX_SECONDS = 3600
WAIT_POLL_SECONDS = 0.2
FILETIME_EPOCH_OFFSET = 116444736000000000
FILETIME_TICKS_PER_SECOND = 10000000
# A person's reservation has no heartbeat; `hand` renews its expiry.
HUMAN_RESERVATION_SECONDS = HOLD_MAX_SECONDS
CALLER_PID_ENV = "_AUTANA_CALLER_PID"
PROCESS_BASIC_INFORMATION = 0
PROCESS_COMMAND_LINE_INFORMATION = 60
WINDOWS_AUTANA_HOST = re.compile(
    r'^\s*(?:'
    r'(?:"[^"\n]*[\\/]cmd\.exe"|(?:\S*[\\/])?cmd\.exe)\s+'
    r'(?:/[^c\s]+\s+)*/c\s+"*(?:[^"\r\n]*[\\/])?autana\.cmd(?:"|\s|$)'
    r'|(?:"(?:[^"\n]*[\\/])?python[\d.]*(?:\.exe)?"|(?:\S*[\\/])?python[\d.]*(?:\.exe)?)\s+'
    r'"?[^"\r\n]*[\\/]scripts[\\/]autana[\\/]autana\.py(?:"|\s|$)'
    r'|(?:"[^"\n]*[\\/]bash(?:\.exe)?"|(?:\S*[\\/])?bash(?:\.exe)?)\s+'
    r'"?[^"\r\n]*[\\/]tools[\\/]autana(?:"|\s|$))', re.IGNORECASE)
# Core fields identify holders and FIFO order across protocols. Ticket liveness
# reads touched_at, falling back to created_at when absent. Optional metadata
# uses .get() so mixed-protocol records remain readable.
PROTOCOL_CORE_FIELDS = ("owner", "pid", "host", "heartbeat_at", "ticket", "sequence", "board", "protocol")


def default_root():
    """One directory per account and machine, shared by every checkout and
    session. Windows: the account's own temp folder. Elsewhere a fixed
    /tmp folder per uid, never $TMPDIR: two jobs with different TMPDIRs must
    still exclude each other. A Linux autana older than this one used
    $TMPDIR/autana-device, so mixed installs do not exclude each other.

    _AUTANA_DEVICE_LOCK_ROOT is test only: a per-checkout or per-shell root
    would split the one lock every checkout on the machine must share."""
    named = os.environ.get("_AUTANA_DEVICE_LOCK_ROOT")
    if named:
        return Path(named)
    if os.name == "nt":
        return Path(tempfile.gettempdir()) / "autana-device"
    return Path("/tmp") / f"autana-device-{os.getuid()}"


def is_wsl():
    if os.name == "nt":
        return False
    if Path("/proc/sys/fs/binfmt_misc/WSLInterop").exists():
        return True
    try:
        return "microsoft" in Path("/proc/version").read_text().lower()
    except OSError:
        return False


def host_identity():
    return socket.gethostname() + ("/wsl" if is_wsl() else "")


def heavy_root():
    """The host-wide root must be visible from both Windows and WSL."""
    named = os.environ.get("_AUTANA_HEAVY_LOCK_ROOT")
    if named:
        return Path(named)
    if os.name == "nt":
        return Path(os.environ.get("ProgramData", r"C:\ProgramData")) / "autana" / HEAVY
    if is_wsl():
        shared = Path("/mnt/c/ProgramData")
        if not shared.is_dir():
            raise OSError("WSL heavy lock root is unreachable: " + str(shared))
        return shared / "autana" / HEAVY
    return Path("/tmp/autana-heavy")


def board_read_root():
    """The WSL systemd GPU runner resolves Windows temp through PowerShell.
    The interactive Windows board runner uses that same Windows account,
    so this read-only root exposes its live board locks.
    """
    if os.environ.get("_AUTANA_DEVICE_LOCK_ROOT") or not is_wsl():
        return default_root()
    windows_temp = subprocess.check_output(
        ["powershell.exe", "-NoProfile", "-Command", "[System.IO.Path]::GetTempPath()"],
        text=True, timeout=30).strip()
    shared = Path(subprocess.check_output(["wslpath", "-u", windows_temp], text=True, timeout=30).strip())
    if not shared.is_dir():
        raise OSError("WSL board lock root is unreachable: " + str(shared))
    return shared / "autana-device"


def default_owner():
    """What a caller that names no owner is called in the lock:
    "<user>@<host>:<pid>". getpass.getuser() can fail with no username in the
    environment (a container); "user" then stands in for it."""
    try:
        user = getpass.getuser()
    except OSError:
        user = "user"
    return f"{user}@{host_identity()}:{os.getpid()}"


def bound_owner(owner, pid):
    """Name the process an until-released hold is bound to."""
    label, separator, suffix = owner.rpartition(":")
    if separator and suffix.isdecimal():
        owner = label
    return f"{owner}:{pid}"


def caller_pid(explicit=None):
    """The invoking process outside the Windows launcher and interpreter shims."""
    if explicit:
        return int(explicit)
    parent = os.getppid()
    if os.name != "nt":
        return parent
    import ctypes
    from ctypes import wintypes

    class BasicInformation(ctypes.Structure):
        _fields_ = [("reserved", ctypes.c_void_p), ("peb", ctypes.c_void_p),
                    ("reserved_pair", ctypes.c_void_p * 2), ("pid", ctypes.c_size_t),
                    ("parent", ctypes.c_size_t)]

    class UnicodeString(ctypes.Structure):
        _fields_ = [("length", wintypes.USHORT), ("maximum", wintypes.USHORT),
                    ("buffer", ctypes.c_void_p)]

    kernel = windows_kernel32()
    query = ctypes.WinDLL("ntdll").NtQueryInformationProcess
    query.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                      wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
    query.restype = wintypes.LONG
    while parent:
        handle = kernel.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, parent)
        if not handle:
            raise OSError("could not find the invoking shell; use --pid")
        try:
            basic = BasicInformation()
            needed = wintypes.ULONG()
            if query(handle, PROCESS_BASIC_INFORMATION, ctypes.byref(basic),
                     ctypes.sizeof(basic), ctypes.byref(needed)) < 0:
                raise OSError("could not read the invoking process parent; use --pid")
            query(handle, PROCESS_COMMAND_LINE_INFORMATION, None, 0, ctypes.byref(needed))
            if not needed.value:
                raise OSError("could not read the invoking process command line; use --pid")
            buffer = ctypes.create_string_buffer(needed.value)
            if query(handle, PROCESS_COMMAND_LINE_INFORMATION, buffer, len(buffer), ctypes.byref(needed)) < 0:
                raise OSError("could not read the invoking process command line; use --pid")
            text = UnicodeString.from_buffer(buffer)
            command = ctypes.wstring_at(text.buffer, text.length // ctypes.sizeof(wintypes.WCHAR))
            if not WINDOWS_AUTANA_HOST.match(command):
                return parent
            parent = basic.parent
        finally:
            kernel.CloseHandle(handle)
    raise OSError("invoking shell exited; use --pid")


def write_json(path, value, indent=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text(json.dumps(value, sort_keys=True, indent=indent) + "\n", encoding="utf-8")
    # Windows refuses to replace a file another process has open, and
    # boards() reads without the guard.
    for _ in range(250):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            time.sleep(0.02)
    os.replace(temporary, path)


def normalise_board(serial):
    """A board's USB serial number as every lock file and record spells it."""
    return serial.strip().upper()


SHORT_ID_DIGITS = 4


def short_id(serial):
    return normalise_board(serial).replace(":", "")[-SHORT_ID_DIGITS:]


def board_label(serial):
    return f"{short_id(serial)} ({normalise_board(serial)})"


def process_alive(pid):
    """Only a process demonstrably absent or exited counts as dead. Every
    other outcome answers alive: this decides whether one owner may take the board
    from another, and a wrongly reclaimed lock corrupts somebody's capture
    while a wrongly held one only waits out its heartbeat."""
    if pid == os.getpid():
        return True
    if pid <= 0:
        return False
    if os.name == "nt":
        return windows_process_alive(pid)
    return posix_process_alive(pid)


LINUX_EXITED_PROCESS_STATES = frozenset(("Z", "X"))


def linux_stat_fields(pid):
    """Fields after /proc's parenthesised command name, or None if unreadable."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8", errors="replace")
        return stat[stat.rindex(")") + 2:].split() or None
    except (OSError, ValueError):
        return None


def posix_process_alive(pid, kill=None):
    """Only proven absence makes a holder dead. Linux zombies and exited
    processes cannot own the board; an unreadable stat leaves them alive."""
    kill = kill or os.kill
    try:
        kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True
    if sys.platform.startswith("linux"):
        fields = linux_stat_fields(pid)
        if fields:
            return fields[0] not in LINUX_EXITED_PROCESS_STATES
    return True


ERROR_ACCESS_DENIED = 5
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259


def windows_kernel32():
    """kernel32 with the calls the process table and job code share typed:
    an untyped HANDLE is truncated to 32 bits on a 64-bit build."""
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    return kernel32


def windows_process_alive(pid, kernel32=None):
    """Never os.kill(pid, 0) on Windows: signal 0 there is CTRL_C_EVENT, so
    CPython calls GenerateConsoleCtrlEvent and treats the pid as a console
    process group. For a process on another console that fails with
    ERROR_INVALID_PARAMETER: a live holder reads as dead and its lock is
    taken, and for one sharing the caller's console it delivers Ctrl+C.
    Asks the process table instead: a pid that cannot be opened for any
    reason but access denied is gone, and an opened one is alive until it
    reports an exit code."""
    import ctypes
    from ctypes import wintypes

    if kernel32 is None:
        kernel32 = windows_kernel32()
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


def windows_process_start(pid):
    """When `pid` began, in epoch seconds, or None if it cannot be read."""
    import ctypes
    from ctypes import wintypes

    kernel32 = windows_kernel32()
    kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        created, unused_exit, unused_kernel, unused_user = (wintypes.FILETIME() for _ in range(4))
        if not kernel32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(unused_exit),
                                        ctypes.byref(unused_kernel), ctypes.byref(unused_user)):
            return None
        ticks = (created.dwHighDateTime << 32) | created.dwLowDateTime
        return (ticks - FILETIME_EPOCH_OFFSET) / FILETIME_TICKS_PER_SECOND
    finally:
        kernel32.CloseHandle(handle)


def posix_process_start(pid):
    """When `pid` began, in epoch seconds, or None where /proc cannot say."""
    fields = linux_stat_fields(pid)
    if fields is None:
        return None
    try:
        with open("/proc/stat", encoding="utf-8") as stream:
            boot = next(int(line.split()[1]) for line in stream if line.startswith("btime"))
        # Field 22, counted from after the parenthesised command name.
        ticks = int(fields[19])
        return boot + ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError, StopIteration):
        return None


def process_start(pid):
    """When the process began, in epoch seconds, or None if unavailable."""
    return windows_process_start(pid) if os.name == "nt" else posix_process_start(pid)


def human_expires_at(human):
    """When a reservation lapses; None for a record with nothing to compute it
    from, which never lapses. A record written before reservations expired has
    no expires_at and lapses an hour after it began."""
    expires = human.get("expires_at")
    if expires is None and human.get("since_at") is not None:
        expires = human["since_at"] + HUMAN_RESERVATION_SECONDS
    return expires


def human_left_text(human, now):
    """'59m left', or '' for a reservation that never lapses."""
    expires = human_expires_at(human)
    return "" if expires is None else duration_text(expires - now) + " left"


def purpose_of(kind):
    """What a lock says it is doing when nothing more specific was given."""
    return f"autana {kind}"


class Held(dict):
    """The lock record a claim wrote, plus the one-shot line for the person
    who took the board from a dead or stale holder, not part of the record."""

    def __init__(self, record, log=""):
        super().__init__(record)
        self.log = log


class LockStore:
    """A key is a board's USB serial number or the host-wide HEAVY resource."""

    def __init__(self, root=None, now=time.time, is_alive=process_alive, *, board_observer=False):
        self.root = Path(root) if root else default_root()
        self.explicit_root = root is not None
        self.board_observer = board_observer
        self.now = now
        self.is_alive = is_alive

    def key_root(self, board):
        return heavy_root() if board == HEAVY and not self.explicit_root else self.root

    def resource_now(self, board):
        if board != HEAVY and not self.board_observer:
            return self.now()
        root = self.key_root(board)
        root.mkdir(parents=True, exist_ok=True)
        # File creation uses the shared filesystem clock even when WSL's clock drifts.
        with tempfile.NamedTemporaryFile(prefix=".clock-", dir=root) as probe:
            return os.fstat(probe.fileno()).st_mtime

    def stem(self, board):
        return board.replace("/", "_").replace("\\", "_").replace(":", "_")

    def key_path(self, board, suffix):
        return self.key_root(board) / (self.stem(board) + suffix)

    def lock_path(self, board):
        return self.key_path(board, ".json")

    def queue_dir(self, board):
        return self.key_path(board, ".queue")

    def guard_path(self, board):
        return self.key_path(board, ".guard")

    def seen_path(self, board):
        return self.key_path(board, ".seen.json")

    def last_path(self, board):
        return self.key_path(board, ".last.json")

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

    def boards(self):
        """Every board a lock, reservation or waiter in this root names."""
        paths = ([path for path in self.root.glob("*.json")
                  if not path.name.endswith((".seen.json", ".last.json"))]
                 + list(self.root.glob("*.queue/*.json")))
        return sorted({record["board"] for record in map(self.read_json, paths)
                       if isinstance(record, dict) and isinstance(record.get("board"), str)
                       and record["board"] != HEAVY})

    def any_board_held(self):
        """Observe active work without changing lock records or taking a board."""
        for board in self.boards():
            lock = self.read_json(self.lock_path(board))
            if lock and lock.get("kind") != "human" and not self.reclaim_reason(lock, DEFAULT_STALE_SECONDS):
                return True
        return False

    def seen_boards(self):
        """Every board this machine has ever found on USB, kept past its own
        lock, reservation or waiter, only for hand-to-human/take-back to
        recall a board that is now idle and unplugged."""
        return sorted({record["board"] for record in map(self.read_json, self.root.glob("*.seen.json"))
                       if isinstance(record, dict) and isinstance(record.get("board"), str)
                       and record["board"] != HEAVY})

    def note_seen(self, board):
        """Records a board found on USB, once; nothing reads this again
        until seen_boards() needs it, so a repeat sighting is a no-op."""
        path = self.seen_path(board)
        if not self.read_json(path):
            write_json(path, {"board": board})

    @contextlib.contextmanager
    def guard(self, board):
        path = self.guard_path(board)
        path.parent.mkdir(parents=True, exist_ok=True)
        # On Windows a guard another process is still deleting, or merely
        # stat()ing, refuses create and unlink with PermissionError: busy, not
        # an error. WSL on a Windows drive reports that same delete-pending
        # guard as FileNotFoundError, even with O_CREAT.
        while True:
            try:
                descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.close(descriptor)
                break
            except (FileExistsError, PermissionError, FileNotFoundError):
                path.parent.mkdir(parents=True, exist_ok=True)
                try:
                    age = self.resource_now(board) - path.stat().st_mtime
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

    def note_last_holder(self, board, lock, how):
        """Keeps who held the board last, for the message a command prints when
        the port stays busy after it won the lock. Under guard()."""
        write_json(self.last_path(board), {
            "board": board, "owner": lock.get("owner", "unknown"),
            "purpose": lock.get("purpose", "unknown"), "pid": lock.get("pid"),
            "host": lock.get("host"), "token": lock.get("token"),
            "acquired_at": lock.get("acquired_at"), "ended_at": self.resource_now(board),
            "how": how, "protocol": LOCK_PROTOCOL, "autana_version": __version__})

    def check_order(self, board):
        if board == HEAVY:
            return
        if (any(held["pid"] == os.getpid() for held in list(HELD_HEAVY.values()))
                or os.environ.get(autana_config.HEAVY_TOKEN_ENV)):
            raise RuntimeError("holding heavy: cannot acquire a board lock (lock order is board -> heavy)")

    def enqueue(self, board, owner, kind, pid=None, purpose=None):
        self.check_order(board)
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
            now = self.resource_now(board)
            write_json(directory / (ticket + ".json"), {
                "board": board,
                "created_at": now,
                "touched_at": now,
                "host": host_identity(),
                "owner": owner,
                "pid": os.getpid() if pid is None else pid,
                "kind": kind,
                "purpose": purpose or purpose_of(kind),
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
        now = self.resource_now(board)
        for ticket in self.tickets(board):
            dead = (ticket.get("host", host_identity()) == host_identity()
                    and ticket["pid"] != os.getpid() and not self.is_alive(ticket["pid"]))
            stale = (now - ticket.get("touched_at", ticket.get("created_at", now))
                     > DEFAULT_STALE_SECONDS)
            if dead or stale:
                try:
                    (self.queue_dir(board) / (ticket["ticket"] + ".json")).unlink()
                except FileNotFoundError:
                    pass

    def is_stale(self, lock, stale_seconds):
        board = lock.get("board")
        now = self.resource_now(board)
        return now - lock["heartbeat_at"] > stale_seconds

    def reclaim_reason(self, lock, stale_seconds):
        """Why `lock` may be taken from its holder, or "" while it holds."""
        if lock.get("kind") in ("hold", "human"):
            expires = lock.get("expires_at")
            if expires is not None:
                board = lock.get("board")
                now = self.resource_now(board)
                return "hold lapsed" if now >= expires else ""
            if not lock.get("until_released"):
                return ""
            if lock.get("host") != host_identity():
                return ""
            return "" if self.owner_alive(lock) else "dead process"
        same_host = lock.get("host") == host_identity()
        if same_host and not self.is_alive(lock["pid"]):
            return "dead process"
        if self.is_stale(lock, stale_seconds):
            return "heartbeat expiry"
        return ""

    def owner_alive(self, record):
        if not self.is_alive(record["pid"]):
            return False
        started = process_start(record["pid"])
        acquired = record.get("acquired_at")
        return not (started is not None and isinstance(acquired, (int, float))
                    and started > acquired + PID_REUSE_SLACK_SECONDS)

    def claim(self, board, ticket, stale_seconds=DEFAULT_STALE_SECONDS, estimate=None):
        held, evicted, reason = self._claim(board, ticket, stale_seconds, estimate)
        if held and held["kind"] != "hand":
            note = (f"reclaimed from {evicted.get('owner', 'unknown')} ({reason})"
                    if evicted else "")
            device_hook.emit("acquired", board, held["owner"], held["purpose"], note=note)
        return held

    def _claim(self, board, ticket, stale_seconds, estimate):
        with self.guard(board):
            path = self.queue_dir(board) / (ticket + ".json")
            record = self.read_json(path)
            if record:
                record["touched_at"] = self.resource_now(board)
                write_json(path, record)
            self.prune_crashed_waiters(board)
            pending = self.tickets(board)
            if not pending or pending[0]["ticket"] != ticket:
                return None, None, ""
            current = self.read_json(self.lock_path(board))
            reclaimed = ""
            evicted = None
            reason = ""
            if current:
                # Judged from PROTOCOL_CORE_FIELDS alone (pid, host,
                # heartbeat_at), so this never depends on a protocol match: a
                # live holder, of any protocol, simply keeps the board until
                # it is dead or stale: never refused, never a reason to stop.
                reason = self.reclaim_reason(current, stale_seconds)
                if not reason:
                    return None, None, ""
                evicted = current
                self.note_last_holder(board, current, "reclaimed")
                reclaimed = "reclaimed lock from {owner} for {label} ({reason})".format(
                    owner=current.get("owner", "unknown"), label=current.get("purpose", "unknown"),
                    reason=reason)
                self.lock_path(board).unlink(missing_ok=True)
            now = self.resource_now(board)
            held = {
                "acquired_at": now,
                "board": board,
                "heartbeat_at": now,
                "host": host_identity(),
                "owner": pending[0]["owner"],
                "pid": pending[0]["pid"],
                "kind": pending[0]["kind"],
                "purpose": pending[0]["purpose"],
                "token": uuid.uuid4().hex,
                "protocol": LOCK_PROTOCOL,
                "autana_version": __version__,
            }
            if estimate is not None:
                held["estimated_end"] = now + estimate
            write_json(self.lock_path(board), held)
            held = Held(held, reclaimed)
            if board == HEAVY and held["pid"] == os.getpid():
                if evicted:
                    HELD_HEAVY.pop(evicted.get("token"), None)
                HELD_HEAVY[held["token"]] = held
            (self.queue_dir(board) / (ticket + ".json")).unlink(missing_ok=True)
            return held, evicted, reason

    def acquire(self, board, owner, kind, wait=0, stale_seconds=DEFAULT_STALE_SECONDS,
                on_wait=None, purpose=None, estimate=None):
        ticket = self.enqueue(board, owner, kind, purpose=purpose)
        deadline = self.resource_now(board) + wait
        label = purpose or purpose_of(kind)
        waiting = False
        with terminate_unwinds():
            try:
                while True:
                    held = self.claim(board, ticket, stale_seconds, estimate)
                    if held:
                        return held
                    if not self.read_json(self.queue_dir(board) / (ticket + ".json")):
                        if waiting:
                            device_hook.emit("gave-up", board, owner, label)
                        return None
                    # Even a first claim that exhausted the wait reports the queue.
                    if wait > 0:
                        if not waiting:
                            device_hook.emit("waiting", board, owner, label)
                            waiting = True
                        if on_wait:
                            on_wait(ticket)
                    if self.resource_now(board) >= deadline:
                        if waiting:
                            device_hook.emit("gave-up", board, owner, label)
                        return None
                    time.sleep(min(0.1, max(0, deadline - self.resource_now(board))))
            finally:
                self.cancel(board, ticket)

    def cancel(self, board, ticket):
        with self.guard(board):
            (self.queue_dir(board) / (ticket + ".json")).unlink(missing_ok=True)

    def heartbeat(self, board, token):
        """Refuses a lock the next claim() may reclaim: a holder that stalled
        past the stale window has to find out it lost the board, not renew it.
        `token` is never a foreign lock's; a mismatch (missing or not ours)
        just means this is not our lock to touch, protocol notwithstanding."""
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock.get("token") != token or self.reclaim_reason(lock, DEFAULT_STALE_SECONDS):
                return False
            lock["heartbeat_at"] = self.resource_now(board)
            write_json(self.lock_path(board), lock)
            return True

    def live_lock(self, board, token, stale_seconds=DEFAULT_STALE_SECONDS):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            return (lock if lock and lock.get("token") == token and
                    not self.reclaim_reason(lock, stale_seconds) else None)

    def board_of_token(self, token):
        for board in self.boards():
            if self.live_lock(board, token):
                return board
        return None

    def check_token(self, board, token, stale_seconds=DEFAULT_STALE_SECONDS):
        return self.live_lock(board, token, stale_seconds) is not None

    def release(self, board, token):
        lock = self._release(board, token)
        if lock:
            device_hook.emit("released", board, lock["owner"], lock.get("purpose", "unknown"))
        return bool(lock)

    def _release(self, board, token):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if board == HEAVY:
                HELD_HEAVY.pop(token, None)
            if not lock or lock.get("token") != token:
                return None
            self.lock_path(board).unlink(missing_ok=True)
            self.note_last_holder(board, lock, "released")
            return lock

    def hold(self, board, token, note, seconds=None, pid=None, human=False, estimate=None):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock.get("token") != token or self.reclaim_reason(lock, DEFAULT_STALE_SECONDS):
                return False
            now = self.resource_now(board)
            lock.update(heartbeat_at=now,
                        pid=0 if seconds is not None else (pid if pid is not None else os.getpid()))
            lock.pop("until_released", None)
            lock.pop("expires_at", None)
            lock.update(kind="human" if human else "hold", purpose=note)
            if estimate is not None:
                lock["estimated_end"] = now + estimate
            if seconds is not None:
                lock["expires_at"] = now + seconds
            else:
                lock["until_released"] = True
                if not human:
                    lock["owner"] = bound_owner(lock["owner"], lock["pid"])
            write_json(self.lock_path(board), lock)
            return True

    def reserve_human(self, board, owner, note, wait=0, on_wait=None):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if lock and lock.get("kind") == "human" and not self.reclaim_reason(lock, DEFAULT_STALE_SECONDS):
                lock.update(owner=owner, purpose=note, heartbeat_at=self.resource_now(board),
                            expires_at=self.resource_now(board) + HUMAN_RESERVATION_SECONDS)
                write_json(self.lock_path(board), lock)
                return lock["token"], True
        held = self.acquire(board, owner, "hand", wait=wait, on_wait=on_wait, purpose=note)
        if not held:
            return None, False
        if not self.hold(board, held["token"], note, seconds=HUMAN_RESERVATION_SECONDS, human=True):
            return None, False
        device_hook.emit("human-reserved", board, owner, note=note)
        return held["token"], False

    def end_human(self, board):
        with self.guard(board):
            lock = self.read_json(self.lock_path(board))
            if not lock or lock.get("kind") != "human":
                return
            self.lock_path(board).unlink(missing_ok=True)
            self.note_last_holder(board, lock, "released")
        device_hook.emit("human-cleared", board, lock["owner"], note=lock.get("purpose", ""))

    def status(self, board, stale_seconds=DEFAULT_STALE_SECONDS):
        """Live holders and queued work; reclaimable records count as free."""
        with self.guard(board):
            self.prune_crashed_waiters(board)
            lock = self.read_json(self.lock_path(board))
            reason = self.reclaim_reason(lock, stale_seconds) if lock else ""
            human = (dict(lock, id=lock["token"], note=lock.get("purpose"), since_at=lock.get("acquired_at"))
                     if lock and lock.get("kind") == "human" else None)
            return {
                "human": human if not reason else None,
                "expired_human": human if reason else None,
                "lock": lock if not reason and not human else None,
                "reclaimable": dict(lock, reason=reason) if reason and not human else None,
                "queue": self.tickets(board),
            }


# A pid whose process began this long after the lock was taken is not the
# holder: the holder is enqueued before it acquires.
PID_REUSE_SLACK_SECONDS = 2
DURATIONS_FILE = "durations.jsonl"
# An estimate is the median of a command kind's last ESTIMATE_RECENT_RUNS
# successful runs, and unknown before ESTIMATE_MINIMUM_RUNS of them. A holder
# past its estimate counts as free now; a reservation or an unknown duration
# ahead of a waiter makes the waiter's estimate unknown.
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


def record_duration(kind, seconds, error=None, root=None, *, board):
    """A failed run is recorded with its error and never shapes an estimate."""
    path = Path(root or default_root()) / DURATIONS_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps({"command": kind, "duration_seconds": seconds,
                                 "error": error, "board": normalise_board(board)}) + "\n")
    rows = duration_rows(path.parent)
    if len(rows) > DURATIONS_TRIM_LINES:
        trim_durations(path, rows)


def trim_durations(path, rows):
    """Keeps each kind and board's last ESTIMATE_RECENT_RUNS successful rows, then the
    newest DURATIONS_TRIM_LINES of those, however many kinds there are. A row
    another process appends during the rewrite can be lost; a median over
    that many runs does not notice."""
    by_kind = {}
    for index, row in enumerate(rows):
        if successful_duration(row) is not None:
            by_kind.setdefault((row["command"], row.get("board")), []).append(index)
    recent = sorted(index for runs in by_kind.values() for index in runs[-ESTIMATE_RECENT_RUNS:])
    kept = recent[-DURATIONS_TRIM_LINES:]
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text("".join(json.dumps(rows[index]) + "\n" for index in kept),
                         encoding="utf-8")
    os.replace(temporary, path)


def duration_history(root=None, minimum=ESTIMATE_MINIMUM_RUNS, board=None):
    """Prefer a board's own successful runs once its kind has enough history."""
    history, own = {}, {}
    board = normalise_board(board) if board else None
    for row in duration_rows(root):
        duration = successful_duration(row)
        if duration is not None:
            kind = row["command"]
            history.setdefault(kind, []).append(duration)
            if board and row.get("board") == board:
                own.setdefault(kind, []).append(duration)
    selected = {kind: own[kind] if len(own.get(kind, [])) >= minimum else values
                for kind, values in history.items()}
    return {kind: statistics.median(values[-ESTIMATE_RECENT_RUNS:])
            for kind, values in selected.items() if len(values) >= minimum}


def local_time(timestamp):
    return datetime.datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M:%S")


def format_estimate(timestamp):
    return local_time(timestamp) if timestamp is not None else "unknown (no duration history)"


def holder_end(lock, durations):
    """Project completion from an explicit estimate or the holder's duration history."""
    end = lock.get("estimated_end")
    if end is not None:
        return end
    duration = durations.get(lock.get("kind")) if lock.get("kind") != "hold" else None
    acquired = lock.get("acquired_at")
    if duration is not None and acquired is not None:
        return acquired + duration
    return None


def queue_estimates(status, now, durations):
    """Each waiter's estimated start, by ticket: the holder's own kind sets
    when the board frees, and each waiter's kind how long it keeps it.
    The None key estimates the start of a new request behind the queue."""
    lock = status.get("lock")
    if status.get("human"):
        start = None
    elif lock:
        end = holder_end(lock, durations)
        start = max(now, end) if end is not None else None
    else:
        start = now
    estimates = {}
    for ticket in status["queue"]:
        estimates[ticket["ticket"]] = start
        duration = durations.get(ticket.get("kind"))
        start = start + duration if start is not None and duration is not None else None
    estimates[None] = start
    return estimates


RESERVATION_EXPIRED = "reservation expired"


def status_entry(store, board, port=None, now=None, durations=None):
    """One board's state as `status --json` prints it. Times are epoch
    seconds; an estimate with too little history is None."""
    now = store.resource_now(board) if now is None else now
    durations = duration_history(store.key_root(board), board=board) if durations is None else durations
    status = store.status(board)
    entry = {"board": board, "port": port, "state": "unlocked", "holder": None,
             "since": None, "estimated_free": None,
             "lapsed": None, "expires_at": None, "waiting": []}
    if status.get("expired_human"):
        lapsed = status["expired_human"]
        entry["lapsed"] = {"owner": lapsed["owner"], "purpose": lapsed.get("note"),
                           "reason": RESERVATION_EXPIRED,
                           "at": human_expires_at(lapsed)}
    if status["human"]:
        human = status["human"]
        entry.update(state="human", holder={"owner": human["owner"], "purpose": human.get("note")},
                     since=human.get("since_at"),
                     expires_at=human_expires_at(human))
    elif status["lock"]:
        lock = status["lock"]
        end = holder_end(lock, durations)
        entry.update(state="held",
                     holder={"owner": lock["owner"], "purpose": lock.get("purpose", "unknown"),
                             "protocol": lock.get("protocol", 0),
                             "autana_version": lock.get("autana_version"),
                             **({"kind": "hold"} if lock.get("kind") == "hold" else {})},
                     since=lock.get("acquired_at"), expires_at=lock.get("expires_at"),
                     estimated_free=max(now, end) if end is not None else None)
    elif status["reclaimable"]:
        stale = status["reclaimable"]
        entry["lapsed"] = {"owner": stale["owner"], "purpose": stale.get("purpose", "unknown"),
                           "reason": stale["reason"], "at": None}
    estimates = queue_estimates(status, now, durations)
    entry["estimated_start"] = estimates[None]
    entry["waiting"] = [{"owner": ticket["owner"], "purpose": ticket.get("purpose", "unknown"),
                         "estimated_start": estimates[ticket["ticket"]]}
                        for ticket in status["queue"]]
    return entry


def previous_holder_alive(store, last, scope):
    """[(pid, name)] of the previous holder's own process, if it still runs,
    and of what `scope` finds besides. The pid is judged here for every OS; a
    pid that started after the lock was taken is another program's, reused."""
    if last.get("host") != host_identity():
        return []
    alive = []
    pid = last.get("pid")
    if isinstance(pid, int) and pid != os.getpid() and store.owner_alive(last):
        alive.append((pid, scope.process_name(pid)))
    for extra in scope.survivors_extra(last):
        if extra != pid and extra != os.getpid():
            alive.append((extra, scope.process_name(extra)))
    return alive


def previous_holder_text(store, board, scope, now=None):
    """What a command that won the lock prints when the port still will not
    open: who held the board before it and which of that holder's processes are
    still running, so a person can decide. Nothing here stops anything: a new
    holder never kills another's processes. `scope` is lock_scope: the
    per-OS process_start, process_name and survivors_extra."""
    last = store.read_json(store.last_path(board))
    if not isinstance(last, dict):
        return ("The board's previous holder is not recorded; a program outside autana "
                "may have the port open.")
    now = store.resource_now(board) if now is None else now
    ended = last.get("ended_at")
    when = f" {duration_text(now - ended)} ago" if isinstance(ended, (int, float)) else ""
    how = "was reclaimed" if last.get("how") == "reclaimed" else "ended"
    text = (f"The board's previous holder was {last.get('owner', 'unknown')} "
            f"({last.get("purpose", "unknown")}); its lock {how}{when}.")
    alive = previous_holder_alive(store, last, scope)
    if alive:
        names = ", ".join(f"{pid} ({name})" if name else str(pid) for pid, name in alive)
        return (text + f" Still running from it: {names}. autana never stops another "
                "holder's processes; end them yourself if you are sure they are not "
                "needed, or wait.")
    return (text + " None of its processes is still running, so a program outside autana "
            "(a serial terminal or monitor) probably has the port open.")


def holder_version_details(holder):
    version = holder.get("autana_version") or "unknown"
    return f"autana {version}, lock protocol {holder.get('protocol', 0)}"


def holder_version_text(holder):
    """'(autana <version>, lock protocol <n>)', the information a differently
    versioned holder's record carries, shown while waiting and in `status`,
    never a reason to refuse it."""
    return f"({holder_version_details(holder)})"


def busy_text(status, now):
    """Why a command that would not wait was not given the lock."""
    if status.get("human"):
        human = status["human"]
        left = human_left_text(human, now)
        return (f"board reserved by {human['owner']}: {human.get('note')} "
                f"({left + '; ' if left else ''}`autana lock take-back` ends it)")
    lock = status.get("lock")
    if lock:
        key = HEAVY if lock.get("board") == HEAVY else "board"
        return (f"{key} held by {lock['owner']} for {lock.get("purpose", "unknown")} since "
                f"{local_time(lock['acquired_at'])}; `autana status` shows the queue")
    return "other commands are queued ahead"


def duration_text(seconds):
    """'45s', '3m', '1h 5m': how long ago something was, or how long is left."""
    seconds = max(0, round(seconds))
    if seconds < 60:
        return f"{seconds}s"
    minutes = seconds // 60
    if minutes < 60:
        return f"{minutes}m"
    return f"{minutes // 60}h {minutes % 60}m" if minutes % 60 else f"{minutes // 60}h"


def elapsed_seconds(since, now):
    return round(max(0, now - since)) if since is not None else None


def availability(entry):
    if entry["state"] != "unlocked":
        return entry["state"]
    return "queued" if entry["waiting"] else "free"


def status_lines(entry, now):
    """The human text of one entry; durations are `now` minus its timestamps."""
    holder = entry["holder"]
    available = availability(entry)
    if available == "human":
        left = (f"; {duration_text(entry['expires_at'] - now)} left, "
                "`autana lock hand` again renews it") if entry["expires_at"] is not None else ""
        lines = [f"human reservation: {holder['owner']}: {holder['purpose']} "
                 f"(since {local_time(entry['since'])}; {elapsed_seconds(entry['since'], now)}s ago{left})"]
    elif available == "held":
        hold = (f"{duration_text(entry['expires_at'] - now)} left" if entry["expires_at"] is not None
                else "until released") if holder.get("kind") == "hold" else ""
        lines = [f"held by {holder['owner']} for {holder['purpose']} since "
                 f"{local_time(entry['since'])} (elapsed {elapsed_seconds(entry['since'], now)}s; "
                 f"estimated free {format_estimate(entry['estimated_free'])}; "
                 f"{holder_version_details(holder)}{'; ' + hold if hold else ''})"]
    elif entry["lapsed"] and entry["lapsed"]["reason"] == RESERVATION_EXPIRED:
        lapsed = entry["lapsed"]
        lines = [f"unlocked - human reservation from {lapsed['owner']}: {lapsed['purpose']} "
                 f"expired {duration_text(now - lapsed['at'])} ago and is released"]
    elif entry["lapsed"]:
        lines = ["unlocked - stale lock from {owner} for {purpose} ({reason})".format(
            **entry["lapsed"])]
    else:
        lines = ["unlocked"]
    if available == "queued":
        lines[0] += " - queued"
    if entry["waiting"]:
        lines.append("waiting:")
    for place, waiter in enumerate(entry["waiting"], 1):
        lines.append(f"  {place}. {waiter['owner']} for {waiter['purpose']}; estimated start "
                     f"{format_estimate(waiter['estimated_start'])}")
    return lines
