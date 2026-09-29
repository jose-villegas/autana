#!/usr/bin/env python3
"""autana - terminal commands for the autana engine repo.

    autana                  a console session with the device
    autana help [topic]     the commands, grouped; a topic is a group or a command

A system-installed tool, not tied to git or worktrees: run from any folder,
board-only commands (monitor, tap, tune, ...) work from anywhere, and a
command that needs a project - build, flash, selftest, suite --flash - acts
on the current directory, like `make -C`, or on --project PATH. Anything
that touches the board goes through scripts/device/device.py, which takes
the device lock. The command list is COMMAND_GROUPS at the end of this
file; docs/tools/Autana-CLI.md mirrors it.
"""

import getpass
import gzip
import importlib
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import threading
import time
from collections import namedtuple
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher" / "tools" / "build"))
from espressif import idf_python  # noqa: E402  (path must be set up first)
from version import __version__  # noqa: E402

VARIANTS = {"rel": "release", "release": "release", "dev": "dev", "diag": "diag"}

# What a directory needs to be an autana project - launcher/CMakeLists.txt,
# not a .git folder: this tool is not tied to git, and a project built from
# a tarball or a non-git checkout is still one.
PROJECT_MARKER = Path("launcher") / "CMakeLists.txt"


def owner():
    """What the device lock calls this autana: AUTANA_DEVICE_OWNER, with
    this process's own pid still appended, when set - two shells that
    export the same override would otherwise see each other's lock as
    their own (board_holder()'s self-check compares by name) - else
    "<user>@<host>:<pid>". getpass.getuser() can fail with no username in
    the environment (a container); "user" then stands in for it."""
    override = os.environ.get("AUTANA_DEVICE_OWNER")
    if override:
        return f"{override}:{os.getpid()}"
    try:
        user = getpass.getuser()
    except OSError:
        user = "user"
    return f"{user}@{socket.gethostname()}:{os.getpid()}"


def git(*args):
    """A git answer, or "" - for the flash/build banner's branch and commit
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


# This invocation's own --project, popped once by run_command() ahead of
# any command's own parsing (main()'s one-shot dispatch, and each line of a
# console session) - so every command that reads it, not just the ones that
# build or flash, shares one flag instead of parsing it for itself. Reset
# around each dispatched call, so a console session's next line starts over.
_project_arg = None


def project_override():
    """This invocation's own --project, unresolved and unvalidated - for a
    command that only wants a path for metadata (`suite` without --flash)
    and has no reason to require PROJECT_MARKER there. A command that
    actually reads the project wants resolve_project() instead."""
    return _project_arg


def resolve_project():
    """This invocation's own --project, or its cwd when none was given -
    like `make -C`/`idf.py -C`, never a search of parent directories.
    Either way the directory itself must carry PROJECT_MARKER; failing that
    names --project, never git."""
    project = Path(_project_arg).resolve() if _project_arg is not None else Path.cwd()
    if not (project / PROJECT_MARKER).is_file():
        sys.exit(f"autana: {project} is not an autana project "
                 f"(no {PROJECT_MARKER.as_posix()}) - pass --project PATH")
    return str(project)


def run_command(handler, args):
    """Pop `--project` once, ahead of `handler`'s own parsing, and let
    resolve_project()/project_override() read it for the length of this
    call. The one place main() and console() both dispatch through."""
    global _project_arg
    value, args = pop_value(args, "--project")
    previous, _project_arg = _project_arg, value
    try:
        return handler(args)
    finally:
        _project_arg = previous


def pop_value(args, flag):
    """`args` with `flag` and the value right after it removed, and that
    value - (None, args unchanged) when `flag` is absent."""
    if flag not in args:
        return None, args
    rest = list(args)
    index = rest.index(flag)
    if index + 1 >= len(rest):
        sys.exit(f"usage: {flag} needs a value")
    value = rest.pop(index + 1)
    rest.pop(index)
    return value, rest


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


def device_command(*args, wait=None):
    """device.py imports pyserial, so it runs under ESP-IDF's Python even
    when some other interpreter started this file. `--owner`/`--wait` are
    device.py's own top-level flags, filled in one place for every board
    command: owner() names the lock holder, and `--wait` is a caller's own
    `wait` when it gives one (send()'s short fail-fast window overrides
    everything else), else AUTANA_DEVICE_WAIT, else left to device.py's own
    600 s default."""
    command = [idf_python(), "-u", str(device_tool()), "--owner", owner()]
    wait = wait if wait is not None else os.environ.get("AUTANA_DEVICE_WAIT")
    if wait:
        command += ["--wait", str(wait)]
    return command + list(args)


def reject_unknown(cmd, args, allowed=()):
    """Any leftover `--flag` a command's own parsing did not recognise -
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
    process = subprocess.Popen(command, stdout=subprocess.PIPE, text=True, bufsize=1)
    for line in process.stdout:
        print(line, end="", flush=True)
        if follower is None and line.startswith("flash log: "):
            follower = threading.Thread(target=follow, args=(line[len("flash log: "):].strip(), finished))
            follower.start()
    code = process.wait()
    finished.set()
    if follower is not None:
        follower.join()
    return code


