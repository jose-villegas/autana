"""Lock-aware commands for the single shared USB Serial/JTAG board."""

import argparse
import contextlib
import gzip
import json
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import device_lock
import device_report

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher" / "tools" / "build"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher" / "tools" / "device"))
from espressif import espressif_tools_root, idf_python  # noqa: E402  (path must be set up first)


BAUD = 115200
BUILD_ID = re.compile(rb"BUILD_ID=([^\s\r\n]+)")
SUITE_RESULT = re.compile(rb":\d+:.*:(PASS|FAIL)(?:\r?$|:)", re.MULTILINE)

# Suite/listen captures run 13-131 KB and a flash log ~270 KB; only a capture
# that lands on the default path (not an explicit --out) is ever gzipped, and
# only once it clears this, so the common case stays plain-text and greppable.
COMPRESS_ABOVE_BYTES = 200_000
SLUG_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]+")
BOOT_ERROR = re.compile(r"(?:\berror\b|\bpanic\b|\babort\b|\bassert\b|^E \(\d+\))", re.I)
MAX_PRINTED_FAILURES = 10


def python_with_pyserial():
    return idf_python()


def rerun_under_idf_python(argv):
    """This command's exit status after running it again under ESP-IDF's
    Python, or None when this interpreter can already reach the board. pyserial
    lives in that environment, not in whichever `python` a caller found first,
    so no caller has to know which one to pick."""
    try:
        import serial  # noqa: F401
        return None
    except ImportError:
        pass
    python = idf_python()
    if os.path.normcase(os.path.abspath(python)) == os.path.normcase(os.path.abspath(sys.executable)):
        return None
    return subprocess.call([python, str(Path(__file__).resolve()), *argv])


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


def find_port():
    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("pyserial is required; run this with the ESP-IDF Python") from error
    matches = [port.device for port in list_ports.comports() if port.vid == 0x303A]
    if not matches:
        raise RuntimeError("no USB Serial/JTAG board found (VID 0x303A)")
    if len(matches) > 1:
        raise RuntimeError("multiple USB Serial/JTAG boards found: " + ", ".join(matches))
    return matches[0]


def open_serial(port):
    require_port_lock(port)
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required; run this with the ESP-IDF Python") from error
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = BAUD
    connection.timeout = 0.2
    connection.dtr = False
    connection.rts = False
    connection.open()
    return connection


def now():
    return datetime.now()


def records_root():
    """Where a session's log and manifest are written.

    AUTANA_RECORDS names it. Unset, records land in the engine worktree's
    own gitignored .records/device - self-contained, and nothing a commit
    can pick up by accident. The maintainer's shell points it at the .dev
    checkout instead (scripts/add-tools-to-path.sh), which is where this
    project's device history is kept and tracked."""
    named = os.environ.get("AUTANA_RECORDS")
    if named:
        return Path(named)
    return Path(__file__).resolve().parents[2] / ".records" / "device"


