#!/usr/bin/env python3
"""autana - terminal commands for the autana engine repo.

    autana                  a console session with the device
    autana help [topic]     the commands, grouped; a topic is a group or a command

Run from any folder of any autana worktree: a command acts on the worktree
you are standing in. Anything that touches the board goes through
scripts/device/device.py, which takes the device lock. The command list is
COMMAND_GROUPS at the end of this file; docs/tools/Autana-CLI.md mirrors it.
"""

import gzip
import importlib
import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from collections import namedtuple
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher" / "tools"))
from espressif import idf_python  # noqa: E402  (path must be set up first)

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


def device_command(*args):
    """device.py imports pyserial, so it runs under ESP-IDF's Python even
    when some other interpreter started this file."""
    return [idf_python(), "-u", str(device_tool()), *args]


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
    perf_scope = "--perf-scope" in args
    args = [arg for arg in args if arg not in ("--quiet", "--perf-scope")]
    asked = args[0] if args else "dev"
    variant = VARIANTS.get(asked)
    if variant is None or len(args) > 1:
        sys.exit("usage: autana flash [rel|dev|diag] [--quiet] [--perf-scope]")

    worktree = engine_worktree()
    branch = git("branch", "--show-current") or "detached"
    commit = git("rev-parse", "--short", "HEAD")
    dirty = " (dirty)" if git("status", "--porcelain") else ""
    print(f"autana flash: {variant} of {branch} @ {commit}{dirty}", flush=True)

    command = device_command(
        "--owner", owner(),
        "flash", "--variant", variant, "--worktree", worktree, "--purpose", f"autana flash {asked}",
    )
    if perf_scope:
        command.append("--perf-scope")
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
    json_output = read_json_flag(args, "usage: autana buildid [--json]")
    code, replies = send("BUILDID", reply="BUILD_ID", purpose="autana buildid")
    if code != 0 or not replies:
        return code or 1
    if json_output:
        print(json.dumps(parse_buildid(replies[-1])))
    else:
        print(replies[-1])
    return 0


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
    usage = "usage: autana monitor [seconds] [--follow] [--stream] [--elf PATH]"
    if "--elf" in rest:
        index = rest.index("--elf")
        if index + 1 >= len(rest):
            sys.exit(usage)
        elf = rest[index + 1]
        del rest[index:index + 2]
    follow = "--follow" in rest
    if follow:
        rest.remove("--follow")
    stream = "--stream" in rest
    if stream:
        rest.remove("--stream")
    if follow and rest:
        sys.exit(usage)
    seconds = seconds_argument(rest, None, usage) if not follow else None
    terminal = sys.stdout.isatty()
    if seconds is None and not follow:
        if not terminal:
            print(usage, file=sys.stderr)
            raise SystemExit(2)
        follow = True
    command = device_command(
        "--owner", owner(),
        "listen", "--purpose", "autana monitor",
    )
    command += ["--follow"] if follow else ["--seconds", str(seconds)]
    if terminal or stream:
        command.append("--echo")
    if elf:
        command += ["--elf", elf]
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
    seconds = seconds_argument(rest, None, "usage: autana reset [--capture [seconds]] [--verbose]")
    if rest and not capture:
        sys.exit("usage: autana reset [--capture [seconds]] [--verbose]")
    command = device_command(
        "--owner", owner(),
        "reset", "--purpose", "autana reset",
    )
    if capture:
        command += ["--capture"]
        if seconds is not None:
            command += ["--seconds", str(seconds)]
    if verbose:
        command.append("--verbose")
    return subprocess.call(command)


def selftest(args):
    """Build+flash the diagnostics+autorun image and run every suite this
    worktree registers, on the device. Can take minutes - the full run's
    own budget, not a bug in this command."""
    rest = list(args)
    verbose = "--verbose" in rest
    if verbose:
        rest.remove("--verbose")
    seconds = seconds_argument(rest, 3000.0, "usage: autana selftest [seconds] [--verbose]")
    worktree = engine_worktree()
    print(f"autana selftest: every suite, {worktree}", flush=True)
    command = device_command(
        "--owner", owner(),
        "selftest", "--worktree", worktree, "--max-seconds", str(seconds),
        "--purpose", "autana selftest",
    )
    if verbose:
        command.append("--verbose")
    return subprocess.call(command)