def variant_request(verb, args, flags, project):
    """The words `autana build` and `autana flash` share: one variant (dev
    when omitted) and the `flags` given, against `project` - already
    resolved by the caller. Prints the banner - branch/commit/dirty only
    when git answers for that project, never required - and returns the
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
        dirty = " (dirty)" if git("-C", project, "status", "--porcelain") else ""
        where = f"{current_branch} @ {commit}{dirty}"
    print(f"autana {verb}: {variant} of {where}", flush=True)
    return asked, variant, seen


def flash(args):
    project = resolve_project()
    asked, variant, seen = variant_request("flash", args, ("--quiet", "--perf-scope"), project)
    quiet = "--quiet" in seen
    perf_scope = "--perf-scope" in seen
    command = device_command(
        "flash", "--variant", variant, "--worktree", project, "--purpose", f"autana flash {asked}",
    )
    if perf_scope:
        command.append("--perf-scope")
    return subprocess.call(command) if quiet else run_streaming_its_log(command)


def build_diag_check(project):
    """The diagnostics build plus the complexity ratchet, unchanged from
    launcher/tools/build/build_diag_check.sh - the two halves of what CI's
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
    _, variant, seen = variant_request("build", args, ("--perf-scope",), project)
    return device_module().build_worktree(project, variant, sorted(seen))


def seconds_argument(args, default, usage):
    if len(args) > 1:
        sys.exit(usage)
    if not args:
        return default
    try:
        return float(args[0])
    except ValueError:
        sys.exit(usage)


def identify(args):
    """What the device lock calls this autana, and the process that is it."""
    reject_unknown("id", args, ("--json",))
    json_output = read_json_flag(args, "usage: autana id [--json]")
    if json_output:
        print(json.dumps({"owner": owner(), "pid": os.getpid()}))
        return 0
    if args:
        sys.exit("usage: autana id")
    print(f"{owner()}   pid {os.getpid()}")
    return 0


def buildid(args):
    """What the BOARD says it is running, asked of it rather than read out of
    a build directory: the point of the question is whether the two agree."""
    reject_unknown("buildid", args, ("--json",))
    json_output = read_json_flag(args, "usage: autana buildid [--json]")
    code, replies = send("BUILDID", reply="BUILD_ID", purpose="autana buildid")
    if code != 0 or not replies:
        return code or 1
    if json_output:
        print(json.dumps(parse_buildid(replies[-1])))
    else:
        print(replies[-1])
    return 0


def framewatch(args):
    """The frame watch's counts for the last frame and the sites repeating
    now, as the board's own JSON - a development build only."""
    reject_unknown("framewatch", args)
    if args:
        sys.exit("usage: autana framewatch")
    code, replies = send("FRAMEWATCH", reply="FRAMEWATCH", purpose="autana framewatch")
    if code != 0 or not replies:
        return code or 1
    print(parse_framewatch(replies[-1]))
    return 0


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
    command = device_command("listen", "--purpose", "autana monitor")
    command += ["--follow"] if follow else ["--seconds", str(seconds)]
    if terminal or stream:
        command.append("--echo")
    if elf:
        command += ["--elf", elf]
    if out:
        command += ["--out", out]
    process = subprocess.Popen(command)
    interrupted = False
    # Ctrl+C reaches device.py too; it saves the capture before exiting.
    while True:
        try:
            return process.wait()
        except KeyboardInterrupt:
            if interrupted:
                return 130
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
    command = device_command("reset", "--purpose", "autana reset")
    if capture:
        command += ["--capture"]
        if seconds is not None:
            command += ["--seconds", str(seconds)]
    if verbose:
        command.append("--verbose")
    return subprocess.call(command)


def selftest(args):
    """Build+flash the diagnostics+autorun image and run every suite this
    project registers, on the device. Can take minutes - the full run's
    own budget, not a bug in this command."""
    rest = list(args)
    verbose = "--verbose" in rest
    if verbose:
        rest.remove("--verbose")
    perf_scope = "--perf-scope" in rest
    if perf_scope:
        rest.remove("--perf-scope")
    out, rest = pop_value(rest, "--out")
    usage = "usage: autana selftest [seconds] [--verbose] [--perf-scope] [--out PATH]"
    reject_unknown("selftest", rest)
    seconds = seconds_argument(rest, 3000.0, usage)
    project = resolve_project()
    print(f"autana selftest: every suite, {project}", flush=True)
    command = device_command(
        "selftest", "--worktree", project, "--max-seconds", str(seconds),
        "--purpose", "autana selftest",
    )
    if verbose:
        command.append("--verbose")
    if perf_scope:
        command.append("--perf-scope")
    if out:
        command += ["--out", out]
    return subprocess.call(command)