def slug(text):
    """Sanitise one manifest field for use inside a filename - collapse any
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


def record_capture(path, managed, *, started_at, port, owner, purpose, command, commit,
                   suite=None, build_id=None, worktree=None, reason=None, error=None, root=None,
                   acquired_at=None):
    """The permanent trail: written for every flash/run-suite/listen call, so
    even a capture left behind in a doomed worktree still has metadata here."""
    path = Path(path)
    if managed:
        path = compress_if_large(path)
    try:
        capture_bytes = path.stat().st_size
    except OSError:
        capture_bytes = None
    append_manifest({
        "started_at": started_at.isoformat(),
        "acquired_at": datetime.fromtimestamp(acquired_at).isoformat() if acquired_at else None,
        "duration_seconds": max(0, time.time() - acquired_at) if acquired_at else None,
        "port": port,
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
    }, root)
    return path


# As long as the lock's own stale window: a legitimate perf capture runs for
# minutes with the port open, so a wait shorter than that makes a task that
# queued correctly give up on a holder that is still working.
PORT_WAIT_SECONDS = 600


def open_when_free(port, seconds=PORT_WAIT_SECONDS, opener=None, sleep=time.sleep,
                   now=time.monotonic, reason="held by another process"):
    """Opens the port once the OS lets go of it and returns the open connection;
    the caller closes it.

    The lock arbitrates intent; the OS owns the port, and the two disagree
    whenever a previous holder's reader outlives its lock - a caller that
    queued fairly then fails on a port it was promised, which reads as a flaky
    board. Waiting is the right answer: this caller already won its turn, a
    straggler drains in seconds, and a port nobody ever frees still reports
    itself at the deadline."""
    open_port = opener or open_serial  # resolved per call, so a patched opener is honoured
    deadline = now() + seconds
    waited = False
    while True:
        try:
            connection = open_port(port)
            if waited:
                print("port " + port + " came free", file=sys.stderr)
            return connection
        except OSError as error:
            if now() >= deadline:
                raise RuntimeError(port + " still " + reason + " when the "
                                   + str(int(seconds)) + "s wait ran out: "
                                   + str(error)) from error
            if not waited:
                print("waiting for " + port + ", " + reason, file=sys.stderr)
                waited = True
            sleep(1.0)


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


def print_suite_output(data, record_path, command, reason, verbose):
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
    print(f"{command} capture: {record_path}")
    print(f"{command} capture ended: {reason}")
    return failed


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


def require_port_lock(port):
    active = getattr(ACTIVE_LOCK, "held", None)
    if active is None:
        raise RuntimeError("serial port access requires the device lock")
    store, locked_port, token = active
    current = store.status(locked_port)["lock"]
    if not current or current["token"] != token:
        raise RuntimeError("device lock was lost before serial port access")


class HeldLock:
    def __init__(self, store, port, owner, purpose, wait, announce_waiters=False, kind=None):
        self.store = store
        self.announce_waiters = announce_waiters
        self.port = port
        self.held = store.acquire(port, owner, purpose, wait=wait, kind=kind)
        if not self.held:
            raise RuntimeError("device lock was not acquired")
        if self.held["log"]:
            print(self.held["log"], file=sys.stderr)
        self.stop = threading.Event()
        self.notified = set()
        self.thread = threading.Thread(target=self.keep_alive, daemon=True)

    def keep_alive(self):
        # Only a holder the person can end with Ctrl+C invites them to.
        poll = 1 if self.announce_waiters else 30
        next_heartbeat = time.monotonic() + 30
        while not self.stop.wait(poll):
            for ticket in (self.store.tickets(self.port) if self.announce_waiters else ()):
                if ticket["ticket"] not in self.notified:
                    self.notified.add(ticket["ticket"])
                    print(f'{ticket["owner"]} is waiting for the board '
                          f'({ticket["purpose"]}) - Ctrl+C to hand it over', file=sys.stderr)
            if time.monotonic() >= next_heartbeat:
                if not self.store.heartbeat(self.port, self.held["token"]):
                    print("device lock was lost", file=sys.stderr)
                    return
                next_heartbeat = time.monotonic() + 30

    def __enter__(self):
        self.previous_lock = getattr(ACTIVE_LOCK, "held", None)
        ACTIVE_LOCK.held = (self.store, self.port, self.held["token"])
        self.thread.start()
        return self

    def __exit__(self, unused_type, unused_value, unused_traceback):
        self.stop.set()
        try:
            self.thread.join()
        finally:
            self.store.release(self.port, self.held["token"])
            ACTIVE_LOCK.held = self.previous_lock


def tests_done(data):
    return (b"TESTS_DONE" in data or b"SELFTEST_COMPLETE" in data or
            (b"Tests " in data and b"Failures" in data))


def build_id_heard(data):
    """Only a finished line counts: an id can arrive split across two reads."""
    return BUILD_ID.search(data[:data.rfind(b"\n") + 1]) is not None


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
            try:
                chunk = connection.read(4096)
            except OSError:
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
                        raise RuntimeError("this build has no test suites - flash --variant diag")
                    if (b"no suite named '" + suite_name.encode("ascii") + b"'") in text:
                        raise RuntimeError("no suite named " + suite_name + " on this build")
                    if b"SUITE_DONE" in text or text.startswith(suite_complete):
                        return bytes(data), "complete"
            if complete is not None and complete(data):
                return bytes(data), "complete"
    return bytes(data), "timeout"


def reset(port, after="hard_reset"):
    """hard_reset pulses RTS, and USB Serial/JTAG stays up through it, so a
    capture hears the boot from its first line. It cannot restart a chip in
    download mode; watchdog_reset can, but re-enumerates USB, losing the
    early boot lines a release image's BUILD_ID is among."""
    require_port_lock(port)
    command = [python_with_pyserial(), "-m", "esptool", "--chip", "esp32s3", "-p", port,
               "--after", after, "chip_id"]
    subprocess.run(command, check=True)


def reset_and_capture(port, output, seconds, idle_seconds, expected_build_id=None,
                      complete=tests_done):
    """A board that says nothing at all after the RTS reset is taken for a chip
    in download mode, the one case RTS cannot start, and gets the watchdog. One
    that spoke, even without a BUILD_ID, is left alone."""
    reset(port)
    data, reason = capture_after_reset(port, output, seconds, idle_seconds, expected_build_id,
                                       complete)
    if reason != "silent":
        return data, reason
    print("board silent after an RTS reset, as from download mode; "
          "restarting it through the watchdog", file=sys.stderr)
    reset(port, after="watchdog_reset")
    return capture_after_reset(port, output, seconds, idle_seconds, expected_build_id,
                               complete)


RESET_FIRST_BYTE_SECONDS = 2
RESET_REOPEN_SECONDS = 10
FLASH_PORT_WAIT_SECONDS = 12
FLASH_BOOT_SECONDS = 12


def open_usb_after_flash(seconds):
    deadline = time.monotonic() + seconds
    while True:
        try:
            port = find_port()
            return port, open_serial(port)
        except (OSError, RuntimeError):
            if time.monotonic() >= deadline:
                raise RuntimeError("USB Serial/JTAG port did not return after the flash")
            time.sleep(0.2)