BATCH_USAGE = "usage: autana batch <suite> [<suite> ...] [--runs N] [--perf-scope] [--verbose]"


def batch(args):
    """Flash once and capture one or more suites `--runs` times under one
    lock - see device.py's own batch() docstring for why this beats a
    sequence of separate `suite` calls on a shared board. Always the
    diagnostics image: a suite only exists to run in one, so a variant
    choice here would only ever have one real answer."""
    suites, runs, perf_scope, verbose = [], "3", False, False
    rest = list(args)
    while rest:
        arg = rest.pop(0)
        if arg == "--runs" and rest:
            runs = rest.pop(0)
        elif arg == "--perf-scope":
            perf_scope = True
        elif arg == "--verbose":
            verbose = True
        elif arg.startswith("--"):
            sys.exit(BATCH_USAGE)
        else:
            suites.append(arg)
    if not suites:
        sys.exit(BATCH_USAGE)
    worktree = engine_worktree()
    print(f"autana batch: {', '.join(suites)} x{runs}", flush=True)
    command = device_command(
        "--owner", owner(),
        "batch", "--worktree", worktree, "--variant", "diag", "--runs", str(runs),
        "--purpose", "autana batch",
    )
    for suite_name in suites:
        command += ["--suite", suite_name]
    if perf_scope:
        command.append("--perf-scope")
    if verbose:
        command.append("--verbose")
    return subprocess.call(command)


def status(args):
    """Who, if anyone, holds the board right now - and who is waiting."""
    if not read_json_flag(args, "usage: autana status [--json]"):
        return subprocess.call(device_command("status"))
    result = subprocess.run(device_command("status"), capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, end="", file=sys.stderr)
        return result.returncode
    print(json.dumps(parse_status(result.stdout)))
    return 0


def parse_status(reply):
    lines = reply.splitlines()
    waiting = next((line[len("waiting: "):].split(", ") for line in lines
                    if line.startswith("waiting: ")), [])
    first = lines[0] if lines else "unlocked"
    if first.startswith("held by "):
        match = re.fullmatch(r"held by (.+) for (.+) since (\d+)", first)
        if match:
            owner_name, purpose, acquired_at = match.groups()
            return {"state": "held", "owner": owner_name, "purpose": purpose,
                    "acquired_at": int(acquired_at), "waiting": waiting}
    if first.startswith("human reservation: "):
        match = re.fullmatch(r"human reservation: (.+?): (.*) \((\d+)s ago\)", first)
        if match:
            owner_name, note, age = match.groups()
            return {"state": "human", "owner": owner_name, "note": note,
                    "age_seconds": int(age), "waiting": waiting}
    if first == "unlocked":
        return {"state": "unlocked", "waiting": waiting}
    raise ValueError(f"unrecognized device status: {first}")


def release(args):
    """Release a lock this session holds, before its own command would have
    - the token comes from what that command printed when it acquired it."""
    if len(args) != 1:
        sys.exit("usage: autana release <token>")
    return subprocess.call(device_command("--owner", owner(),
                                          "release", "--token", args[0]))


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
    command = device_command("--owner", owner(), "hand-to-human",
                             "--note", " ".join(args), *wait)
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
    if args:
        sys.exit("usage: autana take-back")
    return subprocess.call(device_command("take-back"))


SUITE_REGISTRATION = re.compile(r"SUITE_REGISTER(_ON_REQUEST)?\(\s*([A-Za-z_]\w*)\s*\)")


def suite_list(args):
    """What this worktree registers, read from its sources: a suite names
    itself where it is defined and the board serves no listing verb, so there
    is nowhere else to ask. A name is runnable once a build carrying it is on
    the board - which variant and scope was flashed decides that, not this."""
    json_output = "--json" in args
    args = [arg for arg in args if arg != "--json"]
    if len(args) > 1:
        sys.exit("usage: autana suite list [text] [--json]")
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