BATCH_USAGE = ("usage: autana batch <suite> [<suite> ...] [--runs N] [--perf-scope] [--verbose] "
              "[--out PATH] [--expect-build-id ID]")


def batch(args):
    """The old spelling of `suite <name>... --flash`, still the way to flash
    once and capture several suites `--runs` times (3 when omitted) under
    one lock - always the diagnostics image, since a suite only exists to
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
        return subprocess.call(device_command("status", "--json"))
    return subprocess.call(device_command("status"))


def release(args):
    """Release a lock this session holds, before its own command would have
    - the token comes from what that command printed when it acquired it."""
    if len(args) != 1:
        sys.exit("usage: autana release <token>")
    return subprocess.call(device_command("release", "--token", args[0]))


def hand(args):
    """Reserve the board for a maintainer sitting at it - autana refuses new
    work against it until `autana take-back`."""
    usage = "usage: autana hand [--wait <seconds>] <note...>"
    wait = []
    if args and args[0] == "--wait":
        if len(args) < 3:
            sys.exit(usage)
        wait = args[:2]
        args = args[2:]
    if not args:
        sys.exit(usage)
    command = device_command("hand-to-human", "--note", " ".join(args), *wait)
    if not wait:
        return subprocess.call(command)
    process = subprocess.Popen(command)
    try:
        return process.wait()
    except KeyboardInterrupt:
        try:
            code = process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait()
            code = None
        if code != 3:
            print("human reservation wait interrupted")
        return 3


def take_back(args):
    """Clear a reservation `autana hand` made, freeing the board again."""
    reject_unknown("take-back", args)
    if args:
        sys.exit("usage: autana take-back")
    return subprocess.call(device_command("take-back"))


SUITE_REGISTRATION = re.compile(r"SUITE_REGISTER(_ON_REQUEST)?\(\s*([A-Za-z_]\w*)\s*\)")


def suite_list(args):
    """What this project registers, read from its sources: a suite names
    itself where it is defined and the board serves no listing verb, so there
    is nowhere else to ask. A name is runnable once a build carrying it is on
    the board - which variant and scope was flashed decides that, not this."""
    json_output = "--json" in args
    args = [arg for arg in args if arg != "--json"]
    if len(args) > 1:
        sys.exit("usage: autana suite list [text] [--json]")
    wanted = args[0].lower() if args else ""
    project = Path(resolve_project())

    found = {}
    for source in project.glob("launcher/**/*.c"):
        # A build directory holds generated copies of the same sources.
        if "build" in source.parts:
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        for on_request, name in SUITE_REGISTRATION.findall(text):
            found[name] = (source.relative_to(project).as_posix(), bool(on_request),
                           "#ifdef DEVICE_BUILD" in text)

    shown = [{"name": name, "source": found[name][0], "on_request": found[name][1],
              "device_only": found[name][2]} for name in sorted(found) if wanted in name.lower()]
    if json_output:
        print(json.dumps({"suites": shown}))
        return 0
    for row in shown:
        name, where = row["name"], row["source"]
        on_request, device_only = row["on_request"], row["device_only"]
        marks = "".join([" [on request]" if on_request else "", " [device]" if device_only else ""])
        print(f"  {name}{marks}\n      {where}")
    kept = f" matching '{wanted}'" if wanted else ""
    print(f"{len(shown)} suite(s){kept}")
    if any(row["on_request"] for row in shown):
        print("[on request] is left out of a full run: asking for it by name is the only way it runs")
    return 0


SUITE_USAGE = ("usage: autana suite <name> [<name> ...] [seconds] [--runs N] [--flash] "
              "[--perf-scope] [--verbose] [--out PATH] [--expect-build-id ID] | "
              "autana suite list [text]")


def suite(args):
    """One or more registered suites, captured under one lock - against the
    image already on the board, or, with `--flash`, built and flashed first.
    The name is each suite's own function, as SUITE_REGISTER() in its source
    spells it. `seconds` caps the whole run, 600 when omitted - the same
    idea as `selftest`'s own trailing `seconds`."""
    if not args:
        sys.exit(SUITE_USAGE)
    if args[0] == "list":
        return suite_list(args[1:])
    positional, rest = [], list(args)
    while rest and not rest[0].startswith("--"):
        positional.append(rest.pop(0))
    if not positional:
        sys.exit(SUITE_USAGE)
    seconds = 600.0
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
    perf_scope = "--perf-scope" in rest
    if perf_scope:
        rest.remove("--perf-scope")
    verbose = "--verbose" in rest
    if verbose:
        rest.remove("--verbose")
    runs, rest = pop_value(rest, "--runs")
    out, rest = pop_value(rest, "--out")
    expect_build_id, rest = pop_value(rest, "--expect-build-id")
    reject_unknown("suite", rest)
    if rest:
        sys.exit(SUITE_USAGE)
    # Only --flash reads the project - it is what gets built. Without it,
    # `suite` is board-only: the capture still names a project (device.py's
    # own --worktree is metadata, not something it builds), but any
    # directory does, unvalidated, same as project_override()'s other use.
    project = resolve_project() if flash else (project_override() or str(Path.cwd()))
    runs = runs or "1"
    print(f"autana suite: {', '.join(names)} x{runs}" + (" (flash)" if flash else ""), flush=True)
    command = device_command(
        "batch", "--worktree", project, "--variant", "diag", "--runs", runs,
        "--max-seconds", str(seconds),
        "--purpose", "autana suite",
    )
    if not flash:
        command.append("--no-flash")
    for name in names:
        command += ["--suite", name]
    if perf_scope:
        command.append("--perf-scope")
    if verbose:
        command.append("--verbose")
    if out:
        command += ["--out", out]
    if expect_build_id:
        command += ["--expect-build-id", expect_build_id]
    return subprocess.call(command)