def capture_after_reset(port, output, seconds, idle_seconds, expected_build_id=None,
                        complete=tests_done, follow_usb=False):
    """A watchdog reset or a power cycle re-enumerates USB Serial/JTAG and
    kills an open handle, so no capture spans one: what the board prints
    before the port reopens is lost. It re-enumerates late enough that the
    first open can get the old handle, which reads nothing rather than
    failing, so within RESET_REOPEN_SECONDS a handle silent from the start is
    reopened. A capture that heard nothing at all ends as "silent"."""
    started = time.monotonic()
    deadline = started + seconds
    data = bytearray()
    append = False
    first_reopen = True

    def ended(reason):
        return bytes(data), reason if data else "silent"

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return ended("timeout")
        try:
            if follow_usb:
                unused_port, connection = open_usb_after_flash(remaining)
            else:
                connection = open_when_free(port, remaining,
                                            reason="re-enumerating after reset")
        except RuntimeError:
            if first_reopen and not follow_usb:
                raise
            return ended("port lost")
        first_reopen = False
        with connection:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return ended("timeout")
            in_reopen_window = not data and time.monotonic() - started < RESET_REOPEN_SECONDS
            part, reason = capture(connection, output, remaining, idle_seconds,
                                   expected_build_id, append=append, complete=complete,
                                   first_byte_seconds=RESET_FIRST_BYTE_SECONDS
                                   if in_reopen_window else None)
        data.extend(part)
        append = True
        if reason not in ("port lost", "silent"):
            return ended(reason)
        if time.monotonic() >= deadline:
            return ended(reason)
        if reason == "silent":
            print("reset capture read nothing since the reset; reopening", file=sys.stderr)
        else:
            print("reset capture lost the port; reopening", file=sys.stderr)


def reset_device(args, store, port):
    """Reboot under the device lock, optionally keeping the post-reset
    console output as a capture record."""
    if not args.capture:
        with HeldLock(store, port, args.owner, args.purpose, args.wait, kind="reset"):
            with open_when_free(port):
                pass
            reset(port)
            with open_when_free(port, reason="re-enumerating after reset"):
                pass
        print("board reset; USB serial port is back")
        return 0

    started_at = now()
    output, managed = resolve_capture_path(args.out, "reset", args.owner, started_at)
    data = b""
    reason = None
    error = None
    try:
        with HeldLock(store, port, args.owner, args.purpose, args.wait, kind="reset") as held:
            with open_when_free(port):
                pass
            data, reason = reset_and_capture(port, output, args.seconds, None)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
        error = str(caught)
        raise
    finally:
        final_path = record_capture(
            output, managed, started_at=started_at, port=port, owner=args.owner,
            purpose=args.purpose, command="reset", build_id=latest_build_id_from_bytes(data),
            worktree=str(Path.cwd()), commit=git_commit(), reason=reason, error=error,
            acquired_at=held.held.get("acquired_at") if "held" in locals() else None)
    print_reset_output(data, getattr(args, "verbose", False))
    print("reset capture: " + str(final_path))
    print("reset capture ended: " + reason + "; bytes may have been lost in reset gap")
    return 0


CRASH_ADDRESS_RE = re.compile(rb"0x4[0-9a-fA-F]{7}")


def toolchain_addr2line():
    """The xtensa-esp32s3-elf-addr2line beside ESP-IDF's own toolchain,
    found under espressif_tools_root(), or None when it is not installed. Sorted
    reverse-alphabetically so the newest of several installed toolchain
    versions wins."""
    root = espressif_tools_root() / "tools" / "xtensa-esp-elf"
    for bin_dir in sorted(root.glob("*/*/bin"), reverse=True):
        for name in ("xtensa-esp32s3-elf-addr2line.exe", "xtensa-esp32s3-elf-addr2line"):
            candidate = bin_dir / name
            if candidate.is_file():
                return candidate
    return None


def decode_crash_addresses(data, elf):
    """Every address on a `Backtrace:`/`PC` line in a capture, resolved
    against `elf` to a file and line number. Returns [] when nothing looks
    like a crash, or when no addr2line is installed to ask."""
    addresses = []
    for line in data.split(b"\n"):
        if b"Backtrace" in line or b"PC      :" in line or b"PC :" in line:
            addresses += CRASH_ADDRESS_RE.findall(line)
    if not addresses:
        return []
    addr2line = toolchain_addr2line()
    if addr2line is None:
        return []
    unique = list(dict.fromkeys(address.decode("ascii") for address in addresses))
    result = subprocess.run([str(addr2line), "-pfiaC", "-e", str(elf), *unique],
                            capture_output=True, text=True)
    return [line for line in result.stdout.splitlines() if line.strip()]


def find_elf_for_build_id(worktree, build_id):
    """The launcher.elf whose own build_id.txt matches `build_id`, searched
    across every launcher/build*/ directory - the build actually on the
    board, not merely the newest one on disk (a `--dev` build built after
    the board was last flashed `--diag`, say, would otherwise decode
    against the wrong symbols). None when `build_id` is empty or nothing
    under this worktree matches."""
    if not build_id:
        return None
    for build_dir in sorted(Path(worktree).glob("launcher/build*")):
        id_file = build_dir / "build_id.txt"
        try:
            seen = id_file.read_text(encoding="ascii").strip()
        except OSError:
            continue
        if seen != build_id:
            continue
        elf = build_dir / "launcher.elf"
        if elf.is_file():
            return elf
    return None


