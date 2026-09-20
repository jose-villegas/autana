"""Lock-aware commands for the single shared USB Serial/JTAG board."""

import argparse
import contextlib
import gzip
import json
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


BAUD = 115200
BUILD_ID = re.compile(rb"BUILD_ID=([^\s\r\n]+)")
SUITE_RESULT = re.compile(rb":\d+:.*:(PASS|FAIL)(?:\r?$|:)", re.MULTILINE)
IDF_PYTHON = Path(r"C:\Users\ville\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe")

# Suite/listen captures run 13-131 KB and a flash log ~270 KB; only a capture
# that lands on the default path (not an explicit --out) is ever gzipped, and
# only once it clears this, so the common case stays plain-text and greppable.
COMPRESS_ABOVE_BYTES = 200_000
SLUG_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]+")


def python_with_pyserial():
    return str(IDF_PYTHON) if IDF_PYTHON.is_file() else sys.executable


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
                   suite=None, build_id=None, worktree=None, reason=None, error=None, root=None):
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


def wait_for_port(port, seconds=PORT_WAIT_SECONDS, opener=None, sleep=time.sleep,
                  now=time.monotonic):
    """The lock arbitrates intent; the OS owns the port, and the two disagree
    whenever a previous holder's reader outlives its lock - an agent that
    queued fairly then fails on a port it was promised, which reads as a flaky
    board. Waiting is the right answer: this caller already won its turn, a
    straggler drains in seconds, and a port nobody ever frees still reports
    itself at the deadline."""
    open_port = opener or open_serial  # resolved per call, so a patched opener is honoured
    deadline = now() + seconds
    waited = False
    while True:
        try:
            open_port(port).close()
            if waited:
                print("port " + port + " came free", file=sys.stderr)
            return
        except OSError as error:
            if now() >= deadline:
                raise RuntimeError(
                    port + " is held by another process " + str(int(seconds))
                    + "s after this task won the lock: " + str(error)) from error
            if not waited:
                print("waiting for " + port + ", open elsewhere", file=sys.stderr)
                waited = True
            sleep(1.0)


def build_id_from_bytes(data):
    match = BUILD_ID.search(data)
    return match.group(1).decode("ascii", "replace") if match else None


def latest_build_id_from_bytes(data):
    matches = list(BUILD_ID.finditer(data))
    return matches[-1].group(1).decode("ascii", "replace") if matches else None


def read_expected_build_id(worktree, variant):
    build_dir = "build." + variant
    path = Path(worktree) / "launcher" / build_dir / "build_id.txt"
    try:
        return path.read_text(encoding="ascii").strip() or None
    except FileNotFoundError:
        return None


def count_suite_results(data):
    results = SUITE_RESULT.findall(data)
    return results.count(b"PASS"), results.count(b"FAIL")


class HeldLock:
    def __init__(self, store, port, owner, purpose, wait):
        self.store = store
        self.port = port
        self.held = store.acquire(port, owner, purpose, wait=wait)
        if not self.held:
            raise RuntimeError("device lock was not acquired")
        if self.held["log"]:
            print(self.held["log"], file=sys.stderr)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.keep_alive, daemon=True)

    def keep_alive(self):
        while not self.stop.wait(30):
            if not self.store.heartbeat(self.port, self.held["token"]):
                print("device lock was lost", file=sys.stderr)
                return

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, unused_type, unused_value, unused_traceback):
        self.stop.set()
        self.thread.join()
        self.store.release(self.port, self.held["token"])


def capture(connection, output, max_seconds, idle_seconds, expected_build_id=None,
            suite_name=None):
    data = bytearray()
    pending = b""
    seen_build_id = None
    last_non_shell = time.monotonic()
    deadline = last_non_shell + max_seconds
    suite_complete = None
    if suite_name:
        suite_complete = b"RUNSUITE_COMPLETE name=" + suite_name.encode("ascii") + b" "
    with open(output, "wb") as stream:
        while time.monotonic() < deadline:
            try:
                chunk = connection.read(4096)
            except OSError:
                # The USB serial port re-enumerates under the reader now and
                # then; what was read is still a capture worth reporting.
                return bytes(data), "port lost"
            if not chunk:
                if idle_seconds is not None and time.monotonic() - last_non_shell >= idle_seconds:
                    return bytes(data), "idle"
                continue
            data.extend(chunk)
            stream.write(chunk)
            stream.flush()
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
            if b"TESTS_DONE" in data or (b"Tests " in data and b"Failures" in data):
                return bytes(data), "complete"
    return bytes(data), "timeout"


def reset(port):
    command = [python_with_pyserial(), "-m", "esptool", "--chip", "esp32s3", "-p", port,
               "--after", "hard_reset", "chip_id"]
    subprocess.run(command, check=True)