# A console line is asked from a prompt somebody is sitting at: a board another
# owner holds is reported at once rather than waited for, which from the
# outside is a terminal that has hung. The few seconds cover a lock changing
# hands between the look and the send.
SEND_WAIT_S = 5


def board_holder():
    """'held by <owner> for <purpose>' when someone else has the board a
    command would use, else ''. That board is AUTANA_BOARD's, else the only
    one plugged in; with several plugged in and none named, device.py's own
    refusal says so.

    Every line of a console session is a device.py of its own under one
    autana, so a lock this autana already holds is not somebody else's and
    the session does not refuse itself."""
    result = subprocess.run(device_command("status", "--json"), capture_output=True, text=True)
    try:
        boards = json.loads(result.stdout)["boards"]
    except (ValueError, KeyError, TypeError):
        return ""
    # device.py already narrows the list to AUTANA_BOARD's board, plugged or not.
    candidates = boards if os.environ.get("AUTANA_BOARD") else [b for b in boards if b["port"]]
    if len(candidates) != 1:
        return ""
    board = candidates[0]
    holder = board["holder"]
    if board["state"] == "held" and holder["owner"] != owner():
        return f"held by {holder['owner']} for {holder['purpose']}"
    return ""


def send(line, reply="TUNE", purpose="autana tune", optional=False, seconds=None, until=None):
    """One console line to the device, under the lock. Returns (exit code,
    reply lines). `reply` is what the answer's lines start with. `until` are
    the prefixes that end the answer - one reply-line verb needs only
    `[reply]` itself (device.py's default when `until` is omitted; util/tune's
    three endings are its own default for `reply="TUNE"`), a multi-line one
    (an app's own command) passes `[<PREFIX>_END, <PREFIX>_ERR]`. `optional`
    is for a verb that answers only when something is wrong (TOUCH, IMU): a
    timeout with nothing seen is success, not "no reply", since silence is
    that verb's normal happy path."""
    holder = board_holder()
    if holder:
        print(f"the board is busy - {holder}\nnothing was sent; try again when it is free",
              file=sys.stderr)
        return 3, []
    command = device_command("send", line, "--purpose", purpose, wait=SEND_WAIT_S)
    if reply != "TUNE":
        command += ["--reply", reply]
        for one_until in until if until is not None else [reply]:
            command += ["--until", one_until]
    if optional:
        command.append("--optional")
    if seconds is not None:
        command += ["--seconds", str(seconds)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)
    return result.returncode, [answer for answer in result.stdout.splitlines() if answer.startswith(reply)]


