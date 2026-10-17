"""Generate skinning budget tables from committed board captures."""
import argparse
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools" / "render"))
from build.c_header import read_define
from generated_blocks import replace_block
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools" / "perf"))
from capture import build_id, QUARTERS, quarter_name
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[5] / "scripts/device"))
from device_report import BUDGET_TABLE_HEADER

LOG_HEADER = pathlib.Path(__file__).resolve().parents[1] / "tests/skin_perf_log.h"
PREFIX = read_define(LOG_HEADER, "SKIN_PERF_PREFIX")
BUDGET_NS_PER_VERTEX = read_define(LOG_HEADER, "SKIN_PERF_BUDGET_NS_PER_VERTEX")
NANOSECONDS_PER_MICROSECOND = read_define(LOG_HEADER, "SKIN_PERF_NS_PER_US")
CORES_ONE = read_define(LOG_HEADER, "SKIN_PERF_CORES_ONE")
CORES_TWO = read_define(LOG_HEADER, "SKIN_PERF_CORES_TWO")
CORES_UNKNOWN = read_define(LOG_HEADER, "SKIN_PERF_CORES_UNKNOWN")
PERCENT_SCALE = read_define(LOG_HEADER, "SKIN_PERF_PERCENT_SCALE")
SKIN_HEADER = LOG_HEADER.parents[3] / "render/r3d_skin.h"
LIGHTING_MODES = {read_define(SKIN_HEADER, "R3D_SKIN_LIGHT_" + mode.upper()): mode
                  for mode in ("direct", "renormalised", "table")}
SETUP_FIELDS = tuple(re.findall(r"(\w+)=%d", read_define(LOG_HEADER, "SKIN_PERF_SETUP_FORMAT")))
PHASE_BEFORE_ENTER = read_define(LOG_HEADER, "SKIN_PERF_PHASE_BEFORE_ENTER")
PHASE_LOADED = read_define(LOG_HEADER, "SKIN_PERF_PHASE_LOADED")
PHASE_PLAYING = read_define(LOG_HEADER, "SKIN_PERF_PHASE_PLAYING")
STAGE_ANIMATE, STAGE_SKIN, STAGE_ANIMATION_STEP, STAGE_RENDER, STAGE_RENDER_DELTA, STAGE_FRAME, STAGE_FRAME_DELTA = (
    read_define(LOG_HEADER, "SKIN_PERF_STAGE_" + name)
    for name in ("ANIMATE", "SKIN", "ANIMATION_STEP", "RENDER", "RENDER_DELTA", "FRAME", "FRAME_DELTA"))

STAGES = ((STAGE_ANIMATE, CORES_ONE), (STAGE_ANIMATE, CORES_TWO), (STAGE_SKIN, CORES_ONE), (STAGE_SKIN, CORES_TWO),
          (STAGE_ANIMATION_STEP, CORES_UNKNOWN))
PHASES = (PHASE_BEFORE_ENTER, PHASE_LOADED, PHASE_PLAYING)
RENDER_KEYS = ((STAGE_RENDER, "0"), (STAGE_RENDER, "1"), (STAGE_RENDER_DELTA, ""))
FRAME_KEYS = ((STAGE_FRAME, "0"), (STAGE_FRAME, "1"), (STAGE_FRAME_DELTA, ""))

