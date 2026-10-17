"""Discover and exercise a running shell through send()/lines(seconds)."""
from dataclasses import dataclass
from functools import cache
import itertools
from pathlib import Path
import re
import sys
import time

from device_capture import BOOTS_ON_RUNNING, BUILD_ID_RE, BUILD_ID_REQUEST, crash_signs

ROOT = Path(__file__).resolve().parents[2]
HEARTBEAT_S = 15
DEFAULT_TOUR_S = 1800
APPS_RE = re.compile(r"APPS name=(.+?) running=([01])(?: console=(\S+))?")
ERROR_RE = re.compile(r"^\S+_ERR(?:\s|$)")


@cache
def quarter_tilts():
    """Invert the firmware's screen axes for each viewport's down vector."""
    sys.path.insert(0, str(ROOT / "launcher/tools/build"))
    from c_header import read_define
    source = (ROOT / "launcher/main/display/viewport.h").read_text(encoding="utf-8")
    rows = re.findall(r"(?:case (\d+)|default): return \(viewport_quarter_axes_t\){([^}]+)};", source)
    axes = {int(index or 0): tuple(int(v) for v in values.split(",")) for index, values in rows}
    source = ROOT / "launcher/main/input/imu_sample.h"
    one_g = read_define(source, "IMU_COUNTS_PER_G")
    mapping = re.search(r"return \(vec3i_t\){([^}]+)};", source.read_text(encoding="utf-8"))
    if not axes or mapping is None:
        raise RuntimeError("cannot read firmware quarter/IMU axes")
    screen_axes = [part.strip() for part in mapping[1].split(",")]
    tilts = []
    for quarter in range(len(axes)):
        _, x_down, _, y_down = axes[quarter]
        raw = {}
        for expression, value in zip(screen_axes, (x_down, y_down, 0)):
            raw[expression.lstrip("-")] = (-value if expression.startswith("-") else value) * one_g
        tilts.append("IMU " + " ".join(str(raw[axis]) for axis in ("x", "y", "z")))
    return tuple(tilts)


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


def parse_apps(lines):
    rows = []
    for line in lines:
        match = APPS_RE.fullmatch(line)
        if match:
            row = {"name": match[1], "running": match[2] == "1"}
            if match[3]:
                row["console"] = match[3]
            rows.append(row)
        elif line.startswith("APPS") and line != "APPS_END":
            raise TourError("malformed APPS reply: " + line)
    if len({row["name"] for row in rows}) != len(rows):
        raise TourError("duplicate APPS name")
    return rows


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


def plan(apps, tilts, first_quarter=0):
    if not apps:
        raise TourError("APPS listed no apps")
    if not any(len(app.scenes) >= 2 for app in apps):
        raise TourError("no scene pairs found: no app has two or more scenes")
    steps = []

    def add(command, reply):
        steps.append(Step(command, reply, tilts[(first_quarter + len(steps)) % len(tilts)]))

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


def heartbeat(console):
    console.send(BUILD_ID_REQUEST)
    for line in console.lines(HEARTBEAT_S):
        if ERROR_RE.match(line):
            return False
        if BUILD_ID_RE.search(line):
            return True
    return False


def exchange(console, step, *, decline=False):
    console.send(step.command)
    if step.tilt:
        console.send(step.tilt)
    replies = []
    if step.reply:
        for line in console.lines(HEARTBEAT_S):
            if ERROR_RE.match(line):
                if decline and line == step.reply + "_ERR not handled":
                    replies.append(line)
                    break
                raise TourError(line)
            if line.startswith((step.reply + " ", step.reply + "_")):
                replies.append(line)
                if line == step.reply + "_END" or re.match(re.escape(step.reply) + r"_OK(?:\s|$)", line):
                    break
        else:
            raise TourError(f"no completed reply to {step.command!r}; requires a development image")
    if not heartbeat(console):
        raise TourError("missing BUILDID heartbeat after " + step.command)
    return replies


def discover(console, emit=print):
    tilts = quarter_tilts()
    emit("discover: APPS")
    rows = parse_apps(exchange(console, Step("APPS", "APPS")))
    if not rows:
        raise TourError("APPS listed no apps")
    apps = []
    quarter = 0
    for row in rows:
        prefix = row.get("console")
        keys = ()
        if prefix:
            emit("discover: OPEN " + row["name"])
            exchange(console, Step("OPEN " + row["name"], "OPEN", tilts[quarter % len(tilts)]))
            emit("discover: " + prefix + " scenes")
            keys = parse_scenes(exchange(console, Step(prefix + " scenes", prefix.upper()), decline=True), prefix)
            emit("discover: HOME")
            exchange(console, Step("HOME", "HOME", tilts[(quarter + 1) % len(tilts)]))
            quarter += 2
        apps.append(App(row["name"], prefix, keys))
    return apps


class CheckedTransport:
    """Crash checks use the capture owner's verdict, including a single reboot."""
    def __init__(self, console):
        self.console = console

    def send(self, line):
        self.console.send(line)

    def lines(self, seconds=None):
        for line in self.console.lines(seconds):
            signs = crash_signs(line, BOOTS_ON_RUNNING)
            if signs:
                raise TourError("; ".join(signs))
            yield line


def run(console, emit=print):
    started = time.monotonic()
    console = CheckedTransport(console)
    apps = []
    switches = 0
    ok = False
    try:
        apps = discover(console, emit)
        steps = plan(apps, quarter_tilts(), first_quarter=2 * sum(bool(app.console) for app in apps))
        for index, step in enumerate(steps, 1):
            emit(f"tour {index}/{len(steps)}: {step.command}" + (f"; {step.tilt}" if step.tilt else ""))
            exchange(console, step)
        switches = sum(len(app.scenes) * (len(app.scenes) - 1) for app in apps)
        ok = True
    except TourError as error:
        emit("tour FAIL: " + str(error))
        ok = False
    finally:
        if not ok:
            console.send("IMU release")
    emit(f"tour {'PASS' if ok else 'FAIL'}: apps={len(apps)} scenes={sum(max(1, len(app.scenes)) for app in apps)} "
         f"switches={switches} elapsed={time.monotonic() - started:.1f}s")
    return ok
