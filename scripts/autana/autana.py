#!/usr/bin/env python3
"""autana - terminal commands for the autana engine repo.

    autana                          a console session with the device: type "help" in it

    autana flash [rel|dev|diag] [--quiet]
                                    build and flash the worktree you are in (dev when omitted);
                                    the build and flash output streams here, --quiet leaves it
                                    in the log file only

    autana monitor [seconds]        print what the board says, for 60 seconds when omitted
    autana suite <name> [seconds]   run one registered suite and print what it prints. A
                                    diagnostics build serves these with no rebuild and no
                                    reflash, and only one built WITHOUT autorun ever reaches
                                    the prompt to be asked.
    autana suite list [text]        the suites this worktree registers, read from its
                                    sources; [text] keeps the names containing it

    autana tune [text]              the numbers a development build lets you change, live,
                                    with their ranges; [text] keeps the names containing it
    autana tune <name>              one of them, or - when the name is not exactly one of
                                    them - the same filtered listing as [text]. A name may
                                    be given without its owner when that is unambiguous:
                                    trail for ridge.trail.
    autana tune <name> <value>      change one on the running device - no build, no flash,
                                    and nothing kept across a reboot
    autana tune reset <name>        back to the value the source declares
    autana tune save                write the device's current values into the TUNE(...)
                                    lines of the worktree you are in, so they are what
                                    the next build - and release - is made with

    autana screenshot [-o PATH]     what the panel shows right now, as PATH.png plus a
                                    PATH.json state snapshot; PATH defaults to a
                                    timestamped name in the current directory
    autana freeze                   stop the frame loop where it is
    autana resume                   let the frame loop run again
    autana step [N]                 advance N frames while frozen (1 when N is omitted)
    autana touch <down|up> <x> <y>  stand in for the touch controller
    autana imu <ax> <ay> <az>       stand in for the IMU, raw accelerometer counts

    autana buildid                  the BUILD_ID the board answers with, so what is
                                    running can be checked against what was flashed.
                                    A development build has the console that answers;
                                    a release one has none.
    autana id                       the name this autana holds the board under, and the
                                    pid it is: autana-cli@<pid in base36>. It is what
                                    "autana monitor" shows waiting when two sessions
                                    want the board, and what to look for in the task
                                    list when one will not let go.
    autana help                     this

Run from any folder of any autana worktree - what a command acts on is the
worktree you are standing in, not the checkout this file came from. Anything
that touches the board goes through scripts/device/device.py, which takes
the device lock; nothing here opens the serial port itself.

The launchers are tools/autana and tools/autana.cmd, and
scripts/add-tools-to-path.sh puts tools/ on the PATH. The commands are
documented in docs/tools/Autana-CLI.md.
"""

import gzip
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path

VARIANTS = {"rel": "release", "release": "release", "dev": "dev", "diag": "diag"}

# What the device lock calls this autana. Two holders of one board are worth
# telling apart, and this label is the process's own pid in base36: short
# enough to read in a queue and reversible, so an entry that will not let go
# can be found in the task list. The lock records the pid of the device.py it
# spawns; this names the autana above it.
OWNER_PREFIX = "autana-cli@"
BASE36 = "0123456789abcdefghijklmnopqrstuvwxyz"


def base36(number, width=4):
    """`number` in base 36, padded to `width`. Never truncated: a shortened
    pid names some other process."""
    digits = ""
    while number:
        number, remainder = divmod(number, 36)
        digits = BASE36[remainder] + digits
    return digits.rjust(width, "0")


def owner():
    return OWNER_PREFIX + base36(os.getpid())