def parse_capture(text, quarter="portrait"):
    if quarter not in QUARTERS:
        raise ValueError(f"invalid render quarter {quarter}")
    observed = {quarter_name(line) for line in text.splitlines()
                if f"stage={STAGE_RENDER} " in line and quarter_name(line)}
    if observed and set(QUARTERS) != observed:
        raise ValueError("capture missing render quarter " + ", ".join(sorted(set(QUARTERS) - observed)))
    if not observed and quarter != "portrait":
        raise ValueError(f"capture has no quarter={quarter} rows")
    render_stages = {STAGE_RENDER, STAGE_FRAME, STAGE_RENDER_DELTA, STAGE_FRAME_DELTA}
    text = "\n".join(line for line in text.splitlines()
                     if not any(f"stage={stage} " in line for stage in render_stages)
                     or quarter_name(line) in (None, quarter))
    build = build_id(text)
    runs = {}
    current = None
    for line in text.splitlines():
        if line.startswith("RUN="):
            number = line.partition("=")[2]
            if number in runs:
                raise ValueError(f"duplicate run {number}")
            current = runs[number] = {}
        elif PREFIX in line:
            if current is None:
                raise ValueError("measurement has no run")
            fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
            if "phase" in fields:
                current[("phase", fields["phase"])] = fields
            elif "stage" in fields:
                current[(fields["stage"], fields.get("cores", fields.get("paused", "")))] = fields
    if not runs:
        raise ValueError("capture has no runs")
    required = (*STAGES, *RENDER_KEYS, *(FRAME_KEYS if observed else ()), *(("phase", phase) for phase in PHASES))
    for key in required:
        if not any(key in run for run in runs.values()):
            raise ValueError(f"capture missing {' '.join(key)}")
    for number, run in runs.items():
        for key in required:
            if key not in run:
                continue
            fields = ("internal_free", "internal_largest") if key[0] == "phase" else ("mean_us",)
            if key in STAGES:
                fields += ("vertices", "samples")
            if key in STAGES and key[1] == CORES_TWO:
                fields += ("worker_samples",)
            if key[0] == STAGE_SKIN:
                fields += SETUP_FIELDS
            for field in fields:
                try:
                    int(run[key][field])
                except (KeyError, ValueError) as error:
                    raise ValueError(f"run {number} {key} invalid {field}") from error
            if key in STAGES and key[1] == CORES_TWO:
                w, s = int(run[key]["worker_samples"]), int(run[key]["samples"])
                if w != s:
                    raise ValueError(f"run {number} {key} ran {w} of {s} samples on the worker")
    vertices = {int(run[key]["vertices"]) for run in runs.values() for key in STAGES if key in run}
    samples = {int(run[key]["samples"]) for run in runs.values() for key in STAGES if key in run}
    if len(vertices) != 1 or min(vertices) <= 0 or len(samples) != 1 or min(samples) <= 0:
        raise ValueError("inconsistent vertices or samples")
    setup = {}
    for field in SETUP_FIELDS:
        values = {int(run[key][field]) for run in runs.values() for key in STAGES
                  if key[0] == STAGE_SKIN and key in run}
        if len(values) != 1:
            raise ValueError(f"inconsistent {field}")
        setup[field] = values.pop()
    for field in ("joints", "influences", "lights"):
        if setup[field] < 0 or (field != "lights" and setup[field] == 0):
            raise ValueError(f"invalid {field}")
    if setup["lighting"] not in LIGHTING_MODES:
        raise ValueError("invalid lighting")
    frames = tuple(statistics.median(int(run[key]["mean_us"]) for run in runs.values() if key in run)
                   for key in FRAME_KEYS) if all(any(key in run for run in runs.values()) for key in FRAME_KEYS) else None
    return {
        "frames": frames,
        "frame_counts": tuple(sum(key in run for run in runs.values()) for key in FRAME_KEYS),
        "setup": setup,
        "build": build, "run_count": len(runs), "vertices": vertices.pop(), "samples": samples.pop(),
        "stages": {key: statistics.median(int(run[key]["mean_us"]) for run in runs.values() if key in run) for key in STAGES},
        "per_vertex": {key: statistics.median(float(run[key].get("us_per_vertex",
                                    int(run[key]["mean_us"]) / int(run[key]["vertices"])))
                                             for run in runs.values() if key in run) for key in STAGES},
        "phases": {phase: tuple(statistics.median(int(run[("phase", phase)][field]) for run in runs.values() if ("phase", phase) in run)
                                for field in ("internal_free", "internal_largest")) for phase in PHASES},
        "stage_counts": {key: sum(key in run for run in runs.values()) for key in STAGES},
        "render_counts": tuple(sum(key in run for run in runs.values()) for key in RENDER_KEYS),
        "render": tuple(statistics.median(int(run[key]["mean_us"]) for run in runs.values() if key in run) for key in RENDER_KEYS),
    }