def screenshot(args):
    """SCREENSHOT, captured and decoded by launcher/tools/device/screenshot.py's own
    read_screenshot()/write_capture(), under the device lock - see
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
    code, _ = send("FREEZE", reply="FREEZE_STATE", purpose="autana screenshot --frames")
    for i in range(frames):
        if code:
            break
        if i:
            code, _ = send("STEP", reply="FREEZE_STATE", purpose="autana screenshot --frames")
            if code:
                break
        code = capture_screenshot(f"{out}-{i:02d}", view)
    resumed, _ = send("RESUME", reply="FREEZE_STATE", purpose="autana screenshot --frames")
    return code or resumed


def capture_screenshot(out, view):
    holder = board_holder()
    if holder:
        print(f"the board is busy - {holder}\nnothing was sent; try again when it is free",
              file=sys.stderr)
        return 3
    command = device_command("screenshot", "--purpose", "autana screenshot")
    if out:
        command += ["--out", out]
    if view:
        command.append(view)
    return subprocess.call(command)


def freeze(args):
    reject_unknown("freeze", args)
    if args:
        sys.exit("usage: autana freeze")
    code, replies = send("FREEZE", reply="FREEZE_STATE", purpose="autana freeze")
    print("\n".join(replies))
    return code


def resume(args):
    reject_unknown("resume", args)
    if args:
        sys.exit("usage: autana resume")
    code, replies = send("RESUME", reply="FREEZE_STATE", purpose="autana resume")
    print("\n".join(replies))
    return code


def step(args):
    reject_unknown("step", args)
    if len(args) > 1:
        sys.exit("usage: autana step [N]")
    count = args[0] if args else ""
    if count and not (count.isdigit() and int(count) >= 1):
        sys.exit("usage: autana step [N] - N is a positive count")
    code, replies = send("STEP " + count if count else "STEP", reply="FREEZE_STATE",
                         purpose="autana step")
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
    code, replies = send("TOUCH " + " ".join(args), reply="TOUCH", purpose="autana touch",
                         optional=True, seconds=0.5)
    print("\n".join(replies) if replies else "sent")
    return code


def imu(args):
    reject_unknown("imu", args)
    if args == ["release"]:
        code, replies = send("IMU release", reply="IMU", purpose="autana imu", optional=True, seconds=0.5)
        print("\n".join(replies) if replies else "sent")
        return code
    if len(args) != 3 or not all(is_int(value) for value in args):
        sys.exit("usage: autana imu <ax> <ay> <az> | autana imu release")
    code, replies = send("IMU " + " ".join(args), reply="IMU", purpose="autana imu",
                         optional=True, seconds=0.5)
    print("\n".join(replies) if replies else "sent")
    return code


def gesture(args, verb, usage):
    reject_unknown(verb, args)
    if not all(is_int(value) for value in args):
        sys.exit(usage)
    code, replies = send(verb.upper() + " " + " ".join(args), reply=verb.upper(), until=[verb.upper() + "_OK"],
                         purpose="autana " + verb)
    print("\n".join(replies))
    return code


def tap(args):
    if len(args) != 2:
        sys.exit("usage: autana tap <x> <y>")
    return gesture(args, "tap", "usage: autana tap <x> <y>")


def press(args):
    if len(args) not in (2, 3):
        sys.exit("usage: autana press <x> <y> [ms]")
    return gesture(args, "press", "usage: autana press <x> <y> [ms]")


def drag(args):
    if len(args) != 5:
        sys.exit("usage: autana drag <x0> <y0> <x1> <y1> <ms>")
    return gesture(args, "drag", "usage: autana drag <x0> <y0> <x1> <y1> <ms>")


def button(args):
    reject_unknown("button", args)
    if len(args) not in (1, 2) or args[0] not in ("boot", "power") \
            or len(args) == 2 and args[1] not in ("short", "long"):
        sys.exit("usage: autana button <boot|power> [short|long]")
    code, replies = send("BUTTON " + " ".join(args), reply="BUTTON",
                         until=["BUTTON_OK", "BUTTON_ERR"], purpose="autana button")
    print("\n".join(replies))
    return code


def docs(args):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "docs"))
    import docs_search
    return docs_search.main(args, root=resolve_project())


def apps(args):
    reject_unknown("apps", args, ("--json",))
    json_output = read_json_flag(args, "usage: autana apps [--json]")
    code, replies = send("APPS", reply="APPS", until=["APPS_END"], purpose="autana apps")
    if not json_output:
        print("\n".join(reply for reply in replies if reply.startswith("APPS ")))
    elif code == 0:
        print(json.dumps({"apps": parse_apps(replies)}))
    return code


def parse_apps(replies):
    rows = []
    for reply in replies:
        match = re.fullmatch(r"APPS name=(.*) running=([01])", reply)
        if match:
            rows.append({"name": match[1], "running": match[2] == "1"})
    return rows


def open_app(args):
    reject_unknown("open", args)
    if len(args) != 1:
        sys.exit("usage: autana open <name>")
    code, replies = send("OPEN " + args[0], reply="OPEN", purpose="autana open")
    print("\n".join(replies))
    return code


def home(args):
    reject_unknown("home", args)
    if args:
        sys.exit("usage: autana home")
    code, replies = send("HOME", reply="HOME", purpose="autana home")
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
    unambiguous - `theme_rgb` for `ridge.theme_rgb`, the same drop-the-owner shorthand
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
    same listing; a name and a value sets it; `reset`/`save` are recognised
    only in first position, so a tunable actually named that would still be
    reachable through the filtered listing."""
    json_output = "--json" in args
    args = [arg for arg in args if arg != "--json"]
    reject_unknown("tune", args)
    if json_output and (len(args) > 1 or args and args[0] in ("save", "reset")):
        sys.exit("usage: autana tune [text] [--json]")
    if args and args[0] == "save":
        if len(args) > 1:
            sys.exit("usage: autana tune save")
        return save()
    if args and args[0] == "reset":
        if len(args) != 2:
            sys.exit("usage: autana tune reset <name>")
        return tune_reset(args[1])
    if len(args) > 2:
        sys.exit("usage: autana tune [text] | autana tune <name> [value] | "
                 "autana tune reset <name> | autana tune save")
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


