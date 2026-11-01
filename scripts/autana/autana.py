#!/usr/bin/env python3
"""autana: terminal commands for the autana engine repo.

    autana                  a console session with the device
    autana help [topic]     the commands, grouped; a topic is a group or a command

A system-installed tool, not tied to git or worktrees: run from any folder,
board-only commands (monitor, tap, tune, ...) work from anywhere, and a
command that needs a project (build, flash, selftest, suite --flash) acts
on the current directory, like `make -C`, or on --project PATH. Anything
that touches the board goes through scripts/device/device.py, which takes
the device lock. The command list is COMMAND_GROUPS at the end of this
file; docs/cli/ shows it, written by scripts/autana/cli_reference.py.
"""

import contextlib
import gzip
import importlib
import json
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile
import threading
import time
from collections import namedtuple
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher" / "tools" / "build"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))
from process_tree import launch_process_tree, close_process_tree, install_console_interrupt_handler  # noqa: E402
from console_protocol import reply_prefix
import device_lock  # noqa: E402  (path must be set up first)
import autana_config  # noqa: E402  (path must be set up first)
from espressif import idf_python, idf_note_lines, require_idf  # noqa: E402
from version import __version__  # noqa: E402
from device_capture import BUILD_ID_REQUEST, PERF_SEGMENT, parse_apps  # noqa: E402

# The launcher hint belongs to this invocation; children resolve their own caller.
CALLER_PID = os.environ.pop(device_lock.CALLER_PID_ENV, None)

EXIT_BUSY = autana_config.EXIT_BUSY
EXIT_INTERRUPTED = autana_config.EXIT_INTERRUPTED

VARIANTS = {"rel": "release", "release": "release", "dev": "dev", "diag": "diag"}

# What a directory needs to be an autana project, launcher/CMakeLists.txt,
# not a .git folder: this tool is not tied to git, and a project built from
# a tarball or a non-git checkout is still one.
PROJECT_MARKER = Path("launcher") / "CMakeLists.txt"


def owner():
    """What the device lock calls this autana: the global `--owner` label
    (OWNER_ENV), with this process's own pid still appended, when given:
    two shells that use the same label would otherwise see each other's lock as
    their own (board_holder()'s self-check compares by name); else
    device_lock.default_owner(), "<user>@<host>:<pid>"."""
    override = os.environ.get(OWNER_ENV)
    if override:
        return f"{override}:{os.getpid()}"
    return device_lock.default_owner()


def git(*args):
    """A git answer, or "", for the flash/build banner's branch and commit
    only. autana itself is never tied to git: a missing binary, or no
    repository here, is not an error, just nothing to show."""
    try:
        result = subprocess.run(["git", *args], capture_output=True, text=True)
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def git_ok(*args):
    try:
        return subprocess.run(["git", *args], capture_output=True).returncode == 0
    except OSError:
        return False


def resolve_project():
    """This invocation's own --project, or its cwd when none was given,
    like `make -C`/`idf.py -C`, never a search of parent directories.
    Either way the directory itself must carry PROJECT_MARKER; failing that
    names --project, never git."""
    project = Path(os.environ.get(PROJECT_ENV) or Path.cwd()).resolve()
    if not (project / PROJECT_MARKER).is_file():
        sys.exit(f"autana: {project} is not an autana project "
                 f"(no {PROJECT_MARKER.as_posix()}) - pass --project PATH")
    return str(project)


COMMAND_ENV = autana_config.COMMAND_ENV


@contextlib.contextmanager
def command_named(name):
    """Tells the device.py children of this command what to call the lock
    they take (the label `status` shows), for the length of the command."""
    previous = os.environ.get(COMMAND_ENV)
    os.environ[COMMAND_ENV] = name
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop(COMMAND_ENV, None)
        else:
            os.environ[COMMAND_ENV] = previous


def command_words(words):
    """`tune`, or `lock hand` / `debug freeze` for the two verb routers."""
    if len(words) > 1 and words[0] in ("lock", "debug") and not words[1].startswith("-"):
        return f"{words[0]} {words[1]}"
    return words[0]


def run_command(handler, args, name=None, flags=None):
    """The one place main() and console() dispatch a command through: the
    global `flags` (pop_globals()) hold for this call only, then the
    environment is as it was."""
    flags = {**session_flags, **(flags or {})}
    flags.setdefault("--project", str(Path.cwd()))
    with applied(flags):
        try:
            autana_config.load(Path(flags["--project"]))
        except autana_config.ConfigError as error:
            sys.exit(f"autana: {error}")
        with command_named(name) if name else contextlib.nullcontext():
            return handler(args)


def pop_value(args, flag):
    """`args` with `flag` and the value right after it removed, and that
    value: (None, args unchanged) when `flag` is absent."""
    if flag not in args:
        return None, args
    rest = list(args)
    index = rest.index(flag)
    if index + 1 >= len(rest):
        sys.exit(f"usage: {flag} needs a value")
    value = rest.pop(index + 1)
    rest.pop(index)
    return value, rest


def pop_layout_seed(args):
    """(seed or None, `args` without `--layout-seed N`): N is a non-negative
    whole number, 0 being the plain build."""
    value, rest = pop_value(args, "--layout-seed")
    if value is not None and not value.isdigit():
        sys.exit(f"usage: --layout-seed needs a whole number, not {value}")
    return value, rest


def refuse_release_seed(seed, variant):
    """A release image carries no padding to sample, so a seed there is a typo."""
    if seed is not None and variant == "release":
        sys.exit("autana: --layout-seed is for measuring; a release image is never padded")


def device_tool():
    # Under the same scripts/ as this file, so the device tool is the one
    # belonging to the checkout on the PATH.
    device = Path(__file__).resolve().parents[1] / "device" / "device.py"
    if not device.is_file():
        sys.exit(f"autana: {device} not found")
    return device


def device_module():
    """device.py itself, for a verb that runs in this process: one that
    needs neither pyserial nor the board, only device.py's own code."""
    folder = str(device_tool().parent)
    if folder not in sys.path:
        sys.path.insert(0, folder)
    import device
    return device


# How the global `--wait`, `--owner` and `--board` reach nested steps and child
# processes; private, so a caller's environment is never a second way to set
# them. The token is the one a running command's children prove their lock by.
WAIT_ENV = autana_config.WAIT_ENV
OWNER_ENV = autana_config.OWNER_ENV
BOARD_ENV = autana_config.BOARD_ENV
TOKEN_ENV = autana_config.TOKEN_ENV
PROJECT_ENV = autana_config.PROJECT_ENV
# Must exceed device.py's unwind, including lock_group/lock_job GRACE_SECONDS.
INTERRUPT_GRACE_SECONDS = 5


def device_command(*args, wait=None):
    """device.py imports pyserial, so it runs under ESP-IDF's Python even
    when some other interpreter started this file. `--owner`/`--wait` are
    device.py's own top-level flags, filled in one place for every board
    command: owner() names the lock holder, and `--wait` is a caller's own
    `wait` when it gives one (send()'s short fail-fast window, used only
    without a global `--wait`), else the global `--wait` (WAIT_ENV), else
    left to device.py's own 600 s default."""
    command = [idf_python(), "-u", str(device_tool()), "--owner", owner()]
    wait = wait if wait is not None else os.environ.get(WAIT_ENV)
    if wait:
        command += ["--wait", str(wait)]
    return command + list(args)