def budget(text, repeat="autana suite run_capybara_perf_suite --flash --perf-scope"):
    data = parse_capture(text)
    stages, costs = data["stages"], data["per_vertex"]
    limit = BUDGET_NS_PER_VERTEX / NANOSECONDS_PER_MICROSECOND
    setup = data["setup"]
    table = [f"The skin stage's budget is {limit:g} µs per vertex. The board capture",
             f"measures the animated subject playing: {data['vertices']} vertices, {setup['joints']} joints,",
             f"{setup['influences']} influences, {LIGHTING_MODES[setup['lighting']]} lighting, "
             f"{setup['lights']} light{'s' if setup['lights'] != 1 else ''}.", "",
             "| Stage, per frame | One core | Two cores |", "|---|---:|---:|"]
    for stage, label in ((STAGE_ANIMATE, "Animate: sample, blend, palette"), (STAGE_SKIN, "Skin and light")):
        cells = [f"{stages[(stage, core)]:g} µs ({costs[(stage, core)]:.3f} µs / vertex)" for core in (CORES_ONE, CORES_TWO)]
        table.append(f"| {label} | {' | '.join(cells)} |")
    step = (STAGE_ANIMATION_STEP, CORES_UNKNOWN)
    table.extend(("", "| As a frame runs it (worker use not recorded) | Per frame |", "|---|---:|"))
    table.append(f"| Whole animation step | {stages[step]:g} µs ({costs[step]:.3f} µs / vertex) |")
    quarters = [q for q in QUARTERS if any(quarter_name(line) == q and "paused=" in line
                                         for line in text.splitlines())]
    for quarter in quarters or [None]:
        rendered = parse_capture(text, quarter or "portrait")
        playing, paused, _ = rendered["render"]
        delta = playing - paused
        label = f" quarter={quarter}" if quarter else ""
        table.append(f"| Scene render{label}, playing minus paused | {delta:+g} µs ({playing:g} vs {paused:g} µs) |")
        if quarter is not None and rendered["frames"] is not None:
            playing, paused, _ = rendered["frames"]
            table.append(f"| Full frame{label}, playing minus paused | {playing-paused:+g} µs ({playing:g} vs {paused:g} µs) |")
        counts = list(zip(RENDER_KEYS, rendered["render_counts"]))
        if quarter is not None:
            counts.extend(zip(FRAME_KEYS, rendered["frame_counts"]))
        for (stage, slot), count in counts:
            if count < data["run_count"]:
                table.extend(("", f"{stage}{label}, paused={slot}: {count} of {data['run_count']} runs."))
    for key, count in data["stage_counts"].items():
        if count < data["run_count"]:
            table.extend(("", f"{key[0]}, {key[1]} core(s): {count} of {data['run_count']} runs."))
    within = [costs[(STAGE_SKIN, core)] <= limit for core in (CORES_ONE, CORES_TWO)]
    if all(within):
        verdict = "within budget on both cores"
    elif not any(within):
        verdict = "above budget on both cores"
    elif within[1]:
        verdict = "within budget on two cores; above budget on one core"
    else:
        verdict = "within budget on one core; above budget on two cores"
    table.extend(("", f"Skin and light: {verdict} (budget {limit:g} µs / vertex)."))
    table.extend(("", "| Internal RAM | Free / largest block, bytes |", "|---|---:|"))
    for phase, label in zip(PHASES, ("Before the scene loads", "Scene loaded", "Scene playing")):
        free, largest = data["phases"][phase]
        table.append(f"| {label} | {free:g} / {largest:g} |")
    table.extend(("", f"Build `{data['build']}`; {data['run_count']} runs, {data['samples']} samples per stage,",
                  f"{data['vertices']} vertices. Values are medians of each run's last measurement.", "",
                  f"To repeat: `{repeat}`."))
    return "\n".join(table)


def performance_report(text):
    if not re.search(r"^RUN=", text, re.MULTILINE):
        text = "RUN=1\n" + text
    data = parse_capture(text)
    goal = data["vertices"] * BUDGET_NS_PER_VERTEX / NANOSECONDS_PER_MICROSECOND
    lines = [BUDGET_TABLE_HEADER, "|---|---:|---:|---:|---|"]
    for core in (CORES_ONE, CORES_TWO):
        measured = data["stages"][(STAGE_SKIN, core)]
        headroom = (goal - measured) * PERCENT_SCALE / goal
        status = "MET" if measured <= goal else "UNMET"
        lines.append(f"| Skin, {core} core(s) | {goal:g} | {measured:g} | {headroom:+.1f}% | {status} |")
    return "\n".join(lines) + "\n\n" + budget(text) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("document", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--repeat", default="autana suite run_capybara_perf_suite --flash --perf-scope")
    args = parser.parse_args()
    try:
        text = args.capture.read_text(encoding="utf-8")
        if args.source is not None:
            args.document.write_text(performance_report(text), encoding="utf-8")
            return 0
        body = budget(text, args.repeat)
        command = (f"python launcher/main/apps/render_lab/tools/{pathlib.Path(__file__).name} "
                   f"{args.capture.as_posix()} {args.document.as_posix()} --check")
        changed = replace_block(args.document, "skinning-budget", body, check=args.check, command=command)
        if args.check and changed:
            print(f"{args.document}: skinning-budget differs; run the generator")
        return int(args.check and changed)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
