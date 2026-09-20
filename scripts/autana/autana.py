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
    autana get <name>               one of them
    autana set <name> <value>       change one on the running device - no build, no flash.
                                    A name may be given without its owner when that is
                                    unambiguous: trail for ridge.trail.
                                    Nothing is kept across a reboot.
    autana reset <name>             back to the value the source declares
    autana save                     write the device's current values into the TUNE(...)
                                    lines of the worktree you are in, so they are what
                                    the next build - and release - is made with

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

Terminal commands, not agent commands: they cost no model tokens. Run from
any folder of any autana worktree - what a command acts on is the worktree
you are standing in, not the checkout this file came from. Anything that
touches the board goes through scripts/device/device.py, which takes the
device lock; nothing here opens the serial port itself.

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


def send(line, reply="TUNE", purpose="autana tune"):
    """One console line to the device, under the lock. Returns (exit code,
    reply lines). `reply` is what the answer's lines start with, and also
    what ends the answer for a verb that replies once - util/tune's three
    endings are device.py's default and need no saying."""
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
        command += ["--reply", reply, "--until", reply]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)
    return result.returncode, [answer for answer in result.stdout.splitlines() if answer.startswith(reply)]


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


def tune(args):
    if len(args) > 1:
        sys.exit("usage: autana tune [text]")
    rows = [row for row in tunables() if not args or args[0] in row[0]]
    if not rows:
        print("no tunables" + (f" containing '{args[0]}'" if args else ""))
        return 0
    width = max(len(row[0]) for row in rows)
    for name, value, low, high, default in rows:
        changed = int(value) != int(default)
        if name.endswith("_rgb"):
            # a colour reads as one; SET takes 0x38D6E8 back
            value, low, high, default = (f"0x{int(number):06X}" for number in (value, low, high, default))
        print(f"{name:<{width}}  {value:>8}   ({low}..{high})" + (f"   * source has {default}" if changed else ""))
    return 0


def get(args):
    if len(args) != 1:
        sys.exit("usage: autana get <name>")
    code, replies = send("GET " + full_name(args[0]))
    print("\n".join(reply.split(" ", 1)[1] for reply in replies))
    return code


def set_value(args):
    if len(args) != 2:
        sys.exit("usage: autana set <name> <value>")
    code, replies = send("SET " + full_name(args[0]) + " " + args[1])
    print("\n".join(reply.split(" ", 1)[1] for reply in replies))
    return code


def reset(args):
    if len(args) != 1:
        sys.exit("usage: autana reset <name>")
    code, replies = send("RESET " + full_name(args[0]))
    print("\n".join(reply.split(" ", 1)[1] for reply in replies))
    return code


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


def save(args):
    """The device's values, written into the TUNE lines they came from."""
    if args:
        sys.exit("usage: autana save")
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


CONSOLE_HELP = """  tune [text]          list the tunables (names containing text)
  <name>               show one                  (get <name> works too)
  <name> <value>       change it on the device   (set <name> <value> works too)
  reset <name>         back to the value the source declares
  save                 write the device's values into this worktree's source
  flash [rel|dev|diag] build and flash this worktree
  monitor [seconds]    print what the board says
  suite <name> [secs]  run one registered suite on the board
  suite list [text]    the suites this worktree registers
  buildid              what the board says it is running
  id                   what the device lock calls this session, and its pid
  help, quit"""


def console(_args=None):
    """A session with the device: each line takes the lock, asks, and lets go,
    so the board is free for anything else between two of them."""
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
            elif len(words) == 1:
                get(words)
            elif len(words) == 2:
                set_value(words)
            else:
                print("not understood - 'help' lists what is")
        except SystemExit as stop:
            # a command's own refusal ends that command, not the session
            if stop.code not in (0, None):
                print(stop.code)


COMMANDS = {"flash": flash, "tune": tune, "get": get, "set": set_value, "reset": reset, "save": save,
            "monitor": monitor, "suite": suite, "console": console, "id": identify, "buildid": buildid}


def main():
    if len(sys.argv) < 2:
        sys.exit(console())
    if sys.argv[1] in ("help", "--help", "-h"):
        print(__doc__.strip())
        sys.exit(0)
    if sys.argv[1] not in COMMANDS:
        sys.exit(__doc__.strip())
    sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]))


if __name__ == "__main__":
    main()