@contextlib.contextmanager
def device_process(command, **options):
    """Keep the device child with this CLI while sharing console interrupts."""
    process = launch_process_tree(command, parent_lifetime=True, new_session=False, **options)
    try:
        yield process
    except KeyboardInterrupt:
        try:
            process.wait(timeout=INTERRUPT_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            pass
        raise
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait()
        close_process_tree(process)


def call_device(command):
    with device_process(command) as process:
        return process.wait()


def capture_device(command, **options):
    with device_process(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        **options) as process:
        stdout, stderr = process.communicate()
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def reject_unknown(cmd, args, allowed=()):
    """Any leftover `--flag` a command's own parsing did not recognise:
    named, not folded into a generic usage dump. `allowed` are flags this
    command already popped by hand and still wants to see in `args`."""
    for arg in args:
        if arg.startswith("--") and arg not in allowed:
            sys.exit(f"autana {cmd}: unknown flag {arg}")
    return args


def follow(log, finished):
    """Print a growing log file until `finished` is set and the file is drained.

    device.py writes the build and flash output to this file and nowhere else,
    and may gzip it in place once it is done, so a read that fails ends the
    follow rather than the command.
    """
    position = 0
    while True:
        done = finished.is_set()
        try:
            with open(log, "rb") as stream:
                stream.seek(position)
                chunk = stream.read()
                position = stream.tell()
        except OSError:
            # Gzipped away between two polls: the tail is where a failure says
            # why, so finish from the compressed copy.
            try:
                with gzip.open(str(log) + ".gz", "rb") as stream:
                    chunk = stream.read()[position:]
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()
                return
            except OSError:
                if done:
                    return
                chunk = b""
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", errors="replace"))
            sys.stdout.flush()
        if done and not chunk:
            return
        time.sleep(0.2)


def run_streaming_its_log(command):
    finished = threading.Event()
    follower = None
    try:
        with device_process(command, stdout=subprocess.PIPE, text=True, bufsize=1) as process:
            for line in process.stdout:
                print(line, end="", flush=True)
                if follower is None and line.startswith("flash log: "):
                    follower = threading.Thread(target=follow, args=(line[len("flash log: "):].strip(), finished))
                    follower.start()
            return process.wait()
    finally:
        finished.set()
        if follower is not None:
            follower.join()


def variant_request(verb, args, flags, project):
    """The words `autana build` and `autana flash` share: one variant (dev
    when omitted) and the `flags` given, against `project`, already
    resolved by the caller. Prints the banner (branch/commit/dirty only
    when git answers for that project, never required) and returns the
    variant word asked, the variant and the flags seen."""
    reject_unknown(verb, args, flags)
    seen = {flag for flag in flags if flag in args}
    words = [arg for arg in args if arg not in flags]
    asked = words[0] if words else "dev"
    variant = VARIANTS.get(asked)
    if variant is None or len(words) > 1:
        sys.exit(f"usage: autana {verb} [rel|dev|diag] "
                 + " ".join(f"[{flag}]" for flag in flags))
    where = project
    if git_ok("-C", project, "rev-parse", "--is-inside-work-tree"):
        current_branch = git("-C", project, "branch", "--show-current") or "detached"
        commit = git("-C", project, "rev-parse", "--short", "HEAD")
        # Tracked changes only: an untracked scan reads every directory,
        # which CI's job inputs would record as listings.
        dirty = " (dirty)" if git("-C", project, "status", "--porcelain", "--untracked-files=no") else ""
        where = f"{current_branch} @ {commit}{dirty}"
    print(f"autana {verb}: {variant} of {where}", flush=True)
    idf_note(project, emit=False)
    return asked, variant, seen


def idf_note(project, emit=True):
    """Validate before dispatch; the build shell emits its resolved installation note."""
    try:
        if emit:
            print("\n".join(idf_note_lines(project)), flush=True)
        else:
            require_idf(project)
    except (ValueError, OSError) as error:
        sys.exit(str(error))


def flash(args):
    project = resolve_project()
    seed, args = pop_layout_seed(args)
    _, variant, seen = variant_request("flash", args, ("--quiet", "--perf-scope", "--frame-laps", "--hot-tunables"),
                                       project)
    refuse_release_seed(seed, variant)
    refuse_unscoped_laps(seen)
    if "--hot-tunables" in seen and variant == "release":
        sys.exit("autana: --hot-tunables needs dev or diag")
    quiet = "--quiet" in seen
    hot_tunables = "--hot-tunables" in seen
    perf_scope = "--perf-scope" in seen
    command = device_command(
        "flash", "--variant", variant, "--worktree", project,
    )
    if perf_scope:
        command.append("--perf-scope")
    if "--frame-laps" in seen:
        command.append("--frame-laps")
    if hot_tunables:
        command.append("--hot-tunables")
    if seed is not None:
        command += ["--layout-seed", seed]
    return call_device(command) if quiet else run_streaming_its_log(command)


def refuse_unscoped_laps(seen):
    if "--frame-laps" in seen and "--perf-scope" not in seen:
        sys.exit("autana: --frame-laps times the perf scope's frame stages, so it needs --perf-scope")


def build_diag_check(project):
    """The diagnostics build plus the complexity ratchet, unchanged from
    launcher/tools/build/build_diag_check.sh, the two halves of what CI's
    Build (Diagnostics) workflow decides, in one command, no board."""
    script = Path(project) / "launcher" / "tools" / "build" / "build_diag_check.sh"
    if not script.is_file():
        sys.exit(f"autana: {script} not found")
    return subprocess.call([device_module().git_bash(), str(script)], cwd=project)


def build(args):
    """Build this project with no board and no lock: the build half of
    `autana flash`, device.py's own, run in this process. `diag --check`
    runs the diagnostics build plus the complexity ratchet instead."""
    check = "--check" in args
    args = [arg for arg in args if arg != "--check"]
    project = resolve_project()
    if check:
        if args != ["diag"]:
            sys.exit("usage: autana build diag --check")
        return build_diag_check(project)
    seed, args = pop_layout_seed(args)
    _, variant, seen = variant_request("build", args, ("--perf-scope", "--frame-laps", "--hot-tunables", "--configure"),
                                       project)
    refuse_release_seed(seed, variant)
    refuse_unscoped_laps(seen)
    if "--hot-tunables" in seen and variant == "release":
        sys.exit("autana: --hot-tunables needs dev or diag")
    flags = sorted(seen) + (["--layout-seed", seed] if seed is not None else [])
    return device_module().build_worktree(project, variant, flags)


def seconds_argument(args, default, usage):
    if len(args) > 1:
        sys.exit(usage)
    if not args:
        return default
    try:
        return float(args[0])
    except ValueError:
        sys.exit(usage)


def buildid(args):
    """What the BOARD says it is running, asked of it rather than read out of
    a build directory: the point of the question is whether the two agree."""
    reject_unknown("buildid", args, ("--json",))
    json_output = read_json_flag(args, "usage: autana buildid [--json]")
    code, replies = send(BUILD_ID_REQUEST, reply="BUILD_ID")
    if code != 0 or not replies:
        return code or 1
    if json_output:
        print(json.dumps(parse_buildid(replies[-1])))
    else:
        print(replies[-1])
    return 0


BATTERY_PERCENT_MAX = 100
BATTERY_VOLTAGE_MAX_MV = 0x3fff


def battery(args):
    usage = "usage: autana battery [--json] | --log [--json | --clear]"
    reject_unknown("battery", args, ("--json", "--log", "--clear"))
    if len(args) != len(set(args)) or ("--clear" in args and ("--log" not in args or "--json" in args)):
        sys.exit(usage)
    if "--log" in args:
        return battery_log(args)
    json_output = read_json_flag(args, usage)
    code, replies = send("BATTERY", reply="BATTERY")
    if code or not replies:
        return code or 1
    try:
        result = parse_battery(replies[-1])
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps(result) if json_output else replies[-1])
    return 0


BATTERY_LOG_SLOTS = 360
BATTERY_LOG_LIMITS = dict(seq=(1 << 24) - 1, up_min=(1 << 10) - 1,
                          mv=BATTERY_VOLTAGE_MAX_MV, percent=(1 << 7) - 1)


def parse_battery_log(replies):
    if not replies or not re.fullmatch(r"BATLOG end count=[0-9]+", replies[-1]):
        raise ValueError("invalid BATLOG terminator")
    count = int(replies[-1].split("=")[-1])
    if count != len(replies) - 1 or count > BATTERY_LOG_SLOTS:
        raise ValueError("invalid BATLOG count")
    records = []
    for line in replies[:-1]:
        match = re.fullmatch(
            r"BATLOG seq=([0-9]+) up_min=([0-9]+) mv=([0-9]+) percent=([0-9]+) "
            r"usb=([01]) battery=([01]) charge=(unknown|trickle|pre|cc|cv|done|idle) "
            r"flow=(unknown|standby|charging|discharging) reason=(periodic|transition|boot|shutdown-warning)", line)
        if not match:
            raise ValueError(f"invalid BATLOG reply: {line}")
        seq, up_min, mv, percent, usb, present, charge, flow, reason = match.groups()
        record = dict(seq=int(seq), up_min=int(up_min), mv=int(mv), percent=int(percent),
                      usb=usb == "1", battery=present == "1", charge=charge, flow=flow, reason=reason)
        if any(record[key] > limit for key, limit in BATTERY_LOG_LIMITS.items()):
            raise ValueError(f"BATLOG value outside packed range: {line}")
        records.append(record)
    return records


def battery_log(args):
    clear = "--clear" in args
    if clear:
        code, replies = send("BATTERY LOG CLEAR", reply="BATLOG")
    else:
        code, replies = send("BATTERY LOG", reply="BATLOG", until=["BATLOG end", "BATLOG error"])
    if code or not replies:
        return code or 1
    try:
        if clear:
            if replies != ["BATLOG cleared"]:
                raise ValueError("invalid BATLOG clear reply")
            result = None
        else:
            result = parse_battery_log(replies)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps(result) if "--json" in args else "\n".join(replies))
    return 0


def parse_battery(reply):
    match = re.fullmatch(
        r"BATTERY valid=([01]) usb=([01]) battery=([01]) mv=([0-9]+) percent=([0-9]+) "
        r"charge=(unknown|trickle|pre|cc|cv|done|idle) "
        r"flow=(unknown|standby|charging|discharging) reg00=0x([0-9a-fA-F]{2}) reg01=0x([0-9a-fA-F]{2})", reply)
    if not match:
        raise ValueError(f"invalid BATTERY reply: {reply}")
    valid, usb, present, mv, percent, charge, flow, reg00, reg01 = match.groups()
    if int(percent) > BATTERY_PERCENT_MAX or int(mv) > BATTERY_VOLTAGE_MAX_MV:
        raise ValueError(f"invalid BATTERY reply: {reply}")
    return dict(valid=valid == "1", usb=usb == "1", battery=present == "1", mv=int(mv),
                percent=int(percent), charge=charge, flow=flow, reg00=int(reg00, 16), reg01=int(reg01, 16))


def framewatch(args):
    """The frame watch's counts for the last frame and the sites repeating
    now, as the board's own JSON, a development build only."""
    reject_unknown("framewatch", args)
    if args:
        sys.exit("usage: autana framewatch")
    code, replies = send("FRAMEWATCH", reply="FRAMEWATCH")
    if code != 0 or not replies:
        return code or 1
    print(parse_framewatch(replies[-1]))
    return 0


def heap(args):
    """Runtime free bytes, largest block and total internal DMA heap."""
    reject_unknown("heap", args, ("--json",))
    json_output = read_json_flag(args, "usage: autana heap [--json]")
    code, replies = send("HEAP", reply="HEAP")
    if code != 0 or not replies:
        return code or 1
    print(json.dumps(parse_heap(replies[-1])) if json_output else replies[-1])
    return 0


# Covers codec open, amplifier startup, capture settling and transport overhead.
TONE_MARGIN_SECONDS = 10
TONE_CAPTURE_COUNT = 2
TONE_MS_PER_SECOND = 1000


def read_numeric_options(args, usage, allowed_flags, counts):
    rest = list(args)
    flags = []
    while rest and rest[-1].startswith("--"):
        flags.append(rest.pop())
    if (len(flags) != len(set(flags)) or any(flag not in allowed_flags for flag in flags)
            or len(rest) not in counts or any(not re.fullmatch(r"[0-9]+", value) for value in rest)):
        sys.exit(usage)
    return rest, flags


def tone(args):
    usage = "usage: autana tone <hz> <ms> [volume] [--internal] [--json]"
    rest, flags = read_numeric_options(args, usage, ("--json", "--internal"), (2, 3))
    json_output = "--json" in flags
    internal = "--internal" in flags
    ms = int(rest[1])
    seconds = TONE_MARGIN_SECONDS + TONE_CAPTURE_COUNT * ms / TONE_MS_PER_SECOND
    code, replies = send("TONE " + " ".join(rest) + (" internal" if internal else ""), reply="TONE", seconds=seconds)
    if code != 0 or not replies:
        return code or 1
    if replies[-1].startswith(("TONE usage:", "TONE error:")):
        print(replies[-1], file=sys.stderr)
        return 1
    try:
        result = parse_tone(replies[-1])
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps(result) if json_output else replies[-1])
    return 0


SOUND_ONSET_SECONDS = 30