def reset_and_read_build_id(port, seconds=12, expected_build_id=None):
    """Read the boot after flashing, then use the watchdog if it was missed."""
    data, reason = capture_after_reset(port, os.devnull, seconds, None, expected_build_id,
                                       complete=build_id_heard, follow_usb=True)
    actual = latest_build_id_from_bytes(data)
    if actual:
        return actual, reason
    port, connection = open_usb_after_flash(FLASH_PORT_WAIT_SECONDS)
    connection.close()
    reset(port, after="watchdog_reset")
    data, reason = capture_after_reset(port, os.devnull, seconds, None, expected_build_id,
                                       complete=build_id_heard, follow_usb=True)
    actual = latest_build_id_from_bytes(data)
    if actual:
        return actual, reason
    unused_port, connection = open_usb_after_flash(FLASH_PORT_WAIT_SECONDS)
    with connection:
        connection.write(b"BUILDID\n")
        connection.flush()
        deadline = time.monotonic() + 3
        pending = b""
        while time.monotonic() < deadline:
            chunk = connection.read(4096)
            if not chunk:
                continue
            pending += chunk
            lines = pending.split(b"\n")
            pending = lines.pop()
            for line in lines:
                actual = build_id_from_bytes(line)
                if actual:
                    return actual, "console"
        return None, reason


def holding(store, port, args, held_lock, kind):
    """The lock a command runs under: a fresh one, or `held_lock` when a
    batch already holds the board for the whole sequence."""
    if held_lock is not None:
        return contextlib.nullcontext(held_lock)
    return HeldLock(store, port, args.owner, args.purpose, args.wait, kind=kind)


def verify_flashed_image(port, build_dir, output=None):
    require_port_lock(port)
    manifest = build_dir / "flasher_args.json"
    if not manifest.is_file():
        raise RuntimeError("flash arguments missing: " + str(manifest))
    flash_files = json.loads(manifest.read_text(encoding="utf-8"))["flash_files"]
    images = [(offset, build_dir / name) for offset, name in flash_files.items()
              if Path(name).name == "launcher.bin"]
    if len(images) != 1 or not images[0][1].is_file():
        raise RuntimeError("app image missing from flash arguments: " + str(manifest))
    offset, image = images[0]
    command = [python_with_pyserial(), "-m", "esptool", "--chip", "esp32s3", "-p", port,
               "--after", "hard_reset", "verify_flash", offset, str(image)]
    subprocess.run(command, check=True, stdout=output, stderr=subprocess.STDOUT if output else None)
    return image


def flash(args, store, port, held_lock=None, extra_flags=()):
    with holding(store, port, args, held_lock, "flash") as held:
        with open_when_free(port):
            pass
        worktree = Path(args.worktree).resolve()
        script = worktree / "launcher" / "tools" / "build" / "build_flash.sh"
        if not script.is_file():
            raise RuntimeError("build tool not found: " + str(script))
        started_at = now()
        log, managed = resolve_capture_path(args.out, "flash-" + args.variant, args.owner,
                                            started_at)
        flag = {"dev": "--dev", "diag": "--diag", "release": ""}[args.variant]
        command = [git_bash(), str(script)] + ([flag] if flag else []) + list(extra_flags) + [port]
        print("flash log: " + str(log))
        environment = os.environ.copy()
        environment.setdefault("MSYSTEM", "MINGW64")
        # Proof to build_flash.sh that this flash is under the device lock -
        # it refuses to flash without it. Not a secret: it only has to
        # differ from "unset", so a build_flash.sh run directly cannot
        # forge one by guessing.
        environment["AUTANA_DEVICE_LOCK_TOKEN"] = held.held["token"]
        build_id = None
        expected = None
        error = None
        try:
            with open(log, "wb") as stream:
                subprocess.run(command, cwd=worktree, stdin=subprocess.DEVNULL,
                               stdout=stream, stderr=subprocess.STDOUT, check=True,
                               env=environment)
            expected = latest_build_id_from_bytes(Path(log).read_bytes())
            build_dir = worktree / "launcher" / ("build" if args.variant == "release"
                                                 else "build." + args.variant)
            id_file = build_dir / "build_id.txt"
            if not expected or not id_file.is_file() or id_file.read_text(encoding="ascii").strip() != expected:
                raise RuntimeError("build id missing or inconsistent in flash output")
            store.set_expected_build_id(port, held.held["token"], expected)
            with open(log, "ab") as stream:
                image = verify_flashed_image(port, build_dir, stream)
            build_id = expected
            print("verified BUILD_ID=" + expected + " (flash image " + str(image) + ")")
        except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
            error = str(caught)
            raise
        finally:
            record_capture(log, managed, started_at=started_at, port=port, owner=args.owner,
                           purpose=args.purpose, command="flash", build_id=build_id,
                           worktree=str(worktree), commit=git_commit(worktree), error=error,
                           acquired_at=held.held.get("acquired_at"))
        return build_id or expected