def suite(args):
    """One registered suite, run where it can actually run. The name is the
    suite's own function, as SUITE_REGISTER() in its source spells it."""
    if not args:
        sys.exit("usage: autana suite <name> [seconds] | autana suite list [text]")
    if args[0] == "list":
        return suite_list(args[1:])
    name, rest = args[0], args[1:]
    rest = list(rest)
    verbose = "--verbose" in rest
    if verbose:
        rest.remove("--verbose")
    # A perf row can sit silent for minutes; the cap is how long to wait for
    # the whole suite, not how long a quiet stretch inside one may last.
    seconds = seconds_argument(rest, 600.0, "usage: autana suite <name> [seconds] [--verbose]")
    print(f"autana suite: {name}", flush=True)
    command = device_command(
        "--owner", owner(),
        "run-suite", name, "--max-seconds", str(seconds), "--purpose", f"autana suite {name}",
    )
    if verbose:
        command.append("--verbose")
    return subprocess.call(command)


# A console line is asked from a prompt somebody is sitting at: a board another
# owner holds is reported at once rather than waited for, which from the
# outside is a terminal that has hung. The few seconds cover a lock changing
# hands between the look and the send.
SEND_WAIT_S = 5


def board_holder():
    """'held by <owner> for ...' when someone else has the board, else ''.

    Every line of a console session is a device.py of its own under one
    autana, so a lock this autana already holds is not somebody else's and
    the session does not refuse itself."""
    mine = f"held by {owner()} "
    result = subprocess.run(device_command("status"), capture_output=True, text=True)
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
    holder = board_holder()
    if holder:
        print(f"the board is busy - {holder}\nnothing was sent; try again when it is free",
              file=sys.stderr)
        return 3, []
    command = device_command("--owner", owner(), "--wait", str(SEND_WAIT_S), "send", line,
                             "--purpose", purpose)
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
    """SCREENSHOT, captured and decoded by launcher/tools/screenshot.py's own
    read_screenshot()/write_capture(), under the device lock - see
    device.py's own `screenshot` subcommand."""
    out = None
    view = None
    while args:
        arg = args.pop(0)
        if arg in ("-o", "--out") and args and out is None:
            out = args.pop(0)
        elif arg in ("--as-shown", "--framebuffer") and view is None:
            view = arg
        else:
            sys.exit("usage: autana screenshot [--as-shown|--framebuffer] [-o PATH]")
    holder = board_holder()
    if holder:
        print(f"the board is busy - {holder}\nnothing was sent; try again when it is free",
              file=sys.stderr)
        return 3
    command = device_command("--owner", owner(), "screenshot",
                             "--purpose", "autana screenshot")
    if out:
        command += ["--out", out]
    if view:
        command.append(view)
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
    if len(args) not in (1, 2) or args[0] not in ("boot", "power") \
            or len(args) == 2 and args[1] not in ("short", "long"):
        sys.exit("usage: autana button <boot|power> [short|long]")
    code, replies = send("BUTTON " + " ".join(args), reply="BUTTON",
                         until=["BUTTON_OK", "BUTTON_ERR"], purpose="autana button")
    print("\n".join(replies))
    return code


def apps(args):
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
    if len(args) != 1:
        sys.exit("usage: autana open <name>")
    code, replies = send("OPEN " + args[0], reply="OPEN", purpose="autana open")
    print("\n".join(replies))
    return code


def home(args):
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
    json_output = "--json" in args
    args = [arg for arg in args if arg != "--json"]
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
                COMMANDS[verb](rest)
            else:
                replies = forward(line, verb)
                print("\n".join(replies) if replies else "sent")
        except SystemExit as stop:
            # a command's own refusal ends that command, not the session
            if stop.code not in (0, None):
                print(stop.code)


Command = namedtuple("Command", "name handler usages")