def literal(name, value):
    return f"0x{int(value):06X}" if name.endswith("_rgb") else str(int(value))


TUNE_LINE = re.compile(r"^TUNE\(\s*(\w+)\s*,\s*(\w+)\s*,", re.MULTILINE)


def declarations(project):
    """{tunable name: file}, from the TUNE(owner, what, ...) lines in the tree."""
    found = {}
    for source in (Path(project) / "launcher" / "main").rglob("*.[ch]"):
        text = source.read_text(encoding="utf-8", errors="replace")
        for owner, what in TUNE_LINE.findall(text):
            if owner != "owner":  # tune.h's own macro definition
                found[f"{owner}.{what}"] = source
    return found


def save():
    """The device's values, written into the TUNE lines they came from."""
    project = resolve_project()
    where = declarations(project)
    changed = 0
    for name, value, _low, _high, _default in tunables():
        if name not in where:
            print(f"  {name}: not declared in this project - skipped")
            continue
        source = where[name]
        owner, what = name.split(".", 1)
        text = source.read_bytes().decode("utf-8")
        declared = re.search(r"^TUNE\(\s*" + re.escape(owner) + r"\s*,\s*" + re.escape(what) + r"\s*,\s*([^,]+?)\s*,",
                             text, re.MULTILINE)
        if declared is None:
            print(f"  {name}: no TUNE({owner}, {what}, ...) in {source.name} - skipped")
            continue
        old = declared.group(1)
        try:
            same = int(old, 0) == int(value)
        except ValueError:
            same = False  # a macro or an expression: becomes the number it stood for
        if same:
            continue
        start, end = declared.span(1)
        source.write_bytes((text[:start] + literal(name, value) + text[end:]).encode("utf-8"))
        print(f"  {name}: {old} -> {literal(name, value)}   ({source.relative_to(project).as_posix()})")
        changed += 1
    print(f"{changed} value(s) written" if changed else "the source already has the device's values")
    return 0



