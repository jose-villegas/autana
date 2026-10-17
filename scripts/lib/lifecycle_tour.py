"""Discover and exercise a running shell through send()/lines(seconds)."""
from dataclasses import dataclass
import itertools
import re
import time

from device_capture import BOOTS_ON_RUNNING, BUILD_ID_RE, BUILD_ID_REQUEST, crash_signs, parse_apps, IMU_QUARTER_RE
from console_protocol import replies_to, require_development

MILLISECONDS_PER_SECOND = 1000
HEARTBEAT_S = 15
DEFAULT_TOUR_S = 1800
ERROR_RE = re.compile(r"\b\w+_ERR(?:\s|$)")


class TourError(RuntimeError):
    """A graph or console contract could not be verified."""


@dataclass(frozen=True)
class App:
    name: str
    console: str | None = None
    scenes: tuple = ()


@dataclass(frozen=True)
class Step:
    command: str
    reply: str | None = None
    tilt: str | None = None


def parse_scenes(lines, prefix):
    tag = prefix.upper()
    if lines == [tag + "_ERR not handled"]:
        return ()
    if not lines or lines[-1] != tag + "_END":
        raise TourError(f"{prefix} scenes: missing {tag}_END")
    keys = []
    for line in lines[:-1]:
        match = re.fullmatch(re.escape(tag) + r" scene=(\S+)(?: .*)?", line)
        if not match or match[1] in keys:
            raise TourError(f"{prefix} scenes: malformed or duplicate scene: {line}")
        keys.append(match[1])
    if not keys:
        raise TourError(f"{prefix} scenes: empty list")
    return tuple(keys)


def scene_walk(keys):
    remaining = {key: [other for other in reversed(keys) if other != key] for key in keys}
    stack, circuit = [keys[0]], []
    while stack:
        edges = remaining[stack[-1]]
        if edges:
            stack.append(edges.pop())
        else:
            circuit.append(stack.pop())
    return tuple(reversed(circuit))


def plan(apps, tilts):
    if not apps:
        raise TourError("APPS listed no apps")
    if not any(len(app.scenes) >= 2 for app in apps):
        raise TourError("no scene pairs found: no app has two or more scenes")
    steps = []

    def add(command, reply):
        steps.append(Step(command, reply, next(tilts)))

    for app in apps:
        add("OPEN " + app.name, "OPEN")
        add("HOME", "HOME")
    for a, b in itertools.permutations(apps, 2):
        add("OPEN " + a.name, "OPEN")
        add("OPEN " + b.name, "OPEN")
    for app in apps:
        if len(app.scenes) < 2:
            continue
        add("OPEN " + app.name, "OPEN")
        for key in scene_walk(app.scenes):
            add(f"{app.console} scene {key}", app.console.upper())
        add("HOME", "HOME")
    steps.append(Step("IMU release"))
    return steps


def heartbeat(console, *, on_line=None):
    console.send(BUILD_ID_REQUEST)
    for line in console.lines(HEARTBEAT_S):
        if on_line is not None:
            on_line(line)
        if BUILD_ID_RE.search(line):
            return True
    return False


def exchange(console, step, *, decline=False):
    console.send(step.command)
    if step.tilt:
        console.send(step.tilt)
    tilted_at = time.monotonic()
    replies = []
    complete = step.reply is None
    tilt_complete = step.tilt is None
    sample_s = 0
    if not complete or not tilt_complete:
        for line in console.lines(HEARTBEAT_S):
            error = ERROR_RE.search(line)
            if error and not (decline and line[error.start():] == step.reply + "_ERR not handled"):
                raise TourError(line[error.start():])
            try:
                require_development(line.encode(), step.command)
            except RuntimeError as error:
                raise TourError(str(error)) from error
            if not tilt_complete:
                found, _ = replies_to(line.encode(), "IMU", ("IMU_OK", "IMU_ERR"))
                if found:
                    match = IMU_QUARTER_RE.fullmatch(found[-1])
                    if not match or int(match[1]) != int(step.tilt.split()[-1]):
                        raise TourError("invalid quarter reply: " + found[-1])
                    sample_s = int(match[3]) / MILLISECONDS_PER_SECOND
                    tilted_at = time.monotonic()
                    tilt_complete = True
            if step.reply:
                found, ended = replies_to(line.encode(), step.reply,
                                         (step.reply + "_END", step.reply + "_OK", step.reply + "_ERR"))
                for reply in found:
                    if ERROR_RE.match(reply):
                        if not (decline and reply == step.reply + "_ERR not handled"):
                            raise TourError(reply)
                    replies.append(reply)
                complete = complete or ended
            if complete and tilt_complete:
                break
        else:
            raise TourError(f"no completed reply to {step.command!r} within the reply deadline")
    parts = step.command.split()
    if len(parts) == 3 and parts[1] == "scene":
        barrier = exchange(console, Step(parts[0] + " scenes", step.reply))
        current = [line.split()[1].removeprefix("scene=") for line in barrier
                   if "current=1" in line.split()]
        if current != [parts[2]]:
            raise TourError(f"{step.command}: scene switch did not land (current={current})")
    def check_error(line):
        if error := ERROR_RE.search(line):
            raise TourError(line[error.start():])

    if not heartbeat(console, on_line=check_error):
        raise TourError("missing BUILDID heartbeat after " + step.command)
    remaining = sample_s - (time.monotonic() - tilted_at)
    if remaining > 0:
        time.sleep(remaining)
    return replies