def parse_melody_info(reply):
    match = re.fullmatch(r"MELODY_INFO notes=([0-9]+) check_ms=([0-9]+) play_max_ms=([0-9]+) "
                         r"cents_per_octave=([0-9]+)", reply)
    if not match:
        raise ValueError(f"invalid MELODY_INFO reply: {reply}")
    return dict(zip(("notes", "check_ms", "play_max_ms", "cents_per_octave"), map(int, match.groups())))


def melody_constants():
    """What the image on the board times and checks a run by: `melody ?`."""
    code, replies = send("MELODY ?", reply="MELODY_INFO")
    if code or not replies:
        sys.exit(code or 1)
    try:
        return parse_melody_info(replies[-1])
    except ValueError as error:
        sys.exit(str(error))


def parse_melody_note(reply, cents_per_octave):
    match = re.fullmatch(r"MELODY note=([A-G]#?[0-9]+) (TONE .+)", reply)
    if not match:
        raise ValueError(f"invalid MELODY note reply: {reply}")
    note, tone_reply = match.groups()
    result = parse_tone(tone_reply)
    measured = result["measured_hz"]
    if measured is not None and (measured <= 0 or result["hz"] <= 0):
        raise ValueError(f"invalid MELODY frequency: {reply}")
    return dict(result, note=note, cents_error=None if measured is None else
                cents_per_octave * math.log2(measured / result["hz"]))


def melody(args):
    usage = "usage: autana melody [volume] [--check] [--internal] [--json]"
    rest, flags = read_numeric_options(args, usage, ("--check", "--internal", "--json"), (0, 1))
    if "--internal" in flags and "--check" not in flags:
        sys.exit(usage)
    check = "--check" in flags
    words = ["MELODY"] + (["check"] if check else []) + rest + (["internal"] if "--internal" in flags else [])
    constants = melody_constants()
    seconds = (constants["notes"] * (TONE_MARGIN_SECONDS + TONE_CAPTURE_COUNT * constants["check_ms"] / TONE_MS_PER_SECOND)
               if check else constants["play_max_ms"] / TONE_MS_PER_SECOND)
    code, replies = send(" ".join(words), reply="MELODY", until=["MELODY_END", "MELODY_ERR"], seconds=seconds)
    if code or not replies:
        return code or 1
    try:
        if replies[-1].startswith("MELODY_ERR"):
            raise ValueError(replies[-1])
        end = re.fullmatch(r"MELODY_END notes=([0-9]+)" + ("" if check else r" ms=([0-9]+)"), replies[-1])
        if not end or int(end[1]) != constants["notes"]:
            raise ValueError(f"invalid MELODY end reply: {replies[-1]}")
        if check:
            result = [parse_melody_note(line, constants["cents_per_octave"]) for line in replies[:-1]]
            if len(result) != int(end[1]):
                raise ValueError("incomplete MELODY check")
        else:
            if len(replies) != 1:
                raise ValueError("unexpected MELODY play replies")
            result = {"notes": int(end[1]), "ms": int(end[2])}
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps(result) if "--json" in flags else "\n".join(replies))
    return int(check and any(not note["heard"] for note in result))


def play(args):
    return sound(["play", *args])


def sound(args):
    prefix = "SOUND"
    code, replies = send(prefix + " " + " ".join(args), reply=prefix,
                         until=[prefix + "_END", prefix + "_ERR"],
                         **({"seconds": SOUND_ONSET_SECONDS} if args[:1] == ["onset"] else {}))
    if code or not replies:
        return code or 1
    if replies[-1].startswith("SOUND_ERR"):
        print(replies[-1], file=sys.stderr)
        return 1
    print("\n".join(replies))
    return 0


def parse_tone(reply):
    match = re.fullmatch(
        r"TONE (heard|not heard) ([0-9]+) Hz: (-?[0-9]+\.[0-9]+) dB "
        r"\(silence (-?[0-9]+\.[0-9]+) dB\), (?:noise (-?[0-9]+\.[0-9]+) dBFS, )?path (internal|acoustic), "
        r"measured (-?[0-9]+\.[0-9]+ Hz|n/a), open ([0-9]+) B DMA"
        r"(?:, thd (-?[0-9]+\.[0-9]+|n/a) dB, peak (-?[0-9]+\.[0-9]+) dBFS)?"
        r"(?:, shake (?:n/a|([0-9]+\.[0-9]+) mg rms ([0-9]+\.[0-9]+) mg peak \(silence ([0-9]+\.[0-9]+) mg rms\)))?", reply)
    if not match:
        raise ValueError(f"invalid TONE reply: {reply}")
    heard, hz, tone_db, silence_db, noise_dbfs, path, measured_hz, dma, thd_db, peak_dbfs, *shake = match.groups()
    shake_rms_mg, shake_peak_mg, silence_shake_rms_mg = (None if value is None else float(value) for value in shake)
    measured_hz = None if measured_hz == "n/a" else float(measured_hz.removesuffix(" Hz"))
    # hz_error is Hz: measured minus requested.
    return {"hz": int(hz), "tone_db": float(tone_db), "silence_db": float(silence_db),
            "noise_dbfs": None if noise_dbfs is None else float(noise_dbfs),
            "heard": heard == "heard", "open_dma_bytes": int(dma), "path": path,
            "thd_db": None if thd_db in (None, "n/a") else float(thd_db), "peak_dbfs": None if peak_dbfs is None else float(peak_dbfs),
            "measured_hz": measured_hz, "hz_error": None if measured_hz is None else measured_hz - int(hz),
            # Board vibration (IMU, mean removed) during the tone and the silence before it; None without an IMU.
            "shake_rms_mg": shake_rms_mg, "shake_peak_mg": shake_peak_mg, "silence_shake_rms_mg": silence_shake_rms_mg}


def parse_heap(reply):
    match = re.fullmatch(r"HEAP free=(\d+) largest=(\d+) total=(\d+)", reply)
    if not match:
        raise ValueError(f"invalid HEAP reply: {reply}")
    free, largest, total = map(int, match.groups())
    return {"free": free, "largest": largest, "total": total}


def parse_framewatch(reply):
    return reply.removeprefix("FRAMEWATCH ").strip()


def read_json_flag(args, usage):
    if args == ["--json"]:
        return True
    if args:
        sys.exit(usage)
    return False


def parse_buildid(reply):
    return {"build_id": reply.removeprefix("BUILD_ID=")}


def monitor(args):
    """A terminal gets the live stream; a pipe or script gets only the error
    lines and the verdict, and must say how long to listen so it cannot hang.
    device.py decodes crashes using --elf or the capture's BUILD_ID."""
    elf = None
    rest = list(args)
    usage = "usage: autana monitor [seconds] [--follow] [--stream] [--elf PATH] [--out PATH]"
    if "--elf" in rest:
        index = rest.index("--elf")
        if index + 1 >= len(rest):
            sys.exit(usage)
        elf = rest[index + 1]
        del rest[index:index + 2]
    out, rest = pop_value(rest, "--out")
    follow = "--follow" in rest
    if follow:
        rest.remove("--follow")
    stream = "--stream" in rest
    if stream:
        rest.remove("--stream")
    reject_unknown("monitor", rest)
    if follow and rest:
        sys.exit(usage)
    seconds = seconds_argument(rest, None, usage) if not follow else None
    terminal = sys.stdout.isatty()
    if seconds is None and not follow:
        if not terminal:
            print(usage, file=sys.stderr)
            raise SystemExit(2)
        follow = True
    command = device_command("listen")
    command += ["--follow"] if follow else ["--seconds", str(seconds)]
    if terminal or stream:
        command.append("--echo")
    if elf:
        command += ["--elf", elf]
    if out:
        command += ["--out", out]
    with device_process(command) as process:
        interrupted = False
        # Ctrl+C reaches device.py too; it saves the capture before exiting.
        while True:
            try:
                return process.wait()
            except KeyboardInterrupt:
                if interrupted:
                    return EXIT_INTERRUPTED
                interrupted = True


def reset(args):
    """Reboot the board, optionally recording its boot console."""
    capture = False
    rest = list(args)
    verbose = "--verbose" in rest
    if verbose:
        rest.remove("--verbose")
    if "--capture" in rest:
        rest.remove("--capture")
        capture = True
    usage = "usage: autana reset [--capture [seconds]] [--verbose]"
    reject_unknown("reset", rest)
    seconds = seconds_argument(rest, None, usage)
    if rest and not capture:
        sys.exit(usage)
    command = device_command("reset")
    if capture:
        command += ["--capture"]
        if seconds is not None:
            command += ["--seconds", str(seconds)]
    if verbose:
        command.append("--verbose")
    return call_device(command)


def coredump(args):
    """The last panic's core dump from flash, saved and decoded; --core
    decodes a saved one with no board; --erase clears it."""
    elf, rest = pop_value(list(args), "--elf")
    core, rest = pop_value(rest, "--core")
    erase = "--erase" in rest
    if erase:
        rest.remove("--erase")
    reject_unknown("coredump", rest)
    if rest or (erase and (elf or core)):
        sys.exit("usage: autana coredump [--core FILE] [--elf PATH] | autana coredump --erase")
    command = device_command("coredump")
    if erase:
        command.append("--erase")
    if elf:
        command += ["--elf", elf]
    if core:
        command += ["--core", core]
    return call_device(command)


def selftest(args):
    """Build+flash the diagnostics+autorun image and run every suite this
    project registers, on the device. Can take minutes: the full run's
    own budget, not a bug in this command."""
    rest = list(args)
    verbose = "--verbose" in rest
    if verbose:
        rest.remove("--verbose")
    hot_tunables = "--hot-tunables" in rest
    if hot_tunables:
        rest.remove("--hot-tunables")
    perf_scope = "--perf-scope" in rest
    if perf_scope:
        rest.remove("--perf-scope")
    out, rest = pop_value(rest, "--out")
    usage = "usage: autana selftest [seconds] [--verbose] [--perf-scope] [--hot-tunables] [--out PATH]"
    reject_unknown("selftest", rest)
    seconds = seconds_argument(rest, 3000.0, usage)
    project = resolve_project()
    print(f"autana selftest: every suite, {project}", flush=True)
    command = device_command(
        "selftest", "--worktree", project, "--max-seconds", str(seconds),
    )
    if verbose:
        command.append("--verbose")
    if perf_scope:
        command.append("--perf-scope")
    if hot_tunables:
        command.append("--hot-tunables")
    if out:
        command += ["--out", out]
    return call_device(command)


BATCH_USAGE = ("usage: autana batch <suite> [<suite> ...] [--runs N] [--perf-scope] [--verbose] "
              "[--out PATH] [--expect-build-id ID]")