def forward(line, verb):
    """A line no autana command recognises, sent to the device as typed -
    the maintainer's rule that every board operation goes through autana,
    for an app's own command (docs/tools/Autana-CLI.md's "Adding a command
    from an app") same as any built-in one. The reply prefix is the line's
    own first word in capitals: an app's reply always starts with its own
    prefix, so this returns as soon as `<PREFIX>_END`/`<PREFIX>_ERR`
    arrives rather than waiting out send()'s own window - a raw verb with
    no dedicated autana command of its own (runsuite, today) falls back to
    that window, since nothing then completes early."""
    reply = verb.upper()
    _, replies = send(line, reply=reply, until=[reply + "_END", reply + "_ERR"],
                      purpose=f"autana console {verb}", optional=True)
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
            line = input("autana> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not line:
            continue
        words = shlex.split(line)
        verb, rest = words[0], words[1:]
        if verb in ("quit", "exit", "q"):
            return 0
        try:
            if verb == "help":
                print(help_text(rest, prefix=""))
            elif verb in COMMANDS and verb != "console":
                run_command(COMMANDS[verb], rest)
            else:
                replies = forward(line, verb)
                print("\n".join(replies) if replies else "sent")
        except SystemExit as stop:
            # a command's own refusal ends that command, not the session
            if stop.code not in (0, None):
                print(stop.code)


Command = namedtuple("Command", "name handler usages")


def alias(old, new, handler):
    """`old` still works - one migration line to stderr, then straight into
    `handler` - so a script built on a spelling `new` replaced does not
    break on merge."""
    def wrapped(args):
        print(f"autana {old}: use `autana {new}` - this spelling still works", file=sys.stderr)
        return handler(args)
    return wrapped


LOCK_VERBS = {"id": identify, "release": release, "hand": hand, "take-back": take_back}
DEBUG_VERBS = {"freeze": freeze, "resume": resume, "step": step, "touch": touch, "imu": imu,
              "framewatch": framewatch}


def lock(args):
    """`autana lock <verb>` - id/release/hand/take-back, the lock-sharing
    commands a session reaches for far less than `status`, which stays
    top-level on its own."""
    if not args or args[0] not in LOCK_VERBS:
        sys.exit("usage: autana lock <" + "|".join(LOCK_VERBS) + "> ...")
    verb, rest = args[0], args[1:]
    return LOCK_VERBS[verb](rest)


def debug(args):
    """`autana debug <verb>` - freeze/resume/step/touch/imu/framewatch, kept
    out of the top-level help (`autana help debug` still lists them) since a
    session rarely needs them."""
    if not args or args[0] not in DEBUG_VERBS:
        sys.exit("usage: autana debug <" + "|".join(DEBUG_VERBS) + "> ...")
    verb, rest = args[0], args[1:]
    return DEBUG_VERBS[verb](rest)


# (key, title, commands). `autana help <key>` shows one group; each usage is
# (synopsis without "autana ", one line of what it does). "debug" is left
# out of the bare `autana help` listing - see HIDDEN_GROUPS.
COMMAND_GROUPS = (
    ("build", "Build and flash", (
        Command("build", build, (
            ("build [rel|dev|diag] [--perf-scope]", "build this project, no board; dev when omitted"),
            ("build diag --check", "the diagnostics build plus the complexity ratchet, no board"))),
        Command("flash", flash, (
            ("flash [rel|dev|diag] [--quiet] [--perf-scope]",
             "build and flash this project; dev when omitted"),)),
        Command("buildid", buildid, (
            ("buildid [--json]", "the BUILD_ID the board is running"),)),
    )),
    ("tests", "Tests", (
        Command("suite", suite, (
            ("suite <name>... [seconds] [--runs N] [--flash] [--verbose]",
             "run suites under one lock; --flash builds and flashes first"),
            ("suite list [text] [--json]", "the suites this project registers"))),
        Command("selftest", selftest, (
            ("selftest [seconds] [--verbose]",
             "build diagnostics+autorun, run every suite on the board"),)),
    )),
    ("watch", "Watch the board", (
        Command("monitor", monitor, (
            ("monitor [seconds] [--follow] [--stream] [--elf PATH]",
             "the console live until Ctrl+C, or for N s"),)),
        Command("reset", reset, (
            ("reset [--capture [seconds]] [--verbose]", "reboot the board; --capture records the boot"),)),
        Command("screenshot", screenshot, (
            ("screenshot [--as-shown|--framebuffer] [-o PATH]", "the panel as PATH.png plus PATH.json"),
            ("screenshot --frames N -o PATH", "N consecutive frames, PATH-00 on, stepped while frozen"),)),
    )),
    ("input", "Drive input", (
        Command("tap", tap, (("tap <x> <y>", "tap a point"),)),
        Command("press", press, (("press <x> <y> [ms]", "hold a point; 1000 ms when omitted"),)),
        Command("drag", drag, (("drag <x0> <y0> <x1> <y1> <ms>", "drag between two points"),)),
        Command("button", button, (("button <boot|power> [short|long]", "press a board button"),)),
    )),
    ("apps", "Apps", (
        Command("apps", apps, (("apps [--json]", "the registered apps, and which is running"),)),
        Command("open", open_app, (("open <name>", "enter an app; case-insensitive prefix"),)),
        Command("home", home, (("home", "back to the launcher"),)),
    )),
    ("tune", "Tunables", (
        Command("tune", tune, (
            ("tune [text] [--json]", "list the tunables, names containing text"),
            ("tune <name> [value]", "show one, or set it on the board"),
            ("tune reset <name>", "back to the value the source declares"),
            ("tune save", "write the board's values into this project's TUNE() lines"))),
    )),
    ("lock", "Sharing the board", (
        Command("status", status, (("status [--json]", "who holds the board, and who waits"),)),
        Command("lock", lock, (
            ("lock id [--json]", "the name this session holds the lock under"),
            ("lock release <token>", "release a lock this session holds"),
            ("lock hand [--wait <seconds>] <note...>", "reserve the board for a person at it"),
            ("lock take-back", "clear that reservation"))),
    )),
    ("debug", "Debug", (
        Command("debug", debug, (
            ("debug freeze", "stop the frame loop where it is"),
            ("debug resume", "run it again"),
            ("debug step [N]", "advance N frames while frozen; 1 when omitted"),
            ("debug touch <down|up> <x> <y>", "one raw touch level; up hands back"),
            ("debug imu <ax> <ay> <az>", "raw accelerometer counts; imu release hands back"),
            ("debug framewatch", "allocations and log lines repeating frame after frame, as JSON"))),
    )),
    ("docs", "No board needed", (
        Command("docs", docs, (
            ("docs <question...>", "the sections that answer it, and where to read on"),
            ("docs --section <path:line>", "one section whole; --deep adds its subsections"),
            ("docs --outline <path>", "a document's headings, with lines and sizes"),
            ("docs --ask <question...>", "a short answer from the local chat model"))),
    )),
)

# Left out of the bare `autana help` listing; `autana help <key>` still shows
# a hidden group in full, same as any other topic.
HIDDEN_GROUPS = {"debug"}

# Old spellings kept working - see alias()'s own docstring. "batch" moved
# to suite() itself (its translation is not a plain forward) so it is not
# here; every other renamed verb is a bare forward into its new group.
RENAMED_VERBS = {
    "id": ("lock id", identify), "release": ("lock release", release),
    "hand": ("lock hand", hand), "take-back": ("lock take-back", take_back),
    "freeze": ("debug freeze", freeze), "resume": ("debug resume", resume),
    "step": ("debug step", step), "touch": ("debug touch", touch), "imu": ("debug imu", imu),
    "framewatch": ("debug framewatch", framewatch),
}

COMMANDS = {command.name: command.handler
            for _, _, commands in COMMAND_GROUPS for command in commands}
COMMANDS["console"] = console
COMMANDS["batch"] = batch
COMMANDS.update({old: alias(old, new, handler) for old, (new, handler) in RENAMED_VERBS.items()})
HELP_TOPICS = [key for key, _, _ in COMMAND_GROUPS] + [
    command.name for _, _, commands in COMMAND_GROUPS for command in commands] + ["flags"]
USAGE_WIDTH = 34

# (flag, what it does, the commands that take it) - left out of every command's
# own usage line (`autana help flags` instead) since most commands take most
# of these and repeating them on every line was unreadable.
BOARD_FLAGS = (
    ("--out PATH", "write the one capture here instead of the default path",
     "selftest, suite, monitor"),
    ("--expect-build-id ID", "refuse to run a suite unless the board, or the image `--flash` "
                             "just wrote, carries this BUILD_ID",
     "suite"),
    ("--project PATH", "act on PATH instead of the current directory - it must itself carry "
                       "launcher/CMakeLists.txt, no searching parent directories",
     "build, flash, selftest, suite, suite list, tune save, docs"),
)


def board_flags_text(prefix=""):
    width = max(len(flag) for flag, _, _ in BOARD_FLAGS)
    lines = ["Every board command's lock owner is \"<user>@<host>:<pid>\", or "
             "\"<AUTANA_DEVICE_OWNER>:<pid>\" when that variable is set. AUTANA_DEVICE_WAIT "
             "overrides how long a command waits for the board's lock (device.py's own "
             "default: 600 s).",
             "",
             "Flags (on top of each command's own usage above)"]
    for flag, summary, commands in BOARD_FLAGS:
        lines.append(f"  {flag:<{width}}  {summary}")
        lines.append(f"  {'':<{width}}  on: {commands}")
    if not prefix:
        lines.append("")
    return "\n".join(lines)


# The five things a first run actually needs, each a real example rather than
# a placeholder - `autana help` led three newcomer reads past this before
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
    five-line quickstart and leaves out a hidden group (HIDDEN_GROUPS) -
    `autana help <that group>` still shows it in full."""
    topic = args[0] if args else None
    if topic == "flags":
        return board_flags_text(prefix).rstrip()
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
        lines.append(f"{prefix}help flags    --out/--expect-build-id/--project, which commands "
                     "take them, and how the lock owner and its wait are set")
        lines.append(f"{prefix}help [topic]  one group or command"
                     + ("" if prefix else "; quit leaves the session"))
    return "\n".join(lines).rstrip()


def completion_candidates(line, prefix):
    """Return command or static argument matches for the current word."""
    if not any(character.isspace() for character in line):
        words = (set(COMMANDS) - {"console"}) | {"help", "quit"}
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


def main():
    if len(sys.argv) < 2:
        sys.exit(console())
    if sys.argv[1] in ("--version", "-V"):
        print(__version__)
        sys.exit(0)
    if sys.argv[1] in ("help", "--help", "-h"):
        print(help_text(sys.argv[2:]))
        sys.exit(0)
    if sys.argv[1] not in COMMANDS:
        # A one-shot the same as a forwarded line in a session (forward()'s
        # own docstring) - every board operation goes through autana, not
        # only the ones with a command of their own.
        replies = forward(" ".join(sys.argv[1:]), sys.argv[1])
        print("\n".join(replies) if replies else "sent")
        sys.exit(0)
    sys.exit(run_command(COMMANDS[sys.argv[1]], sys.argv[2:]))


if __name__ == "__main__":
    main()