def discover(console, tilts, emit=print):
    emit("discover: APPS")
    rows = parse_apps(exchange(console, Step("APPS", "APPS")))
    if not rows:
        raise TourError("APPS listed no apps")
    apps = []
    for row in rows:
        prefix = row.get("console")
        keys = ()
        if prefix:
            emit("discover: OPEN " + row["name"])
            exchange(console, Step("OPEN " + row["name"], "OPEN", next(tilts)))
            emit("discover: " + prefix + " scenes")
            keys = parse_scenes(exchange(console, Step(prefix + " scenes", prefix.upper()), decline=True), prefix)
            emit("discover: HOME")
            exchange(console, Step("HOME", "HOME", next(tilts)))
        apps.append(App(row["name"], prefix, keys))
    return apps


class CheckedTransport:
    """Crash checks use the capture owner's verdict, including a single reboot."""
    def __init__(self, console):
        self.console = console

    def send(self, line):
        if time.monotonic() >= getattr(self.console, "deadline", float("inf")):
            raise TourError("the tour's time ran out")
        try:
            self.console.send(line)
        except OSError as error:
            raise TourError("port lost: the board reset or rebooted") from error

    def lines(self, seconds=None):
        try:
            for line in self.console.lines(seconds):
                signs = crash_signs(line, BOOTS_ON_RUNNING)
                if signs:
                    raise TourError("firmware crash or reboot detected (see device lines)")
                yield line
        except OSError as error:
            raise TourError("port lost: the board reset or rebooted") from error
        if time.monotonic() >= getattr(self.console, "deadline", float("inf")):
            raise TourError("the tour's time ran out")


def run(console, emit=print):
    started = time.monotonic()
    console = CheckedTransport(console)
    apps = []
    switches = 0
    ok = False
    try:
        reply = exchange(console, Step("IMU quarter 0", "IMU"))
        match = IMU_QUARTER_RE.fullmatch(reply[-1])
        if not match or int(match[1]) != 0 or int(match[2]) <= 0 or int(match[3]) <= 0:
            raise TourError("invalid IMU quarter capabilities")
        tilts = itertools.cycle(f"IMU quarter {q}" for q in range(int(match[2])))
        next(tilts)  # The capability probe already injected the first quarter.
        apps = discover(console, tilts, emit)
        steps = plan(apps, tilts)
        for index, step in enumerate(steps, 1):
            emit(f"tour {index}/{len(steps)}: {step.command}" + (f"; {step.tilt}" if step.tilt else ""))
            exchange(console, step)
        switches = sum(len(app.scenes) * (len(app.scenes) - 1) for app in apps)
        ok = True
    except (TourError, ValueError) as error:
        emit("tour FAIL: " + str(error))
        ok = False
    finally:
        if not ok:
            try:
                console.console.send("IMU release")
            except OSError:
                pass
    emit(f"tour {'PASS' if ok else 'FAIL'}: apps={len(apps)} scenes={sum(max(1, len(app.scenes)) for app in apps)} "
         f"steps={len(steps) if ok else 0} switches={switches} elapsed={time.monotonic() - started:.1f}s")
    return ok