def git(*args):
    result = subprocess.run(["git", *args], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def engine_worktree():
    worktree = git("rev-parse", "--show-toplevel")
    if not worktree or not (Path(worktree) / "launcher").is_dir():
        sys.exit("autana: not inside an engine worktree (no launcher/ here)")
    return worktree


def device_tool():
    # Under the same scripts/ as this file, so the device tool is the one
    # belonging to the checkout on the PATH.
    device = Path(__file__).resolve().parents[1] / "device" / "device.py"
    if not device.is_file():
        sys.exit(f"autana: {device} not found")
    return device


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


def flash(args):
    quiet = "--quiet" in args
    args = [arg for arg in args if arg != "--quiet"]
    asked = args[0] if args else "dev"
    variant = VARIANTS.get(asked)
    if variant is None or len(args) > 1:
        sys.exit("usage: autana flash [rel|dev|diag] [--quiet]")

    worktree = engine_worktree()
    device = device_tool()
    branch = git("branch", "--show-current") or "detached"
    commit = git("rev-parse", "--short", "HEAD")
    dirty = " (dirty)" if git("status", "--porcelain") else ""
    print(f"autana flash: {variant} of {branch} @ {commit}{dirty}", flush=True)

    command = [
        sys.executable, "-u", str(device), "--owner", owner(),
        "flash", "--variant", variant, "--worktree", worktree, "--purpose", f"autana flash {asked}",
    ]
    return subprocess.call(command) if quiet else run_streaming_its_log(command)


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
    if args:
        sys.exit("usage: autana id")
    print(f"{owner()}   pid {os.getpid()}")
    return 0


def buildid(args):
    """What the BOARD says it is running, asked of it rather than read out of
    a build directory: the point of the question is whether the two agree."""
    if args:
        sys.exit("usage: autana buildid")
    code, replies = send("BUILDID", reply="BUILD_ID", purpose="autana buildid")
    if code != 0 or not replies:
        return code or 1
    print(replies[-1])
    return 0


def monitor(args):
    """The board's console, streamed for a while. The lock is held throughout -
    listening IS using the board, and two readers of one port interleave."""
    seconds = seconds_argument(args, 60.0, "usage: autana monitor [seconds]")
    return subprocess.call([
        sys.executable, "-u", str(device_tool()), "--owner", owner(),
        "listen", "--seconds", str(seconds), "--purpose", "autana monitor",
    ])


SUITE_REGISTRATION = re.compile(r"SUITE_REGISTER(_ON_REQUEST)?\(\s*([A-Za-z_]\w*)\s*\)")


def suite_list(args):
    """What this worktree registers, read from its sources: a suite names
    itself where it is defined and the board serves no listing verb, so there
    is nowhere else to ask. A name is runnable once a build carrying it is on
    the board - which variant and scope was flashed decides that, not this."""
    if len(args) > 1:
        sys.exit("usage: autana suite list [text]")
    wanted = args[0].lower() if args else ""
    worktree = Path(engine_worktree())

    found = {}
    for source in worktree.glob("launcher/**/*.c"):
        # A build directory holds generated copies of the same sources.
        if "build" in source.parts:
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        for on_request, name in SUITE_REGISTRATION.findall(text):
            found[name] = (source.relative_to(worktree).as_posix(), bool(on_request),
                           "#ifdef DEVICE_BUILD" in text)

    shown = sorted(name for name in found if wanted in name.lower())
    for name in shown:
        where, on_request, device_only = found[name]
        marks = "".join([" [on request]" if on_request else "", " [device]" if device_only else ""])
        print(f"  {name}{marks}\n      {where}")
    kept = f" matching '{wanted}'" if wanted else ""
    print(f"{len(shown)} suite(s){kept}")
    if any(found[name][1] for name in shown):
        print("[on request] is left out of a full run: asking for it by name is the only way it runs")
    return 0


def suite(args):
    """One registered suite, run where it can actually run. The name is the
    suite's own function, as SUITE_REGISTER() in its source spells it."""
    if not args:
        sys.exit("usage: autana suite <name> [seconds] | autana suite list [text]")
    if args[0] == "list":
        return suite_list(args[1:])
    name, rest = args[0], args[1:]
    # A perf row can sit silent for minutes; the cap is how long to wait for
    # the whole suite, not how long a quiet stretch inside one may last.
    seconds = seconds_argument(rest, 600.0, "usage: autana suite <name> [seconds]")
    print(f"autana suite: {name}", flush=True)
    return subprocess.call([
        sys.executable, "-u", str(device_tool()), "--owner", owner(),
        "run-suite", name, "--max-seconds", str(seconds), "--purpose", f"autana suite {name}",
    ])


# A console line is asked from a prompt somebody is sitting at: a board another
# owner holds is reported at once rather than waited for, which from the
# outside is a terminal that has hung. The few seconds cover a lock changing
# hands between the look and the send.
SEND_WAIT_S = 5


def board_holder(device):
    """'held by <owner> for ...' when someone else has the board, else ''.

    Every line of a console session is a device.py of its own under one
    autana, so a lock this autana already holds is not somebody else's and
    the session does not refuse itself."""
    mine = f"held by {owner()} "
    result = subprocess.run([sys.executable, str(device), "status"], capture_output=True, text=True)
    for line in result.stdout.splitlines():
        if line.startswith("held by ") and not line.startswith(mine):
            return line.strip()
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
    device = device_tool()
    holder = board_holder(device)
    if holder:
        print(f"the board is busy - {holder}\nnothing was sent; try again when it is free",
              file=sys.stderr)
        return 3, []
    if '"send"' not in device.read_text(encoding="utf-8"):
        sys.exit(f"autana: {device} has no 'send' command yet - "
                 "merge the claude/device-send branch into .dev")
    command = [sys.executable, str(device), "--owner", owner(), "--wait", str(SEND_WAIT_S), "send", line,
               "--purpose", purpose]
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
    """SCREENSHOT, captured and decoded the way launcher/tools/screenshot.sh
    does (launcher/tools/screenshot.py's own read_screenshot()/write_capture()),
    but under the device lock rather than opening the port directly - see
    device.py's own `screenshot` subcommand."""
    out = None
    if args[:1] and args[0] in ("-o", "--out"):
        if len(args) != 2:
            sys.exit("usage: autana screenshot [-o PATH]")
        out = args[1]
    elif args:
        sys.exit("usage: autana screenshot [-o PATH]")
    device = device_tool()
    holder = board_holder(device)
    if holder:
        print(f"the board is busy - {holder}\nnothing was sent; try again when it is free",
              file=sys.stderr)
        return 3
    command = [sys.executable, "-u", str(device), "--owner", owner(), "screenshot",
               "--purpose", "autana screenshot"]
    if out:
        command += ["--out", out]
    return subprocess.call(command)


def freeze(args):
    if args:
        sys.exit("usage: autana freeze")
    code, replies = send("FREEZE", reply="FREEZE_STATE", purpose="autana freeze")
    print("\n".join(replies))
    return code


def resume(args):
    if args:
        sys.exit("usage: autana resume")
    code, replies = send("RESUME", reply="FREEZE_STATE", purpose="autana resume")
    print("\n".join(replies))
    return code


def step(args):
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
    if len(args) != 3 or args[0] not in ("down", "up") or not is_int(args[1]) or not is_int(args[2]):
        sys.exit("usage: autana touch <down|up> <x> <y>")
    code, replies = send("TOUCH " + " ".join(args), reply="TOUCH", purpose="autana touch",
                         optional=True, seconds=0.5)
    print("\n".join(replies) if replies else "sent")
    return code


def imu(args):
    if len(args) != 3 or not all(is_int(value) for value in args):
        sys.exit("usage: autana imu <ax> <ay> <az>")
    code, replies = send("IMU " + " ".join(args), reply="IMU", purpose="autana imu",
                         optional=True, seconds=0.5)
    print("\n".join(replies) if replies else "sent")
    return code


def tunables():
    """[(name, value, low, high, default)] as the running device reports them."""
    code, replies = send("TUNE")
    if code != 0:
        sys.exit(code)
    rows = []
    for reply in replies:
        if not reply.startswith("TUNE ") or "=" not in reply:
            continue
        fields = dict(field.split("=", 1) for field in reply[len("TUNE "):].split(" "))
        name = reply[len("TUNE "):].split("=", 1)[0]
        rows.append((name, fields[name], fields.get("min", "?"), fields.get("max", "?"),
                     fields.get("default", fields[name])))
    return rows


def full_name(name):
    """`trail` for `ridge.trail`, when only one tunable ends that way."""
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
    unambiguous - `trail` for `ridge.trail`, the same drop-the-owner shorthand
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
    format_tune_rows(shown, text)
    return 0


def literal(name, value):
    return f"0x{int(value):06X}" if name.endswith("_rgb") else str(int(value))


TUNE_LINE = re.compile(r"^TUNE\(\s*(\w+)\s*,\s*(\w+)\s*,", re.MULTILINE)


def declarations(worktree):
    """{tunable name: file}, from the TUNE(owner, what, ...) lines in the tree."""
    found = {}
    for source in (Path(worktree) / "launcher" / "main").rglob("*.[ch]"):
        text = source.read_text(encoding="utf-8", errors="replace")
        for owner, what in TUNE_LINE.findall(text):
            if owner != "owner":  # tune.h's own macro definition
                found[f"{owner}.{what}"] = source
    return found


def save():
    """The device's values, written into the TUNE lines they came from."""
    worktree = engine_worktree()
    where = declarations(worktree)
    changed = 0
    for name, value, _low, _high, _default in tunables():
        if name not in where:
            print(f"  {name}: not declared in this worktree - skipped")
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
        print(f"  {name}: {old} -> {literal(name, value)}   ({source.relative_to(worktree).as_posix()})")
        changed += 1
    print(f"{changed} value(s) written" if changed else "the source already has the device's values")
    return 0


CONSOLE_HELP = """  tune [text]              list the tunables (names containing text)
  tune <name>              show one, or the same filtered list when the name is not exactly one
  tune <name> <value>      change it on the device
  tune reset <name>        back to the value the source declares
  tune save                write the device's values into this worktree's source
  screenshot [-o PATH]     what the panel shows right now
  freeze                   stop the frame loop where it is
  resume                   let the frame loop run again
  step [N]                 advance N frames while frozen (1 when omitted)
  touch <down|up> <x> <y>  stand in for the touch controller
  imu <ax> <ay> <az>       stand in for the IMU, raw counts
  flash [rel|dev|diag]     build and flash this worktree
  monitor [seconds]        print what the board says
  suite <name> [secs]      run one registered suite on the board
  suite list [text]        the suites this worktree registers
  buildid                  what the board says it is running
  id                       what the device lock calls this session, and its pid
  help, quit"""


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
                print(CONSOLE_HELP)
            elif verb in COMMANDS and verb != "console":
                COMMANDS[verb](rest)
            else:
                replies = forward(line, verb)
                print("\n".join(replies) if replies else "sent")
        except SystemExit as stop:
            # a command's own refusal ends that command, not the session
            if stop.code not in (0, None):
                print(stop.code)


COMMANDS = {"flash": flash, "tune": tune, "monitor": monitor, "suite": suite, "console": console,
            "id": identify, "buildid": buildid, "screenshot": screenshot, "freeze": freeze,
            "resume": resume, "step": step, "touch": touch, "imu": imu}


def main():
    if len(sys.argv) < 2:
        sys.exit(console())
    if sys.argv[1] in ("help", "--help", "-h"):
        print(__doc__.strip())
        sys.exit(0)
    if sys.argv[1] not in COMMANDS:
        # A one-shot the same as a forwarded line in a session (forward()'s
        # own docstring) - every board operation goes through autana, not
        # only the ones with a command of their own.
        replies = forward(" ".join(sys.argv[1:]), sys.argv[1])
        print("\n".join(replies) if replies else "sent")
        sys.exit(0)
    sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]))


if __name__ == "__main__":
    main()