def batch(args):
    """The old spelling of `suite <name>... --flash`, still the way to flash
    once and capture several suites `--runs` times (3 when omitted) under
    one lock; always the diagnostics image, since a suite only exists to
    run in one."""
    print("autana batch: use `autana suite <name>... --flash` - this spelling still works",
         file=sys.stderr)
    names, rest = [], list(args)
    while rest and not rest[0].startswith("--"):
        names.append(rest.pop(0))
    if not names:
        sys.exit(BATCH_USAGE)
    runs, rest = pop_value(rest, "--runs")
    return suite(names + ["--runs", runs or "3", "--flash"] + rest)


def status(args):
    """Every board, whether it is free, and if not who has it and until when."""
    reject_unknown("status", args, ("--json",))
    if read_json_flag(args, "usage: autana status [--json]"):
        return call_device(device_command("status", "--json"))
    return call_device(device_command("status"))


def devices(args):
    reject_unknown("devices", args, ("--json",))
    read_json_flag(args, "usage: autana devices [--json]")
    return call_device(device_command("devices", *args))


def release(args):
    """Release a lock this session holds, before its own command would have.
    The token is in TOKEN_ENV in every process that command started, which
    is where it comes from when none is given."""
    token = args[0] if args else os.environ.get(TOKEN_ENV)
    if len(args) > 1 or not token:
        sys.exit("usage: autana lock release [<token>] - without one, the "
                 "token of the command that holds the lock, inside that command")
    return call_device(device_command("release", "--token", token))


def take(args):
    if "--until-released" in args and "--pid" not in args:
        args = ["--pid", str(device_lock.caller_pid(CALLER_PID)), *args]
    return call_device(device_command("take", *args))


def lock_wait(args):
    reject_unknown("lock wait", args)
    if args:
        sys.exit("usage: autana lock wait [--wait <seconds>]")
    cap = os.environ.get(WAIT_ENV)
    return call_device(device_command("wait-free", *(["--cap", cap] if cap is not None else [])))


def lock_run(args):
    if "--" not in args:
        sys.exit("usage: autana lock run [--heavy] [--keep-on-failure <seconds>] [--estimate <seconds>] -- <command...>")
    return call_device(device_command("run", *args))


def hand(args):
    """Reserve the board for a maintainer sitting at it. New work waits until
    `autana lock take-back`, or an hour after the last `lock hand`, which
    running it again renews."""
    usage = "usage: autana lock hand [--until-back <seconds>] <note...>"
    wait = []
    if args and args[0] == "--until-back":
        if len(args) < 3:
            sys.exit(usage)
        wait = ["--until-back", args[1]]
        args = args[2:]
    if not args:
        sys.exit(usage)
    command = device_command("hand-to-human", "--note", " ".join(args), *wait)
    if not wait:
        return call_device(command)
    process = None
    try:
        with device_process(command) as process:
            return process.wait()
    except KeyboardInterrupt:
        if process is None or process.returncode != EXIT_INTERRUPTED:
            print("human reservation wait interrupted")
        return EXIT_INTERRUPTED


def take_back(args):
    """Clear a reservation `autana lock hand` made, freeing the board again."""
    reject_unknown("lock take-back", args)
    if args:
        sys.exit("usage: autana lock take-back")
    return call_device(device_command("take-back"))


def parse_suite_list(replies):
    """`RUNSUITE ?` replies -> [{"name", "on_request"}], in name order."""
    if not replies or not re.fullmatch(r"RUNSUITE_END count=([0-9]+)", replies[-1]):
        raise ValueError("autana suite list: no suite listing - the board needs a self-test "
                         "image (autana flash diag)")
    rows = [re.fullmatch(r"RUNSUITE_SUITE name=(\w+) on_request=([01])", reply) for reply in replies[:-1]]
    if None in rows or len(rows) != int(replies[-1].split("=")[1]):
        raise ValueError(f"autana suite list: incomplete listing: {replies}")
    return sorted(({"name": row[1], "on_request": row[2] == "1"} for row in rows), key=lambda row: row["name"])


def suite_list(args):
    """What the self-test image on the board registers (`RUNSUITE ?`):
    what this lists is exactly what `suite <name>` can run there."""
    json_output = "--json" in args
    args = [arg for arg in args if arg != "--json"]
    if len(args) > 1:
        sys.exit("usage: autana suite list [text] [--json]")
    wanted = args[0].lower() if args else ""
    code, replies = send("RUNSUITE ?", reply="RUNSUITE_", until=["RUNSUITE_END"])
    try:
        found = parse_suite_list(replies)
    except ValueError as error:
        print(error, file=sys.stderr)
        return code or 1
    shown = [row for row in found if wanted in row["name"].lower()]
    if json_output:
        print(json.dumps({"suites": shown}))
        return 0
    for row in shown:
        print(f"  {row['name']}" + (" [on request]" if row["on_request"] else ""))
    kept = f" matching '{wanted}'" if wanted else ""
    print(f"{len(shown)} suite(s){kept}")
    if any(row["on_request"] for row in shown):
        print("[on request] is left out of a full run: asking for it by name is the only way it runs")
    return 0


SUITE_USAGE = ("usage: autana suite <name> [<name> ...] [seconds] [--runs N] [--test PATTERN] "
              "[--flash] [--perf-scope] [--layout-seed N] [--hot-tunables] [--set NAME=VALUE] [--send LINE] [--verbose] [--out PATH] "
              "[--expect-build-id ID] | "
              "autana suite list [text]")


def suite(args):
    """One or more registered suites, captured under one lock, against the
    image already on the board, or, with `--flash`, built and flashed first.
    The name is each suite's own function, as SUITE_REGISTER() in its source
    spells it. `seconds` caps each capture (1800 when omitted, and a
    silent board ends one sooner). `--test` (repeatable, or a comma
    list) narrows the run, on the device, to the tests whose name contains
    a pattern. `--set NAME=VALUE` (repeatable) sets a tunable under each
    capture's own lock, just before it runs, and stops the batch unless the
    board echoes the value. `--send LINE` repeats console lines before every
    capture; the sequence should be idempotent."""
    if not args:
        sys.exit(SUITE_USAGE)
    if args[0] == "list":
        return suite_list(args[1:])
    positional, rest = [], list(args)
    while rest and not rest[0].startswith("--"):
        positional.append(rest.pop(0))
    if not positional:
        sys.exit(SUITE_USAGE)
    seconds = None
    if len(positional) > 1:
        try:
            seconds = float(positional[-1])
        except ValueError:
            pass
        else:
            positional.pop()
    names = positional
    if not names:
        sys.exit(SUITE_USAGE)
    flash = "--flash" in rest
    if flash:
        rest.remove("--flash")
    hot_tunables = "--hot-tunables" in rest
    if hot_tunables:
        rest.remove("--hot-tunables")
    perf_scope = "--perf-scope" in rest
    if perf_scope:
        rest.remove("--perf-scope")
    seed, rest = pop_layout_seed(rest)
    verbose = "--verbose" in rest
    if verbose:
        rest.remove("--verbose")
    runs, rest = pop_value(rest, "--runs")
    out, rest = pop_value(rest, "--out")
    expect_build_id, rest = pop_value(rest, "--expect-build-id")
    tests = []
    while "--test" in rest:
        value, rest = pop_value(rest, "--test")
        tests.append(value)
    settings = []
    while "--set" in rest:
        value, rest = pop_value(rest, "--set")
        name, equals, number = value.partition("=")
        try:
            int(number, 0)
        except ValueError:
            equals = ""
        if not name or not equals:
            sys.exit(f"usage: --set NAME=VALUE with a whole number, decimal or 0x hex, not {value}")
        settings.append(value)
    console_lines = []
    while "--send" in rest:
        value, rest = pop_value(rest, "--send")
        console_lines.append(value)
    reject_unknown("suite", rest)
    if rest:
        sys.exit(SUITE_USAGE)
    # Only --flash reads the project; it is what gets built. Without it,
    # `suite` is board-only: the capture still names a project (device.py's
    # own --worktree is metadata, not something it builds), but any
    # directory does, unvalidated.
    project = resolve_project() if flash else autana_config.project_dir().as_posix()
    runs = runs or "1"
    filtered = f" --test {','.join(tests)}" if tests else ""
    print(f"autana suite: {', '.join(names)} x{runs}{filtered}" + (" (flash)" if flash else ""),
          flush=True)
    command = device_command(
        "batch", "--worktree", project, "--variant", "diag", "--runs", runs,
    )
    if seconds is not None:
        command += ["--max-seconds", str(seconds)]
    for value in tests:
        command += ["--test", value]
    for value in settings:
        command += ["--set", value]
    for line in console_lines:
        command += ["--send", line]
    if not flash:
        command.append("--no-flash")
    for name in names:
        command += ["--suite", name]
    if perf_scope:
        command.append("--perf-scope")
    if hot_tunables:
        command.append("--hot-tunables")
    if seed is not None:
        command += ["--layout-seed", seed]
    if verbose:
        command.append("--verbose")
    if out:
        command += ["--out", out]
    if expect_build_id:
        command += ["--expect-build-id", expect_build_id]
    return call_device(command)


# A console line is asked from a prompt somebody is sitting at: a board another
# owner holds is reported at once rather than waited for, which from the
# outside is a terminal that has hung. The few seconds cover a lock changing
# hands between the look and the send. A global --wait asks to queue instead.
SEND_WAIT_S = 5


def refused_as_busy():
    """True, having said so, when another owner holds the board and no
    global --wait asked to wait for it."""
    if os.environ.get(WAIT_ENV):
        return False
    holder = board_holder()
    if holder:
        print(f"the board is busy - {holder}\nnothing was sent; try again when it is free, "
              "or give --wait", file=sys.stderr)
    return bool(holder)


def board_holder():
    """'held by <owner> for <purpose>' when someone else has the board a
    command would use, else ''. That board is the global --board's, else the only
    one plugged in; with several plugged in and none named, device.py's own
    refusal says so.

    Every line of a console session is a device.py of its own under one
    autana, so a lock this autana already holds is not somebody else's and
    the session does not refuse itself."""
    result = capture_device(device_command("status", "--json"), text=True)
    try:
        boards = json.loads(result.stdout)["boards"]
    except (ValueError, KeyError, TypeError):
        return ""
    # device.py already narrows the list to the named board, plugged or not.
    candidates = boards if os.environ.get(BOARD_ENV) else [b for b in boards if b["port"]]
    if len(candidates) != 1:
        return ""
    board = candidates[0]
    holder = board["holder"]
    token = os.environ.get(TOKEN_ENV)
    if token and device_lock.LockStore().check_token(board["board"], token):
        return ""
    if board["state"] == "held" and holder["owner"] != owner():
        return f"held by {holder['owner']} for {holder['purpose']}"
    return ""