def run_suite(args, store, port, held_lock=None, worktree=None, commit=None):
    """`worktree`/`commit` name the checkout the flashed build came from. A
    standalone `run-suite` has no `--worktree` of its own, so it keeps
    recording the ambient cwd; a `batch` call passes its own `--worktree`
    and that worktree's HEAD, since that is what was actually flashed."""
    started_at = now()
    output, managed = resolve_capture_path(args.out, "runsuite-" + args.suite, args.owner,
                                           started_at)
    data = b""
    reason = None
    error = None
    try:
        with holding(store, port, args, held_lock, "run-suite") as held:
            with open_when_free(port) as connection:
                connection.write(("\nRUNSUITE " + args.suite + "\n").encode("ascii"))
                connection.flush()
                data, reason = capture(connection, output, args.max_seconds, args.idle_seconds,
                                       args.expect_build_id, args.suite)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
        error = str(caught)
        raise
    finally:
        final_path = record_capture(
            output, managed, started_at=started_at, port=port, owner=args.owner,
            purpose=args.purpose, command="run-suite", suite=args.suite,
            build_id=latest_build_id_from_bytes(data) or args.expect_build_id,
            worktree=worktree if worktree is not None else str(Path.cwd()),
            commit=commit if commit is not None else git_commit(), reason=reason, error=error,
            acquired_at=held.held.get("acquired_at") if "held" in locals() else None)
        try:
            report_path = device_report.write_report_for_capture(
                final_path, records_root() / "index.jsonl")
            print("report: " + str(report_path))
        except Exception as report_error:  # a report is a convenience, never fails the capture
            print("report generation failed (capture is unaffected): " + str(report_error),
                  file=sys.stderr)
    failed = print_suite_output(data, final_path, "suite", reason, getattr(args, "verbose", False))
    return 1 if failed else 0


def selftest(args, store, port):
    """Build+flash the diagnostics+autorun image under one held lock, then
    reset and capture the boot-time run of every registered suite until
    SELFTEST_COMPLETE (or a timeout) - the way to run every suite this
    worktree registers on the device, and device_report.sh's own
    build+capture step for a report with no single named suite
    (report_test_results.sh and the frame-budget reports). A report scoped
    to one suite (report_boot_anim_perf.sh) uses `batch` instead, the same
    build-then-capture-under-one-lock shape with a suite name to send."""
    worktree = str(Path(args.worktree).resolve())
    started_at = now()
    extra_flags = ["--autorun"]
    if args.perf_scope:
        extra_flags.append("--perf-scope")
    with HeldLock(store, port, args.owner, args.purpose, args.wait, kind="selftest") as held:
        flash_args = argparse.Namespace(owner=args.owner, purpose=args.purpose + " (flash)",
                                        wait=args.wait, worktree=args.worktree,
                                        variant="diag", out=None)
        build_id = flash(flash_args, store, port, held_lock=held, extra_flags=extra_flags)
        commit = git_commit(worktree)

        data = b""
        reason = None
        error = None
        output, managed = resolve_capture_path(args.out, "selftest", args.owner, started_at)
        try:
            with open_when_free(port):
                pass
            data, reason = reset_and_capture(port, output, args.max_seconds, args.idle_seconds,
                                             build_id)
        except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
            error = str(caught)
            raise
        finally:
            final_path = record_capture(
                output, managed, started_at=started_at, port=port, owner=args.owner,
                purpose=args.purpose, command="selftest",
                build_id=latest_build_id_from_bytes(data) or build_id,
                worktree=worktree, commit=commit, reason=reason, error=error,
                acquired_at=held.held.get("acquired_at"))
            try:
                report_path = device_report.write_report_for_capture(
                    final_path, records_root() / "index.jsonl")
                print("report: " + str(report_path))
            except Exception as report_error:  # a report is a convenience, never fails the capture
                print("report generation failed (capture is unaffected): " + str(report_error),
                      file=sys.stderr)
        failed = print_suite_output(data, final_path, "selftest", reason, getattr(args, "verbose", False))
        return 1 if failed else 0


class ErrorLineSink:
    def __init__(self, output):
        self.output = output
        self.pending = b""

    def write(self, chunk):
        self.pending += chunk
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace")
            if BOOT_ERROR.search(text):
                self.output.write(text + "\n")
                self.output.flush()

    def flush(self):
        self.output.flush()

    def finish(self):
        text = self.pending.decode("utf-8", errors="replace")
        if BOOT_ERROR.search(text):
            self.output.write(text + "\n")
        self.output.flush()


def listen(args, store, port):
    started_at = now()
    output, managed = resolve_capture_path(args.out, "listen", args.owner, started_at)
    data = b""
    reason = None
    error = None
    echo = getattr(args, "echo", False)
    sink = sys.stdout.buffer if echo else ErrorLineSink(sys.stdout)
    try:
        with HeldLock(store, port, args.owner, args.purpose, args.wait,
                      announce_waiters=True, kind="listen") as held:
            with open_when_free(port) as connection:
                data, reason = capture(connection, output, args.seconds, None,
                                       echo=sink, complete=None)
    except KeyboardInterrupt:
        reason = "stopped"
        data = Path(output).read_bytes() if Path(output).exists() else b""
    except (OSError, RuntimeError, subprocess.CalledProcessError) as caught:
        error = str(caught)
        raise
    finally:
        final_path = record_capture(output, managed, started_at=started_at, port=port,
                                    owner=args.owner, purpose=args.purpose, command="listen",
                                    build_id=latest_build_id_from_bytes(data), worktree=str(Path.cwd()),
                                    commit=git_commit(), reason=reason, error=error,
                                    acquired_at=held.held.get("acquired_at") if "held" in locals() else None)
    if not echo:
        sink.finish()
    elif data and not data.endswith(b"\n"):
        print()
    print("listen capture: " + str(final_path))
    print("listen capture ended: " + reason)
    elf = Path(args.elf) if args.elf else find_elf_for_build_id(
        Path.cwd(), latest_build_id_from_bytes(data))
    if elf:
        decoded = decode_crash_addresses(data, elf)
        if decoded:
            print("\ncrash addresses decoded against " + str(elf) + ":")
            for line in decoded:
                print("  " + line)