def boot_build_id(port, seconds=12, expected_build_id=None):
    with open_serial(port) as connection:
        data, reason = capture(connection, os.devnull, seconds, 2, expected_build_id)
        actual = latest_build_id_from_bytes(data)
        if actual:
            return actual, reason
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
                    return actual, reason
        return None, reason


def holding(store, port, args, held_lock):
    """The lock a command runs under: a fresh one, or `held_lock` when a
    batch already holds the board for the whole sequence."""
    if held_lock is not None:
        return contextlib.nullcontext(held_lock)
    return HeldLock(store, port, args.owner, args.purpose, args.wait)


def flash(args, store, port, held_lock=None, extra_flags=()):
    with holding(store, port, args, held_lock) as held:
        wait_for_port(port)
        worktree = Path(args.worktree).resolve()
        script = worktree / "launcher" / "tools" / "build_flash.sh"
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
        build_id = None
        error = None
        try:
            with open(log, "wb") as stream:
                subprocess.run(command, cwd=worktree, stdin=subprocess.DEVNULL,
                               stdout=stream, stderr=subprocess.STDOUT, check=True,
                               env=environment)
            expected = read_expected_build_id(worktree, args.variant)
            if expected:
                store.set_expected_build_id(port, held.held["token"], expected)
            else:
                print("build id is unverified: build_id.txt is absent", file=sys.stderr)
            reset(port)
            actual, reason = boot_build_id(port, expected_build_id=expected)
            if not expected or not actual:
                print("build id is unverified: boot did not provide BUILD_ID", file=sys.stderr)
            elif actual != expected:
                raise RuntimeError(
                    "flashed build id mismatch: expected " + expected + ", got " + actual)
            else:
                build_id = actual
                print("verified BUILD_ID=" + actual + " (" + reason + ")")
        except (RuntimeError, subprocess.CalledProcessError) as caught:
            error = str(caught)
            raise
        finally:
            record_capture(log, managed, started_at=started_at, port=port, owner=args.owner,
                           purpose=args.purpose, command="flash", build_id=build_id,
                           worktree=str(worktree), commit=git_commit(worktree), error=error)
        return build_id or read_expected_build_id(worktree, args.variant)


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
        with holding(store, port, args, held_lock):
            wait_for_port(port)
            with open_serial(port) as connection:
                connection.write(("\nRUNSUITE " + args.suite + "\n").encode("ascii"))
                connection.flush()
                data, reason = capture(connection, output, args.max_seconds, args.idle_seconds,
                                       args.expect_build_id, args.suite)
    except RuntimeError as caught:
        error = str(caught)
        raise
    finally:
        final_path = record_capture(
            output, managed, started_at=started_at, port=port, owner=args.owner,
            purpose=args.purpose, command="run-suite", suite=args.suite,
            build_id=latest_build_id_from_bytes(data) or args.expect_build_id,
            worktree=worktree if worktree is not None else str(Path.cwd()),
            commit=commit if commit is not None else git_commit(), reason=reason, error=error)
        try:
            report_path = device_report.write_report_for_capture(
                final_path, records_root() / "index.jsonl")
            print("report: " + str(report_path))
        except Exception as report_error:  # a report is a convenience, never fails the capture
            print("report generation failed (capture is unaffected): " + str(report_error),
                  file=sys.stderr)
    passed, failed = count_suite_results(data)
    print("suite results: " + str(passed) + " PASS, " + str(failed) + " FAIL")
    print("suite capture ended: " + reason)
    return 1 if failed else 0


def listen(args, store, port):
    started_at = now()
    output, managed = resolve_capture_path(args.out, "listen", args.owner, started_at)
    data = b""
    reason = None
    error = None
    try:
        with HeldLock(store, port, args.owner, args.purpose, args.wait):
            wait_for_port(port)
            with open_serial(port) as connection:
                data, reason = capture(connection, output, args.seconds, None)
    except RuntimeError as caught:
        error = str(caught)
        raise
    finally:
        record_capture(output, managed, started_at=started_at, port=port, owner=args.owner,
                       purpose=args.purpose, command="listen",
                       build_id=latest_build_id_from_bytes(data), worktree=str(Path.cwd()),
                       commit=git_commit(), reason=reason, error=error)
    print("listen capture ended: " + reason)


def replies_to(data, reply, until):
    """The device's replies found in console bytes, and whether the last of
    them has arrived.

    A reply is everything from `reply` to the end of its line: the console
    also carries the firmware's own log lines, and a reply can come out
    behind a log prefix. `until` are the prefixes that end an answer.
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
    """
    data = bytearray()
    with HeldLock(store, port, args.owner, args.purpose, args.wait):
        wait_for_port(port)
        with open_serial(port) as connection:
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
    raise RuntimeError("no reply to '" + args.line + "' within " + str(args.seconds) +
                       " s - is a development build running?")