def send(line, reply="TUNE", optional=False, seconds=None, until=None):
    """One console line to the device, under the lock. Returns (exit code,
    reply lines). `reply` is what the answer's lines start with. `until` are
    the prefixes that end the answer; one reply-line verb needs only
    `[reply]` itself (device.py's default when `until` is omitted; services/tune's
    three endings are its own default for `reply="TUNE"`), a multi-line one
    (an app's own command) passes `[<PREFIX>_END, <PREFIX>_ERR]`. `optional`
    is for a verb that answers only when something is wrong (TOUCH, IMU): a
    timeout with nothing seen is success, not "no reply", since silence is
    that verb's normal happy path."""
    if refused_as_busy():
        return EXIT_BUSY, []
    command = device_command("send", line,
                             wait=None if os.environ.get(WAIT_ENV) else SEND_WAIT_S)
    if reply != "TUNE":
        command += ["--reply", reply]
        for one_until in until if until is not None else [reply]:
            command += ["--until", one_until]
    if optional:
        command.append("--optional")
    if seconds is not None:
        command += ["--seconds", str(seconds)]
    result = capture_device(command, text=True)
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)
    return result.returncode, [answer for answer in result.stdout.splitlines() if answer]


def screenshot(args):
    """SCREENSHOT, captured and decoded by launcher/tools/device/screenshot.py's own
    read_screenshot()/write_capture(), under the device lock, see
    device.py's own `screenshot` subcommand."""
    out = None
    view = None
    frames = None
    usage = "usage: autana screenshot [--as-shown|--framebuffer] [-o PATH] [--frames N]"
    while args:
        arg = args.pop(0)
        if arg in ("-o", "--out") and args and out is None:
            out = args.pop(0)
        elif arg in ("--as-shown", "--framebuffer") and view is None:
            view = arg
        elif arg == "--frames" and args and frames is None:
            count = args.pop(0)
            if not (count.isdigit() and int(count) >= 1):
                sys.exit(usage + " - N is a positive count")
            frames = int(count)
        elif arg.startswith("--"):
            reject_unknown("screenshot", [arg])
        else:
            sys.exit(usage)
    if frames is not None:
        if out is None:
            sys.exit(usage + " - --frames needs -o PATH, the prefix of PATH-00, PATH-01, ...")
        return screenshot_frames(frames, out, view)
    return capture_screenshot(out, view)


def screenshot_frames(frames, out, view):
    """N consecutive frames: one capture while running (a band-mode app's
    first capture needs a frame to fill its copy of the panel), then FREEZE
    and a STEP between captures, then RESUME."""
    code = capture_screenshot(None, view)
    if code:
        return code
    code, _ = send("FREEZE", reply="FREEZE_STATE")
    for i in range(frames):
        if code:
            break
        if i:
            code, _ = send("STEP", reply="FREEZE_STATE")
            if code:
                break
        code = capture_screenshot(f"{out}-{i:02d}", view)
    resumed, _ = send("RESUME", reply="FREEZE_STATE")
    return code or resumed


def capture_screenshot(out, view):
    if refused_as_busy():
        return EXIT_BUSY
    command = device_command("screenshot")
    if out:
        command += ["--out", out]
    if view:
        command.append(view)
    return call_device(command)


def mic(args):
    import argparse
    parser = argparse.ArgumentParser(prog="autana mic")
    parser.add_argument("action", choices=("record",))
    parser.add_argument("ms", type=int)
    parser.add_argument("--settle", type=int)
    parser.add_argument("--at", nargs="+", action="append",
                             help="offset ms from take start; negative offsets fall in settling; "
                                  "followed by reg value or tone hz duration_ms gain",
                             type=lambda word: word if word == "tone" else int(word, 0), default=[])
    parser.add_argument("-o", "--out", required=True)
    parser.add_argument("--json", action="store_true")
    parsed = parser.parse_args(args)
    if refused_as_busy():
        return EXIT_BUSY
    command = device_command("mic-record") + [str(parsed.ms), "--out", parsed.out]
    if parsed.settle is not None:
        command += ["--settle", str(parsed.settle)]
    for write in parsed.at:
        command += ["--at", *map(str, write)]
    if parsed.json:
        command.append("--json")
    return call_device(command)


CODEC_FAILURES = ("CODEC error", "CODEC usage")


def codec(args):
    """codec <reg> [value]: the firmware's one-line register reply, or its error, on a development build."""
    if not 1 <= len(args) <= 2:
        print("usage: autana codec <reg> [value]", file=sys.stderr)
        return 2
    code, replies = send("codec " + " ".join(args), reply="CODEC")
    if code or not replies:
        return code or 1
    if replies[-1].startswith(CODEC_FAILURES):
        print(replies[-1], file=sys.stderr)
        return 1
    print(replies[-1])
    return 0


def freeze(args):
    reject_unknown("freeze", args)
    if args:
        sys.exit("usage: autana freeze")
    code, replies = send("FREEZE", reply="FREEZE_STATE")
    print("\n".join(replies))
    return code


def resume(args):
    reject_unknown("resume", args)
    if args:
        sys.exit("usage: autana resume")
    code, replies = send("RESUME", reply="FREEZE_STATE")
    print("\n".join(replies))
    return code


def step(args):
    reject_unknown("step", args)
    if len(args) > 1:
        sys.exit("usage: autana step [N]")
    count = args[0] if args else ""
    if count and not (count.isdigit() and int(count) >= 1):
        sys.exit("usage: autana step [N] - N is a positive count")
    code, replies = send("STEP " + count if count else "STEP", reply="FREEZE_STATE")
    print("\n".join(replies))
    return code


def is_int(text):
    try:
        int(text)
        return True
    except ValueError:
        return False


def touch(args):
    reject_unknown("touch", args)
    if len(args) != 3 or args[0] not in ("down", "up") or not is_int(args[1]) or not is_int(args[2]):
        sys.exit("usage: autana touch <down|up> <x> <y>")
    code, replies = send("TOUCH " + " ".join(args), reply="TOUCH",
                         optional=True, seconds=0.5)
    print("\n".join(replies) if replies else "sent")
    return code


def imu(args):
    reject_unknown("imu", args)
    if args == ["release"]:
        code, replies = send("IMU release", reply="IMU", optional=True, seconds=0.5)
        print("\n".join(replies) if replies else "sent")
        return code
    if len(args) == 2 and args[0] == "quarter" and is_int(args[1]):
        code, replies = send("IMU " + " ".join(args), reply="IMU", until=["IMU_OK", "IMU_ERR"])
        print("\n".join(replies))
        return code
    if len(args) != 3 or not all(is_int(value) for value in args):
        sys.exit("usage: autana debug imu <ax> <ay> <az> | quarter <q> | release")
    code, replies = send("IMU " + " ".join(args), reply="IMU",
                         optional=True, seconds=0.5)
    print("\n".join(replies) if replies else "sent")
    return code


def gesture(args, verb, usage, points):
    """TAP, PRESS or DRAG, its first `points` x, y pairs read as pixels of the
    default `autana screenshot` and sent in the panel's own frame."""
    reject_unknown(verb, args)
    if not all(is_int(value) for value in args):
        sys.exit(usage)
    values = [int(value) for value in args]
    try:
        for i in range(0, 2 * points, 2):
            values[i:i + 2] = device_module().touch_point(values[i], values[i + 1])
    except ValueError as error:
        sys.exit(f"{usage} - {error}")
    line = verb.upper() + " " + " ".join(str(value) for value in values)
    code, replies = send(line, reply=verb.upper(), until=[verb.upper() + "_OK"])
    print("\n".join(replies))
    return code


def tap(args):
    if len(args) != 2:
        sys.exit("usage: autana tap <x> <y>")
    return gesture(args, "tap", "usage: autana tap <x> <y>", 1)


def press(args):
    if len(args) not in (2, 3):
        sys.exit("usage: autana press <x> <y> [ms]")
    return gesture(args, "press", "usage: autana press <x> <y> [ms]", 1)


def drag(args):
    if len(args) != 5:
        sys.exit("usage: autana drag <x0> <y0> <x1> <y1> <ms>")
    return gesture(args, "drag", "usage: autana drag <x0> <y0> <x1> <y1> <ms>", 2)


def button(args):
    reject_unknown("button", args)
    if len(args) not in (1, 2) or args[0] not in ("boot", "power") \
            or len(args) == 2 and args[1] not in ("short", "long"):
        sys.exit("usage: autana button <boot|power> [short|long]")
    code, replies = send("BUTTON " + " ".join(args), reply="BUTTON",
                         until=["BUTTON_OK", "BUTTON_ERR"])
    print("\n".join(replies))
    return code


def docs(args):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "docs"))
    import docs_search
    return docs_search.main(args, root=resolve_project())


def apps(args):
    reject_unknown("apps", args, ("--json",))
    json_output = read_json_flag(args, "usage: autana apps [--json]")
    code, replies = send("APPS", reply="APPS", until=["APPS_END"])
    if not json_output:
        print("\n".join(reply for reply in replies if reply.startswith("APPS ")))
    elif code == 0:
        try:
            print(json.dumps({"apps": parse_apps(replies)}))
        except ValueError as error:
            print(str(error), file=sys.stderr)
            return 1
    return code


def open_app(args):
    reject_unknown("open", args)
    if len(args) != 1:
        sys.exit("usage: autana open <name>")
    code, replies = send("OPEN " + args[0], reply="OPEN")
    print("\n".join(replies))
    return code


def home(args):
    reject_unknown("home", args)
    if args:
        sys.exit("usage: autana home")
    code, replies = send("HOME", reply="HOME")
    print("\n".join(replies))
    return code


def tunables():
    """[(name, value, low, high, default)] as the running device reports them."""
    code, replies = send("TUNE")
    if code != 0:
        sys.exit(code)
    return parse_tunables(replies)


def parse_tunables(replies):
    rows = []
    for reply in replies:
        if not reply.startswith("TUNE ") or "=" not in reply:
            continue
        fields = dict(field.split("=", 1) for field in reply[len("TUNE "):].split(" "))
        name = reply[len("TUNE "):].split("=", 1)[0]
        rows.append((name, fields[name], fields.get("min", "?"), fields.get("max", "?"),
                     fields.get("default", fields[name])))
    return rows