def replies_to(data, reply, until):
    """The device's replies found in console bytes, and whether the last of
    them has arrived.

    A reply is everything from `reply` to the end of its line: the console
    also carries the firmware's own log lines, and a reply can come out
    behind a log prefix. `until` are the prefixes that end an answer -
    autana console forwards a line under the app's own prefix in capitals,
    the same convention every built-in verb's own reply already follows.
    """
    found = []
    for raw in data.split(b"\n"):
        at = raw.find(reply.encode("ascii"))
        if at < 0:
            continue
        text = raw[at:].decode("ascii", errors="replace").strip()
        found.append(text)
        if text.startswith(tuple(until)):
            return found, True
    return found, False


def send(args, store, port):
    """Write one console line and print what the device answers.

    Not a capture: nothing is written under records/ and index.jsonl gets no
    line. A tuning session is dozens of these, and none of them is evidence.

    `args.optional` is for a verb that only answers when something is wrong
    (TOUCH, IMU): a timeout with nothing seen is success, not "no reply" -
    silence is that verb's normal happy path, so whatever partial match was
    found (possibly nothing) is printed and this returns 0 rather than
    raising. autana console forwards a line the same optional way, with
    `args.reply` set to the line's own first word in capitals - an app's
    own command always replies under its own prefix, so this still
    completes as soon as `<PREFIX>_END`/`<PREFIX>_ERR` arrives rather than
    waiting out the window.
    """
    data = bytearray()
    found = []
    with HeldLock(store, port, args.owner, args.purpose, args.wait, kind="send"):
        with open_when_free(port) as connection:
            connection.reset_input_buffer()
            connection.write(("\n" + args.line + "\n").encode("ascii"))
            connection.flush()
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                data.extend(connection.read(4096))
                found, complete = replies_to(bytes(data), args.reply, args.until)
                if complete:
                    print("\n".join(found))
                    return 1 if found[-1].startswith(args.reply + "_ERR") else 0
                if ("ignoring line: '" + args.line).encode("ascii") in data:
                    raise RuntimeError("this build does not answer '" + args.line.split(" ")[0] +
                                       "' - it needs a development build that has it")
    if args.optional:
        print("\n".join(found))
        return 0
    raise RuntimeError("no reply to '" + args.line + "' within " + str(args.seconds) +
                       " s - is a development build running?")


def screenshot(args, store, port):
    """SCREENSHOT, decoded by read_screenshot()/write_capture() in
    launcher/tools/device/screenshot.py - the one decoder autana's own
    `screenshot` shares. Under the device lock, so it queues behind
    whatever else already holds the board rather than fighting it for the
    port.

    Not a capture: nothing is written under records/, the same reasoning
    send()'s own docstring gives - a screenshot is a look at the screen, not
    evidence of a run.
    """
    import screenshot as screenshot_tool

    out = args.out or str(Path.cwd() / ("screenshot_" + now().strftime("%Y%m%d_%H%M%S") + ".png"))

    def report(message):
        print(message, file=sys.stderr, flush=True)

    with HeldLock(store, port, args.owner, args.purpose, args.wait, kind="screenshot"):
        with open_when_free(port) as connection:
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
        image_turn_quarter = 3
        description = "to match the board"
    png = screenshot_tool.turn_png(png, image_turn_quarter)
    png_path, state_path = screenshot_tool.write_capture(out, png, state_json, image_turn_quarter)
    degrees = image_turn_quarter * 90
    direction = "clockwise" if image_turn_quarter == 1 else "counter-clockwise"
    if image_turn_quarter == 0:
        print(f"wrote {png_path} (framebuffer bytes; turned 0 degrees)")
    else:
        print(f"wrote {png_path} (turned {min(degrees, 360 - degrees)} degrees {direction} {description})")
    if state_path:
        print(f"wrote {state_path}")
    else:
        print("no SCREENSHOT_STATE line arrived - device state was not captured", file=sys.stderr)
    return 0