def build_script_supports(worktree, option):
    script = Path(worktree) / "launcher" / "tools" / "build_flash.sh"
    try:
        return option in script.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def batch(args, store, port):
    """Flash once and capture every suite `runs` times under ONE lock, then
    write one summary across all runs. Holding the board for the whole
    sequence is the point: another agent cannot flash between two captures
    of this image, and nothing here needs a model to wait on a capture. A
    capture that errors is recorded and the batch continues; only a failed
    build or flash stops it."""
    extra_flags = []
    if args.perf_scope:
        if not build_script_supports(args.worktree, "--perf-scope"):
            raise RuntimeError(
                "--perf-scope asked for, but this worktree's launcher/tools/build_flash.sh "
                "has no --perf-scope option; building the full image instead would measure "
                "a different thing than asked. Drop --perf-scope or add the option there.")
        extra_flags.append("--perf-scope")
    worktree = str(Path(args.worktree).resolve())
    started_at = now()
    entries = []
    with HeldLock(store, port, args.owner, args.purpose, args.wait) as held:
        flash_args = argparse.Namespace(owner=args.owner, purpose=args.purpose + " (flash)",
                                        wait=args.wait, worktree=args.worktree,
                                        variant=args.variant, out=None)
        build_id = flash(flash_args, store, port, held_lock=held, extra_flags=extra_flags)
        commit = git_commit(worktree)
        for run in range(1, args.runs + 1):
            for suite_name in args.suite:
                capture_at = now()
                out, _ = resolve_capture_path(
                    None, "runsuite-" + suite_name + "-run" + str(run), args.owner, capture_at)
                suite_args = argparse.Namespace(
                    owner=args.owner, wait=args.wait, suite=suite_name, out=str(out),
                    purpose=f"{args.purpose} ({suite_name} run {run}/{args.runs})",
                    max_seconds=args.max_seconds, idle_seconds=args.idle_seconds,
                    expect_build_id=build_id)
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
                     "purpose": args.purpose, "command": "batch", "suite": ",".join(args.suite),
                     "build_id": build_id, "worktree": worktree, "commit": meta["commit"],
                     "reason": None,
                     "error": "; ".join(e["error"] for e in entries if e["error"]) or None,
                     "capture_path": str(summary_path),
                     "capture_bytes": summary_path.stat().st_size})
    print("batch summary: " + str(summary_path))
    return 1 if any(e["error"] for e in entries) else 0


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
    subparsers.add_parser("take-back")
    flash_parser = subparsers.add_parser("flash")
    flash_parser.add_argument("--variant", choices=("dev", "diag", "release"), required=True)
    flash_parser.add_argument("--worktree", required=True)
    flash_parser.add_argument("--purpose", default="flash")
    flash_parser.add_argument("--out")
    suite = subparsers.add_parser("run-suite")
    suite.add_argument("suite")
    suite.add_argument("--out")
    suite.add_argument("--max-seconds", type=float, default=180)
    # A perf row can run silently for minutes, and a freshly flashed diag build
    # runs every suite at boot before serving RUNSUITE; an 8 s default ended a
    # perf capture after three tests and read as "the rows are missing".
    suite.add_argument("--idle-seconds", type=float, default=300)
    suite.add_argument("--expect-build-id")
    suite.add_argument("--purpose", default="run suite")
    listen_parser = subparsers.add_parser("listen")
    listen_parser.add_argument("--seconds", type=float, required=True)
    listen_parser.add_argument("--out")
    listen_parser.add_argument("--purpose", default="listen")
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
    batch_parser = subparsers.add_parser(
        "batch", help="flash once, capture suites N times under one lock, write one summary")
    batch_parser.add_argument("--worktree", required=True)
    batch_parser.add_argument("--variant", choices=("dev", "diag", "release"), default="diag")
    batch_parser.add_argument("--suite", action="append", required=True,
                              help="a suite to capture; repeat for several")
    batch_parser.add_argument("--runs", type=int, default=3)
    batch_parser.add_argument("--perf-scope", action="store_true",
                              help="build the perf-scoped image (needs build_flash.sh support)")
    batch_parser.add_argument("--max-seconds", type=float, default=1800)
    batch_parser.add_argument("--idle-seconds", type=float, default=300)
    batch_parser.add_argument("--purpose", default="batch capture")
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
            store.set_human(port, args.owner, args.note)
            print("human reservation recorded")
            return 0
        if args.command == "take-back":
            store.clear_human(port)
            device_lock.print_status(store.status(port))
            return 0
        if args.command == "flash":
            flash(args, store, port)
        elif args.command == "run-suite":
            return run_suite(args, store, port)
        elif args.command == "batch":
            return batch(args, store, port)
        elif args.command == "send":
            if not args.until:
                args.until = [args.reply + "_OK", args.reply + "_ERR", args.reply + "_END"]
            return send(args, store, port)
        else:
            listen(args, store, port)
        return 0
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print("device: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