def tune_dict(row):
    name, value, low, high, default = row
    return {"name": name, "value": int(value), "min": int(low), "max": int(high),
            "default": int(default)}


def full_name(name):
    """`theme_rgb` for `ridge.theme_rgb`, when only one tunable ends that way."""
    if "." in name:
        return name
    matches = [row[0] for row in tunables() if row[0].split(".", 1)[-1] == name]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        sys.exit(f"autana: no tunable is named {name} - see: autana tune")
    sys.exit(f"autana: {name} could be any of: " + ", ".join(matches))


def exact_tune_matches(rows, name):
    """Rows whose name IS `name`, or whose name ends `.<name>` when that is
    unambiguous: `theme_rgb` for `ridge.theme_rgb`, the same drop-the-owner shorthand
    `full_name()` resolves for a set/reset."""
    if "." in name:
        return [row for row in rows if row[0] == name]
    return [row for row in rows if row[0].split(".", 1)[-1] == name]


def format_tune_rows(rows, filter_text=""):
    if not rows:
        print("no tunables" + (f" containing '{filter_text}'" if filter_text else ""))
        return
    width = max(len(row[0]) for row in rows)
    for name, value, low, high, default in rows:
        changed = int(value) != int(default)
        if name.endswith("_rgb"):
            # a colour reads as one; SET takes 0x38D6E8 back
            value, low, high, default = (f"0x{int(number):06X}" for number in (value, low, high, default))
        print(f"{name:<{width}}  {value:>8}   ({low}..{high})" + (f"   * source has {default}" if changed else ""))


def tune_set(name, value):
    code, replies = send("SET " + full_name(name) + " " + value)
    print("\n".join(reply.split(" ", 1)[1] for reply in replies))
    return code


def tune_reset(name):
    code, replies = send("RESET " + full_name(name))
    print("\n".join(reply.split(" ", 1)[1] for reply in replies))
    return code


def tune(args):
    """`tune` alone or with filter text lists; a name that is exactly one
    tunable's own (owner optional when unambiguous) shows that one in the
    same listing; a name and a value sets it; `reset` is recognised only in
    first position, so a tunable actually named that would still be
    reachable through the filtered listing. Writing values into the source
    is launcher/tools/tune/apply.py's, fed `tune --json`."""
    json_output = "--json" in args
    args = [arg for arg in args if arg != "--json"]
    reject_unknown("tune", args)
    if json_output and (len(args) > 1 or args and args[0] == "reset"):
        sys.exit("usage: autana tune [text] [--json]")
    if args and args[0] == "reset":
        if len(args) != 2:
            sys.exit("usage: autana tune reset <name>")
        return tune_reset(args[1])
    if len(args) > 2:
        sys.exit("usage: autana tune [text] | autana tune <name> [value] | autana tune reset <name>")
    if len(args) == 2:
        return tune_set(args[0], args[1])
    rows = tunables()
    text = args[0] if args else ""
    shown = rows
    if text:
        exact = exact_tune_matches(rows, text)
        shown = exact if len(exact) == 1 else [row for row in rows if text in row[0]]
    if json_output:
        print(json.dumps({"tunables": [tune_dict(row) for row in shown]}))
    else:
        format_tune_rows(shown, text)
    return 0


PERF_USAGE = "usage: autana perf [?] | autana perf off | autana perf <name> [event] [seconds]"
PERF_DEFAULT_SECONDS = 10.0


def perf_text(reply):
    """A board reply without its `PERFMON_` prefix, the way `tune` drops its own."""
    return reply.removeprefix("PERFMON_")


def perf_listing(replies):
    """The `PERFMON_NAME`/`PERFMON_EVENT` lines as two titled lists, and the
    names the board had no room to remember."""
    def words(prefix):
        return [reply.split(" ", 1)[1] for reply in replies if reply.startswith(prefix + " ")]

    lines = ["names seen so far (a name appears once its bracket has run):"]
    lines += [f"  {name}" for name in words("PERFMON_NAME")] or ["  none yet"]
    for dropped in words("PERFMON_NAMES_DROPPED"):
        lines.append(f"  ({dropped} more did not fit the board's name table)")
    lines.append("events (the first, insn, is the default):")
    lines += [f"  {event}" for event in words("PERFMON_EVENT")]
    return "\n".join(lines)


def perf_segments(text, name):
    """(event, avg, min, max, event avg, n) for every `perf:` line of `text`
    that belongs to `name`."""
    found = []
    for match in PERF_SEGMENT.finditer(text):
        if match.group(1) == name:
            found.append((match.group(5),) + tuple(int(group) for group in
                                                  (match.group(2), match.group(3), match.group(4),
                                                   match.group(6), match.group(7))))
    return found


def perf_summary(name, segments):
    """One line over every report window: the averages weighted by n, the
    extremes across windows."""
    event = segments[0][0]
    samples = sum(n for *_, n in segments)
    cycles = sum(avg * n for _, avg, _, _, _, n in segments) / samples
    counted = sum(avg * n for _, _, _, _, avg, n in segments) / samples
    summary = (f"{name}: cycles avg {cycles:.0f} min {min(s[2] for s in segments)} "
               f"max {max(s[3] for s in segments)} | {event} avg {counted:.0f} | n={samples}")
    if counted:
        summary += f" | {cycles / counted:.2f} cycles per {event}"
    return summary


def perf_parse(args):
    """(name, event or None, seconds), positional like the usage line."""
    if len(args) > 3:
        sys.exit(PERF_USAGE)
    seconds = PERF_DEFAULT_SECONDS
    if len(args) == 3:
        try:
            seconds = float(args[2])
        except ValueError:
            sys.exit(PERF_USAGE)
        if seconds <= 0:
            sys.exit(PERF_USAGE)
    return args[0], args[1] if len(args) > 1 else None, seconds


def perf_collect(seconds):
    """The console for `seconds`, as text, from device.py's own listen."""
    with tempfile.TemporaryDirectory() as folder:
        capture = Path(folder) / "perf.log"
        result = capture_device(device_command("listen", "--seconds", str(seconds), "--out", str(capture)),
                                text=True)
        if result.returncode != 0:
            print((result.stderr or result.stdout).strip(), file=sys.stderr)
            return result.returncode, ""
        return 0, capture.read_text(encoding="utf-8", errors="replace")


def perf_disarm():
    """True when the board confirmed `off`; says so on stderr when it did not."""
    code, replies = send("PERF off", reply="PERFMON")
    if code == 0 and replies and replies[-1].startswith("PERFMON_OK"):
        return True
    print("autana perf: the counters may still be armed - " + (perf_text(replies[-1]) if replies else "no reply"),
          file=sys.stderr)
    return False


def perf(args):
    """The S3's cycle counter and one event over one frame_cost bracket: bare
    or `?` lists, `off` disarms, `<name>` arms, listens, disarms and
    summarises. The board's own PERF verb is the arm; the numbers come from
    its `perf:` line."""
    reject_unknown("perf", args)
    if not args or args == ["?"]:
        code, replies = send("PERF ?", reply="PERFMON", until=["PERFMON_END", "PERFMON_ERR"])
        if code != 0 or not replies:
            return code or 1
        print(perf_listing(replies))
        return 0
    if args == ["off"]:
        if not perf_disarm():
            return 1
        print("counters off")
        return 0
    name, event, seconds = perf_parse(args)
    code, replies = send(f"PERF {name} {event}" if event else f"PERF {name}", reply="PERFMON")
    if replies:
        print(perf_text(replies[-1]))
    if code != 0 or not replies or not replies[-1].startswith("PERFMON_OK"):
        return code or 1
    disarmed = False
    try:
        code, text = perf_collect(seconds)
    finally:
        disarmed = perf_disarm()
    if code != 0:
        return code
    segments = perf_segments(text, name)
    if not segments:
        print(f"autana perf: no samples of {name} in {seconds:g} s - is it running (open its app)?",
              file=sys.stderr)
        return 1
    print(perf_summary(name, segments))
    return 0 if disarmed else 1


def forward(line, verb):
    """A line no autana command recognises, sent to the device as typed;
    the maintainer's rule that every board operation goes through autana,
    for an app's own command (docs/cli/App-Commands.md)
    same as any built-in one. The reply prefix is the line's
    own first word in capitals: an app's reply always starts with its own
    prefix, so this returns as soon as `<PREFIX>_END`/`<PREFIX>_ERR`
    arrives rather than waiting out send()'s own window; a raw verb with
    no dedicated autana command of its own (runsuite, today) falls back to
    that window, since nothing then completes early."""
    if verb.startswith("-"):
        sys.exit(f"autana: unknown option {verb.split('=', 1)[0]}; 'autana help' lists the commands")
    reply, endings = reply_prefix(line)
    with command_named(f"console {verb}"):
        code, replies = send(line, reply=reply, until=list(endings), optional=True)
    if code:
        print("\n".join(replies))
        raise SystemExit(code)
    return replies


def console(_args=None):
    """A session with the device: each line takes the lock, asks, and lets go,
    so the board is free for anything else between two of them. A bare word
    is an autana command, or the whole line is sent to the device as typed
    (forward()); `tune <name>` is the only way to a tunable, never an
    implicit lookup of a bare name."""
    install_completion()
    print("autana console - 'help' for the commands, 'quit' to leave")
    while True:
        try:
            pinned = session_flags.get("--board")
            prompt = f"autana@{device_module().short_id(pinned)}> " if pinned else "autana> "
            line = input(prompt).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not line:
            continue
        try:
            flags, words = pop_globals(shlex.split(line))
        except SystemExit as stop:
            print(stop.code)
            continue
        if not words:
            continue
        verb, rest = words[0], words[1:]
        if verb in ("quit", "exit", "q"):
            return 0
        try:
            if verb == "use":
                if not rest:
                    print(session_flags.get("--board", "no board pinned"))
                elif rest == ["-"]:
                    session_flags.pop("--board", None)
                elif len(rest) == 1:
                    session_flags["--board"] = checked_global("--board", rest[0])
                else:
                    print("usage: use [<board>|-]")
            elif verb == "help":
                print(help_text(rest, prefix=""))
            elif verb in COMMANDS and verb != "console":
                run_command(COMMANDS[verb], rest, command_words([verb, *rest]), flags)
            else:
                with applied({**session_flags, **flags}):
                    replies = forward(" ".join(words), verb)
                print("\n".join(replies) if replies else "sent")
        except SystemExit as stop:
            # a command's own refusal ends that command, not the session
            if stop.code not in (0, None):
                print(stop.code)