def batch(args, store, port):
    """Flash once and capture every suite `runs` times under ONE lock, then
    write one summary across all runs. Holding the board for the whole
    sequence is the point: nobody else can flash between two captures
    of this image. A
    capture that errors is recorded and the batch continues; only a failed
    build or flash stops it."""
    extra_flags = ["--perf-scope"] if args.perf_scope else []
    if args.out and (len(args.suite) != 1 or args.runs != 1):
        raise RuntimeError("--out only makes sense with exactly one --suite and --runs 1 - "
                           "several captures cannot all land on one path")
    worktree = str(Path(args.worktree).resolve())
    started_at = now()
    entries = []
    with HeldLock(store, port, args.owner, args.purpose, args.wait, kind="batch") as held:
        flash_args = argparse.Namespace(owner=args.owner, purpose=args.purpose + " (flash)",
                                        wait=args.wait, worktree=args.worktree,
                                        variant=args.variant, out=None)
        build_id = flash(flash_args, store, port, held_lock=held, extra_flags=extra_flags)
        commit = git_commit(worktree)
        for run in range(1, args.runs + 1):
            for suite_name in args.suite:
                capture_at = now()
                out, _ = resolve_capture_path(
                    args.out, "runsuite-" + suite_name + "-run" + str(run), args.owner, capture_at)
                suite_args = argparse.Namespace(
                    owner=args.owner, wait=args.wait, suite=suite_name, out=str(out),
                    purpose=f"{args.purpose} ({suite_name} run {run}/{args.runs})",
                    max_seconds=args.max_seconds, idle_seconds=args.idle_seconds,
                    expect_build_id=build_id, verbose=getattr(args, "verbose", False))
                print(f"batch: {suite_name} run {run}/{args.runs}", flush=True)
                error = None
                try:
                    run_suite(suite_args, store, port, held_lock=held, worktree=worktree,
                             commit=commit)
                except RuntimeError as caught:
                    error = str(caught)
                    print("batch: capture error, continuing: " + error, file=sys.stderr)
                entries.append({"suite": suite_name, "run": run, "capture": str(out),
                                "error": error})
    meta = {"build_id": build_id, "owner": args.owner, "purpose": args.purpose,
            "runs": args.runs, "worktree": worktree, "commit": commit}
    summary_path, _ = resolve_capture_path(None, "batch", args.owner, started_at)
    summary_path = summary_path.with_suffix(".md")
    summary_path.write_text(device_report.batch_summary_markdown(entries, meta),
                            encoding="utf-8")
    append_manifest({"started_at": started_at.isoformat(), "port": port, "owner": args.owner,
                     "acquired_at": datetime.fromtimestamp(held.held["acquired_at"]).isoformat()
                     if held.held.get("acquired_at") else None,
                     "duration_seconds": time.time() - held.held["acquired_at"]
                     if held.held.get("acquired_at") else None,
                     "purpose": args.purpose, "command": "batch", "suite": ",".join(args.suite),
                     "build_id": build_id, "worktree": worktree, "commit": meta["commit"],
                     "reason": None,
                     "error": "; ".join(e["error"] for e in entries if e["error"]) or None,
                     "capture_path": str(summary_path),
                     "capture_bytes": summary_path.stat().st_size})
    print("batch summary: " + str(summary_path))
    return 1 if any(e["error"] for e in entries) else 0


def human_wait_seconds(value):
    try:
        seconds = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("wait must be a nonnegative number") from error
    if not math.isfinite(seconds) or seconds < 0:
        raise argparse.ArgumentTypeError("wait must be a nonnegative finite number")
    return seconds