# (key, title, commands). `autana help <key>` shows one group; each usage is
# (synopsis without "autana ", one line of what it does).
COMMAND_GROUPS = (
    ("build", "Build and flash", (
        Command("flash", flash, (
            ("flash [rel|dev|diag] [--quiet] [--perf-scope]", "build and flash this worktree; dev when omitted"),)),
        Command("buildid", buildid, (
            ("buildid [--json]", "the BUILD_ID the board is running"),)),
    )),
    ("tests", "Tests", (
        Command("suite", suite, (
            ("suite <name> [seconds] [--verbose]", "run one registered suite on the board"),
            ("suite list [text] [--json]", "the suites this worktree registers"))),
        Command("selftest", selftest, (
            ("selftest [seconds] [--verbose]", "build diagnostics+autorun, run every suite on the board"),)),
        Command("batch", batch, (
            ("batch <suite>... [--runs N] [--perf-scope] [--verbose]",
             "flash once, capture the suites N times under one lock"),)),
    )),
    ("watch", "Watch the board", (
        Command("monitor", monitor, (
            ("monitor [seconds] [--follow] [--stream] [--elf PATH]", "the console live until Ctrl+C, or for N s"),)),
        Command("reset", reset, (
            ("reset [--capture [seconds]] [--verbose]", "reboot the board; --capture records the boot"),)),
        Command("screenshot", screenshot, (
            ("screenshot [--as-shown|--framebuffer] [-o PATH]", "the panel as PATH.png plus PATH.json"),)),
    )),
    ("input", "Drive input", (
        Command("tap", tap, (("tap <x> <y>", "tap a point"),)),
        Command("press", press, (("press <x> <y> [ms]", "hold a point; 1000 ms when omitted"),)),
        Command("drag", drag, (("drag <x0> <y0> <x1> <y1> <ms>", "drag between two points"),)),
        Command("touch", touch, (("touch <down|up> <x> <y>", "one raw touch level; up hands back"),)),
        Command("imu", imu, (
            ("imu <ax> <ay> <az>", "raw accelerometer counts"),
            ("imu release", "hand back to the sensor"))),
        Command("button", button, (("button <boot|power> [short|long]", "press a board button"),)),
    )),
    ("apps", "Apps", (
        Command("apps", apps, (("apps [--json]", "the registered apps, and which is running"),)),
        Command("open", open_app, (("open <name>", "enter an app; case-insensitive prefix"),)),
        Command("home", home, (("home", "back to the launcher"),)),
    )),
    ("frames", "Frame loop", (
        Command("freeze", freeze, (("freeze", "stop the frame loop"),)),
        Command("resume", resume, (("resume", "run it again"),)),
        Command("step", step, (("step [N]", "advance N frames while frozen; 1 when omitted"),)),
    )),
    ("tune", "Tunables", (
        Command("tune", tune, (
            ("tune [text] [--json]", "list the tunables, names containing text"),
            ("tune <name> [value]", "show one, or set it on the board"),
            ("tune reset <name>", "back to the value the source declares"),
            ("tune save", "write the board's values into this worktree's TUNE() lines"))),
    )),
    ("lock", "Sharing the board", (
        Command("status", status, (("status [--json]", "who holds the board, and who waits"),)),
        Command("id", identify, (("id [--json]", "the name this session holds the lock under"),)),
        Command("release", release, (("release <token>", "release a lock this session holds"),)),
        Command("hand", hand, (("hand [--wait <seconds>] <note...>",
                                 "reserve the board for a person at it"),)),
        Command("take-back", take_back, (("take-back", "clear that reservation"),)),
    )),
)

COMMANDS = {command.name: command.handler
            for _, _, commands in COMMAND_GROUPS for command in commands}
COMMANDS["console"] = console
HELP_TOPICS = [key for key, _, _ in COMMAND_GROUPS] + [
    command.name for _, _, commands in COMMAND_GROUPS for command in commands]
USAGE_WIDTH = 34


def help_text(args, prefix="autana "):
    """Every group, or the one group or command `args` names."""
    topic = args[0] if args else None
    lines = []
    for key, title, commands in COMMAND_GROUPS:
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
        if len(parts) != 2 or parts[0] not in ("flash", "help"):
            return []
        words = VARIANTS if parts[0] == "flash" else HELP_TOPICS
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
    sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]))


if __name__ == "__main__":
    main()