Command = namedtuple("Command", "name handler usages")


def one(name, handler, usage, what):
    return Command(name, handler, ((usage, what),))


def alias(old, new, handler):
    """`old` still works: one migration line to stderr, then straight into
    `handler`, so a script built on a spelling `new` replaced does not
    break on merge."""
    def wrapped(args):
        print(f"autana {old}: use `autana {new}` - this spelling still works", file=sys.stderr)
        return handler(args)
    return wrapped


LOCK_VERBS = {"wait": lock_wait, "take": take, "run": lock_run, "release": release, "hand": hand, "take-back": take_back}
DEBUG_VERBS = {"freeze": freeze, "resume": resume, "step": step, "touch": touch, "imu": imu}
DEBUG_ALIASES = {"framewatch": alias("debug framewatch", "framewatch", framewatch)}


def lock(args):
    """`autana lock <verb>`: wait/take/run/release/hand/take-back, the lock-sharing
    commands a session reaches for far less than `status`, which stays
    top-level on its own."""
    if not args or args[0] not in LOCK_VERBS:
        sys.exit("usage: autana lock <" + "|".join(LOCK_VERBS) + "> ...")
    verb, rest = args[0], args[1:]
    return LOCK_VERBS[verb](rest)


def debug(args):
    """Frame-loop intervention and raw sensor overrides, hidden from bare help."""
    if not args or args[0] not in DEBUG_VERBS | DEBUG_ALIASES:
        sys.exit("usage: autana debug <" + "|".join(DEBUG_VERBS) + "> ...")
    verb, rest = args[0], args[1:]
    return (DEBUG_VERBS | DEBUG_ALIASES)[verb](rest)


LOCK_USAGES = (
    ("lock run --heavy -- <command...>", "hold the host-wide heavy key; nested builds reuse its token; acquire a board before heavy when both are needed"),
    ("lock run [--keep-on-failure <seconds>] [--estimate <seconds>] -- <command...>", "hold the board while a command runs"),
    ("lock take [--for <seconds> | --until-released [--pid <pid>]] [--estimate <seconds>] <note...>", "wait for the board and print a token; a timed hold lasts at most an hour"),
    ("lock wait", "wait until the board is free without taking it; --wait caps the wait"),
    ("lock release [<token>]", "release a hold early; the token is optional inside a hold"),
    ("lock hand [--until-back <seconds>] <note...>",
     "reserve the board for a person for an hour; run again to renew"),
    ("lock take-back", "clear that reservation"),
)


# Written into docs/cli/ by cli_reference.py, one page per group.
COMMAND_GROUPS = (
    ("lock", "Sharing the board", (
        Command("devices", devices, (("devices [--json]", "short ids, serials, ports and availability; the same JSON as status --json"),)),
        Command("status", status, (("status [--json]", "every board and the heavy key: holder, command, since when, estimated free time and queue"),)),
        Command("lock", lock, LOCK_USAGES),
    )),
    ("build", "Build and flash", (
        Command("build", build, (
            ("build [rel|dev|diag] [--perf-scope [--frame-laps]] [--layout-seed N] [--hot-tunables] [--configure]",
             "build without a board lock; dev when omitted; --layout-seed N > 0 pads the layout, 0 is the plain build; --frame-laps adds the laps inside the frame stages; --hot-tunables adds live hot-path knobs; --configure only writes sdkconfig and compile_commands.json"),
            ("build diag --check", "the diagnostics build plus the complexity ratchet, no board"))),
        one("flash", flash, "flash [rel|dev|diag] [--quiet] [--perf-scope [--frame-laps]] [--layout-seed N] [--hot-tunables]",
            "build, then flash under the lock; dev when omitted; --quiet logs only; --perf-scope (diag) builds the perf-scoped image, --frame-laps adds the laps inside its frame stages"),
        one("buildid", buildid, "buildid [--json]", "the BUILD_ID the board runs, to check against what was flashed"),
    )),
    ("tests", "Tests", (
        Command("suite", suite, (
            ("suite <name>... [seconds] [--runs N] [--flash] [--layout-seed N] [--hot-tunables] [--verbose]",
             "run suites under one lock, N times each (1 when omitted); seconds caps each capture "
             "(1800 when omitted); --flash builds and flashes the diagnostics image first, so nobody flashes between captures"),
            ("suite <name> --test PATTERN[,PATTERN]",
             "only the tests whose name contains a pattern (case-sensitive); --test repeats"),
            ("suite <name> --send LINE",
             "send a console line after tunables, before every capture; --send repeats in order; the sequence should be idempotent, and an error or incomplete reply stops the batch"),
            ("suite <name> --set NAME=VALUE",
             "set a tunable before each capture; --set repeats"),
            ("suite list [text] [--json]", "the registered suites; [on request] ones run only by name"))),
        one("selftest", selftest, "selftest [seconds] [--verbose]",
            "build the autorun diagnostics image, flash, run every suite; 3000 s when omitted"),
        one("melody", melody, "melody [volume] [--check] [--internal] [--json]",
            "development builds: play the phrase, or check every note with --check; --internal selects silent codec feedback"),
        one("tone", tone, "tone <hz> <ms> [volume] [--internal] [--json]",
            "development builds: play <hz> for <ms> at volume % (firmware default 30) and report the level against a silence baseline, whether it was heard and the measured frequency; --internal uses silent codec feedback"),
    )),
    ("watch", "Watch the board", (
        one("monitor", monitor, "monitor [seconds] [--follow] [--stream] [--elf PATH]",
            "in a terminal the console live, until Ctrl+C or for seconds; piped it needs seconds or --follow and prints only errors and FRAME_WATCH warnings unless --stream; exits 1 on a crash or reboot"),
        Command("coredump", coredump, (
            ("coredump [--elf PATH]", "the last panic's core dump, saved to the records and decoded against the build that crashed; resets the board"),
            ("coredump --core FILE [--elf PATH]", "decode a saved dump; no board"),
            ("coredump --erase", "clear the dump so the next crash's is told apart; resets the board"),)),
        one("mic", mic, "mic record <ms> [--settle ms] [--at offset reg value | --at offset tone hz duration_ms gain]... -o take.wav [--json]",
            "record microphone PCM and codec state; --at offsets are ms from take start; negative offsets fall in settling"),
        one("codec", codec, "codec <reg> [value]",
            "development builds: read, or write then read back, one codec register (hex or decimal)"),
        Command("screenshot", screenshot, (
            ("screenshot [--as-shown|--framebuffer] [-o PATH]", "PATH.png plus a PATH.json state snapshot; landscape by default"),
            ("screenshot --frames N -o PATH", "N frames as PATH-00 on: the loop is frozen and stepped a frame between captures, then resumed"),)),
        Command("battery", battery, (
            ("battery [--json]", "fresh voltage, percentage, USB, charge phase and flow, plus raw PMU status bytes; development builds"),
            ("battery --log [--json]", "persistent battery records oldest first (JSON is an array); reasons: periodic, transition, boot, shutdown-warning; development builds"),
            ("battery --log --clear", "erase the battery history namespace"))),
        one("heap", heap, "heap [--json]", "free bytes and largest block in the internal DMA pool (MEMORY_DMA); development builds"),
        Command("perf", perf, (
            ("perf", "the frame_cost names seen and the counter events"),
            ("perf <name> [event] [seconds]",
             "count one event (insn when omitted) on one frame_cost bracket for seconds (10 when omitted); prints cycles avg/min/max, n and cycles per event"),
            ("perf off", "disarm the counters"))),
        one("framewatch", framewatch, "framewatch", "the frame watch as JSON: allocations, frees and log lines repeating frame after frame"),
    )),
    ("drive", "Drive", (
        one("play", play, "play <pack>/<name> [loop] [gain]", "play a pack sound; alias for sound play"),
        Command("sound", sound, (
            ("sound play <pack>/<name> [loop] [gain]",
             "play a sound from a pack; a bare name means the engine pack; gain is a percent, full gain when omitted; development builds"),
            ("sound stop [voice|all]", "stop a voice or all voices; all when omitted"),
            ("sound stats", "voice states, dropped commands, amplifier state and RAM sizes"),
            ("sound onset <event> [hz] [gain]", "measure amplifier, start, stop and steal transients"))),
        one("tap", tap, "tap <x> <y>", "tap, 50 ms, at a point in pixels of the default screenshot"),
        one("press", press, "press <x> <y> [ms]", "hold a point, as tap reads it; 1000 ms when omitted"),
        one("drag", drag, "drag <x0> <y0> <x1> <y1> <ms>", "drag between two points, as tap reads them"),
        Command("button", button, (
            ("button <boot|power> [short]", "press a board button; short when omitted"),
            ("button <boot|power> long", "hold it"))),
        one("reset", reset, "reset [--capture [seconds]] [--verbose]",
            "reboot and wait for USB serial; --capture records the boot (20 s when omitted)"),
        Command("apps", apps, (
            ("apps", "the registered apps, and which is running"),
            ("apps --json", "the same, as JSON"))),
        one("open", open_app, "open <name>", "enter an app; a case-insensitive, unambiguous prefix"),
        one("home", home, "home", "back to the launcher"),
    )),
    ("tune", "Tune", (
        Command("tune", tune, (
            ("tune [text] [--json]", "list the tunables with ranges; names containing text"),
            ("tune <name> [value]", "show one, or set it on the board (lost on reboot)"),
            ("tune reset <name>", "back to the value the source declares"))),
    )),
    ("debug", "Debug", (
        Command("debug", debug, (
            ("debug freeze", "stop the frame loop where it is"),
            ("debug resume", "run it again"),
            ("debug step [N]", "advance N frames while frozen; 1 when omitted"),
            ("debug touch <down|up> <x> <y>", "one raw touch level at a panel-frame point (a --framebuffer pixel); up hands back to the controller"),
            ("debug imu <ax> <ay> <az>", "raw accelerometer counts; imu quarter <q> turns the screen to display quarter q, imu release hands back to the sensor"))),
    )),
    ("docs", "Docs", (
        Command("docs", docs, (
            ("docs <question...>", "the three best sections, excerpted with their path:line, then five more to read on"),
            ("docs --section <path:line>", "one section whole, or path#heading words; --deep adds subsections"),
            ("docs --outline <path>", "a document's headings, with lines and sizes"),
            ("docs --ask <question...>", "a short answer from the local chat model"))),
    )),
)