def wait_for_human_release(store, port, reservation_id, seconds):
    deadline = time.monotonic() + seconds
    try:
        while True:
            human = store.status(port)["human"]
            if human is None:
                print("human reservation released")
                return 0
            if human.get("id") != reservation_id:
                print("human reservation replaced")
                return 4
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print("human reservation wait timed out")
                return 3
            time.sleep(min(1.0, remaining))
    except KeyboardInterrupt:
        print("human reservation wait interrupted")
        return 3


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--owner", default=os.environ.get("AUTANA_DEVICE_OWNER", "unknown"))
    parser.add_argument("--wait", type=float, default=600)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("status")
    release = subparsers.add_parser("release")
    release.add_argument("--token", required=True)
    hand = subparsers.add_parser("hand-to-human")
    hand.add_argument("--note", required=True)
    hand.add_argument("--token")
    hand.add_argument("--purpose", default="handing board to maintainer")
    hand.add_argument("--wait", type=human_wait_seconds)
    subparsers.add_parser("take-back")
    flash_parser = subparsers.add_parser("flash")
    flash_parser.add_argument("--variant", choices=("dev", "diag", "release"), required=True)
    flash_parser.add_argument("--worktree", required=True)
    flash_parser.add_argument("--purpose", default="flash")
    flash_parser.add_argument("--out")
    flash_parser.add_argument("--perf-scope", action="store_true",
                              help="with --variant diag: build the perf-scoped image")
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
    suite.add_argument("--purpose", default="run suite")
    listen_parser = subparsers.add_parser("listen")
    listen_duration = listen_parser.add_mutually_exclusive_group(required=True)
    listen_duration.add_argument("--seconds", type=float)
    listen_duration.add_argument("--follow", action="store_true")
    listen_parser.add_argument("--echo", action="store_true")
    listen_parser.add_argument("--out")
    listen_parser.add_argument("--purpose", default="listen")
    listen_parser.add_argument("--elf",
                               help="decode any crash addresses seen against this .elf's symbols")
    reset_parser = subparsers.add_parser("reset", help="reboot the board and wait for USB serial")
    reset_parser.add_argument("--capture", action="store_true",
                              help="capture the boot console after the reset")
    reset_parser.add_argument("--verbose", action="store_true")
    reset_parser.add_argument("--seconds", type=float, default=20.0,
                              help="boot capture window (default: 20)")
    reset_parser.add_argument("--out")
    reset_parser.add_argument("--purpose", default="reset")
    selftest_parser = subparsers.add_parser(
        "selftest", help="flash the diagnostics+autorun image and capture the on-device run "
                         "of every registered suite")
    selftest_parser.add_argument("--worktree", required=True)
    selftest_parser.add_argument("--out")
    selftest_parser.add_argument("--verbose", action="store_true")
    selftest_parser.add_argument("--perf-scope", action="store_true",
                                 help="build the perf-scoped image")
    # 3000 s leaves headroom over a full run's measured time - see
    # launcher/tools/quality/report_test_results.sh.
    selftest_parser.add_argument("--max-seconds", type=float, default=3000)
    selftest_parser.add_argument("--idle-seconds", type=float, default=300)
    selftest_parser.add_argument("--purpose", default="selftest")
    send_parser = subparsers.add_parser(
        "send", help="write one console line and print the device's replies to it")
    send_parser.add_argument("line")
    send_parser.add_argument("--reply", default="TUNE",
                             help="what a reply line contains (default: TUNE)")
    send_parser.add_argument("--until", action="append",
                             help="a reply prefix that ends the answer; repeatable "
                                  "(default: TUNE_OK, TUNE_ERR, TUNE_END)")
    send_parser.add_argument("--seconds", type=float, default=3.0)
    send_parser.add_argument("--purpose", default="send")
    send_parser.add_argument("--optional", action="store_true",
                             help="a timeout with nothing seen is success, not an error - "
                                  "for a verb that only answers when something is wrong")
    screenshot_parser = subparsers.add_parser(
        "screenshot", help="capture what the panel shows right now, as a .png plus a .json state snapshot")
    screenshot_parser.add_argument("--out",
                                   help="output path; any extension given is replaced with .png "
                                        "(default: a timestamped name in the current directory)")
    screenshot_parser.add_argument("--timeout", type=float, default=90.0)
    screenshot_parser.add_argument("--purpose", default="screenshot")
    screenshot_view = screenshot_parser.add_mutually_exclusive_group()
    screenshot_view.add_argument("--as-shown", action="store_true")
    screenshot_view.add_argument("--framebuffer", action="store_true")
    batch_parser = subparsers.add_parser(
        "batch", help="flash once, capture suites N times under one lock, write one summary")
    batch_parser.add_argument("--worktree", required=True)
    batch_parser.add_argument("--variant", choices=("dev", "diag", "release"), default="diag")
    batch_parser.add_argument("--suite", action="append", required=True,
                              help="a suite to capture; repeat for several")
    batch_parser.add_argument("--runs", type=int, default=3)
    batch_parser.add_argument("--verbose", action="store_true")
    batch_parser.add_argument("--perf-scope", action="store_true",
                              help="build the perf-scoped image")
    batch_parser.add_argument("--max-seconds", type=float, default=1800)
    batch_parser.add_argument("--idle-seconds", type=float, default=300)
    batch_parser.add_argument("--purpose", default="batch capture")
    batch_parser.add_argument("--out",
                              help="write the one capture here instead of the default path - "
                                   "only with exactly one --suite and --runs 1")
    report_parser = subparsers.add_parser("report")
    report_parser.add_argument("capture", help="an existing capture file (.log or .log.gz)")
    report_parser.add_argument("--index", help="override index.jsonl (default: records/device)")
    args = parser.parse_args(argv)

    # Touches no lock and no port - it only reads a capture already on disk,
    # so it is handled before port discovery even runs, unlike every command
    # below this.
    if args.command == "report":
        index_path = Path(args.index) if args.index else records_root() / "index.jsonl"
        try:
            report_path = device_report.write_report_for_capture(Path(args.capture), index_path)
        except Exception as error:
            print("device: report generation failed: " + str(error), file=sys.stderr)
            return 1
        print("report: " + str(report_path))
        return 0

    try:
        port = args.port or find_port()
        store = device_lock.LockStore()
        if args.command == "status":
            device_lock.print_status(store.status(port))
            return 0
        if args.command == "release":
            return 0 if store.release(port, args.token) else 1
        if args.command == "hand-to-human":
            active = store.status(port)["lock"]
            if active:
                if not args.token or not store.release(port, args.token):
                    raise RuntimeError("active lock requires its token before handoff")
            reservation_id = store.set_human(port, args.owner, args.note)
            if args.wait is None:
                print("human reservation recorded")
                return 0
            return wait_for_human_release(store, port, reservation_id, args.wait)
        if args.command == "take-back":
            store.clear_human(port)
            device_lock.print_status(store.status(port))
            return 0
        if args.command == "flash":
            extra_flags = ["--perf-scope"] if args.perf_scope else []
            flash(args, store, port, extra_flags=extra_flags)
        elif args.command == "run-suite":
            return run_suite(args, store, port)
        elif args.command == "selftest":
            return selftest(args, store, port)
        elif args.command == "reset":
            return reset_device(args, store, port)
        elif args.command == "batch":
            return batch(args, store, port)
        elif args.command == "send":
            if not args.until:
                args.until = [args.reply + "_OK", args.reply + "_ERR", args.reply + "_END"]
            return send(args, store, port)
        elif args.command == "screenshot":
            return screenshot(args, store, port)
        else:
            listen(args, store, port)
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("device: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    rerun = rerun_under_idf_python(sys.argv[1:])
    raise SystemExit(main() if rerun is None else rerun)