# Left out of the bare `autana help` listing; `autana help <key>` still shows
# a hidden group in full, same as any other topic.
HIDDEN_GROUPS = {"debug"}

# Old spellings kept working, see alias()'s own docstring. "batch" moved
# to suite() itself (its translation is not a plain forward) so it is not
# here; the remaining aliases forward directly to their handler.
RENAMED_VERBS = {
    "freeze": ("debug freeze", freeze), "resume": ("debug resume", resume),
    "step": ("debug step", step), "touch": ("debug touch", touch), "imu": ("debug imu", imu),
}

COMMANDS = {command.name: command.handler
            for _, _, commands in COMMAND_GROUPS for command in commands}
COMMANDS["console"] = console
COMMANDS["batch"] = batch
COMMANDS.update({old: alias(old, new, handler) for old, (new, handler) in RENAMED_VERBS.items()})
HELP_TOPICS = [key for key, _, _ in COMMAND_GROUPS] + [
    command.name for _, _, commands in COMMAND_GROUPS for command in commands] + ["flags", "config"]
USAGE_WIDTH = 34

# (flag, what it does, the commands that take it), left out of every command's
# own usage line (`autana help flags` instead) since most commands take most
# of these and repeating them on every line was unreadable.
BOARD_FLAGS = (
    ("--lock-token TOKEN", "run inside this board's live hold", "every board command"),
    ("--wait SECONDS", "how long to wait for the board's lock; 0 fails at once (exit 75)",
     "every board command"),
    ("--owner NAME", "the lock owner's label",
     "every board command"),
    ("--board SERIAL", "USB serial or short id; -b is an alias",
     "every board command"),
    ("--out PATH", "write the one capture here instead of the default path",
     "selftest, suite, monitor"),
    ("--expect-build-id ID", "refuse to run a suite unless the board, or the image `--flash` "
                             "just wrote, carries this BUILD_ID",
     "suite"),
    ("--project PATH", "act on PATH instead of the current directory - it must itself carry "
                       "launcher/CMakeLists.txt, no searching parent directories",
     "build, flash, selftest, suite, docs"),
)


def board_flags_text(prefix=""):
    width = max(len(flag) for flag, _, _ in BOARD_FLAGS)
    lines = ["--wait, --owner, --board (-b) and --project are global: they go anywhere on the "
             "line, and docs/cli/Settings.md has their defaults.",
             "",
             "Flags (on top of each command's own usage above)"]
    for flag, summary, commands in BOARD_FLAGS:
        lines.append(f"  {flag:<{width}}  {summary}")
        lines.append(f"  {'':<{width}}  on: {commands}")
    if not prefix:
        lines.append("")
    return "\n".join(lines)


# The five things a first run actually needs, each a real example rather than
# a placeholder; `autana help` led three newcomer reads past this before
# they found any of them among 37 commands in 9 groups.
QUICKSTART = (
    ("flash dev", "build and flash; the everyday form"),
    ("monitor 30", "the console for 30 s"),
    ("suite list", "then `suite <name>` to run one"),
    ("tune ridge_trail", "a live value, read or set"),
    ("screenshot -o shot", "the panel as shot.png plus shot.json"),
)


def quickstart_text(prefix):
    width = max(len(prefix + synopsis) for synopsis, _ in QUICKSTART)
    lines = ["Most used"]
    for synopsis, summary in QUICKSTART:
        lines.append(f"  {prefix + synopsis:<{width}}  {summary}")
    return "\n".join(lines)


def help_text(args, prefix="autana "):
    """Every group, or the one group or command `args` names, or the shared
    board flags for `args == ["flags"]`. With no args, leads with the
    five-line quickstart and leaves out a hidden group (HIDDEN_GROUPS);
    `autana help <that group>` still shows it in full."""
    topic = args[0] if args else None
    if topic == "flags":
        return board_flags_text(prefix).rstrip()
    if topic == "config":
        return autana_config.help_text()
    lines = [quickstart_text(prefix), ""] if topic is None else []
    for key, title, commands in COMMAND_GROUPS:
        if topic is None and key in HIDDEN_GROUPS:
            continue
        if topic not in (None, key):
            commands = [command for command in commands if command.name == topic]
            if not commands:
                continue
        lines.append(f"{title} ({key})")
        for command in commands:
            for synopsis, summary in command.usages:
                usage = prefix + synopsis
                if len(usage) > USAGE_WIDTH:
                    lines.append(f"  {usage}")
                    usage = ""
                lines.append(f"  {usage:{USAGE_WIDTH}}  {summary}")
        lines.append("")
    if not lines:
        return f"no command or group '{topic}'; groups: " + ", ".join(
            key for key, _, _ in COMMAND_GROUPS)
    if topic is None:
        if not prefix:
            lines.append("use [<board>|-]  show, pin or clear the session board")
        lines.append(f"{prefix}--wait SECONDS <command>  how long to wait for the board; 0 fails "
                     "at once")
        lines.append(f"{prefix}--owner NAME <command>    label this run in the lock")
        lines.append(f"{prefix}--board SERIAL <command>  which board, when several are plugged in")
        lines.append(f"{prefix}help flags    --wait/--owner/--board/--out/--expect-build-id/--project, which "
                     "commands take them, and how the lock owner and its wait are set")
        lines.append(f"{prefix}help config   the settings a project's autana.local.toml can hold")
        lines.append(f"{prefix}help [topic]  one group or command"
                     + ("" if prefix else "; quit leaves the session"))
    return "\n".join(lines).rstrip()


def completion_candidates(line, prefix):
    """Return command or static argument matches for the current word."""
    if not any(character.isspace() for character in line):
        words = (set(COMMANDS) - {"console"}) | {"help", "quit", "use"}
    else:
        parts = line.split()
        if line and line[-1].isspace():
            parts.append("")
        if len(parts) != 2 or parts[0] not in ("build", "flash", "help"):
            return []
        words = VARIANTS if parts[0] in ("build", "flash") else HELP_TOPICS
    return sorted(word for word in words if word.startswith(prefix))


def completion_readline():
    """Load a terminal completion module when the interpreter provides one."""
    try:
        return importlib.import_module("readline")
    except ImportError:
        try:
            return importlib.import_module("pyreadline3")
        except ImportError:
            return None


def install_completion():
    readline = completion_readline()
    if readline is None:
        return
    matches = []

    def complete(prefix, state):
        if state == 0:
            matches[:] = completion_candidates(readline.get_line_buffer(), prefix)
        return matches[state] if state < len(matches) else None

    readline.set_completer(complete)
    readline.set_completer_delims(" \t")
    readline.parse_and_bind("tab: complete")


GLOBALS = ("--wait", "--owner", "--board", "-b", "--project", "--lock-token")
_GLOBAL_ENV = {"--wait": WAIT_ENV, "--owner": OWNER_ENV, "--board": BOARD_ENV,
               "--project": PROJECT_ENV, "--lock-token": TOKEN_ENV}

# Flags typed before a bare `autana`: they hold for the whole console session.
session_flags = {}


def project_of(value):
    """`value` resolved, when it is an autana project."""
    project = Path(value).resolve() if value.strip() else None
    return str(project) if project and (project / PROJECT_MARKER).is_file() else None


# One validator per flag: what it needs, an example, and a check returning
# the value to store or None. Device subprocesses resolve board selectors;
# an owner is a label the lock appends `:pid` to and prints,
# so it holds no whitespace.
GLOBAL_CHECKS = {
    "--lock-token": ("a non-empty token without whitespace", "autana --lock-token TOKEN screenshot",
                     lambda v: v if v and not re.search(r"\s", v) else None),
    "--wait": ("a non-negative integer number of seconds", "autana --wait 0 monitor 5",
               lambda v: v if v.isascii() and v.isdigit() else None),
    "--owner": ("a name without whitespace", "autana --owner ci-7 flash",
                lambda v: v if v and not re.search(r"\s", v) else None),
    "--board": ("a board's USB serial number or four-digit short id",
                "autana --board 90:70:69:FE:A3:08 monitor 5",
                lambda v: device_module().board_selector(v)),
    "--project": ("a path to an autana project", "autana flash --project PATH", project_of),
}


def checked_global(flag, value):
    """`value` as `flag`'s environment value, or exit naming the flag, the
    value and an example."""
    needs, example, check = GLOBAL_CHECKS[flag]
    checked = check(value)
    if checked is None:
        where = f" (no {PROJECT_MARKER.as_posix()} in {Path(value).resolve()})" if flag == "--project" else ""
        sys.exit(f"autana {flag} {value!r}: {flag} needs {needs}, e.g. {example}{where}")
    return checked


def pop_globals(words):
    """(flags, rest): every GLOBALS flag in `words`, as `--flag V` or
    `--flag=V` wherever it stands, validated, and the words left over. The
    one parser, so the order of the flags never matters."""
    flags, rest = {}, []
    queue = list(words)
    while queue:
        word = queue.pop(0)
        if word == "--":
            rest.extend([word, *queue])
            break
        flag, equals, value = word.partition("=")
        if flag == "-b":
            flag = "--board"
        if flag not in GLOBALS:
            rest.append(word)
            continue
        if not equals:
            value = queue.pop(0) if queue else ""
        flags[flag] = checked_global(flag, value)
    return flags, rest


@contextlib.contextmanager
def applied(flags):
    """`flags` written into the environment device_command(), owner() and
    every child process read, restored on the way out."""
    previous = {_GLOBAL_ENV[flag]: os.environ.get(_GLOBAL_ENV[flag]) for flag in flags}
    for flag, value in flags.items():
        os.environ[_GLOBAL_ENV[flag]] = value
    try:
        yield
    finally:
        for name, old in previous.items():
            if old is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = old


def main():
    install_console_interrupt_handler()
    flags, argv = pop_globals(sys.argv[1:])
    if not argv or argv[0] == "console":
        session_flags.update(flags)
        sys.exit(console())
    if argv[0] in ("--version", "-V"):
        print(__version__)
        sys.exit(0)
    if argv[0] in ("help", "--help", "-h"):
        print(help_text(argv[1:]))
        sys.exit(0)
    if argv[0] not in COMMANDS:
        with applied(flags):
            replies = forward(" ".join(argv), argv[0])
        print("\n".join(replies) if replies else "sent")
        sys.exit(0)
    sys.exit(run_command(COMMANDS[argv[0]], argv[1:], command_words(argv), flags))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(EXIT_INTERRUPTED)
