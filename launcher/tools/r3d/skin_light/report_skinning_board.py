"""Generate skinning budget tables from committed board captures."""
import argparse
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "render"))
from build.c_header import read_define
from generated_blocks import replace_block

LOG_HEADER = pathlib.Path(__file__).resolve().parents[3] / "main/apps/render_lab/tests/capybara_perf_log.h"
PREFIX = read_define(LOG_HEADER, "CAPYBARA_PERF_PREFIX")
BUDGET_NS_PER_VERTEX = read_define(LOG_HEADER, "CAPYBARA_PERF_BUDGET_NS_PER_VERTEX")
NANOSECONDS_PER_MICROSECOND = 1000
SKIN_HEADER = LOG_HEADER.parents[3] / "render/r3d_skin.h"
LIGHTING_MODES = {read_define(SKIN_HEADER, "R3D_SKIN_LIGHT_" + mode.upper()): mode
                  for mode in ("direct", "renormalised", "table")}
SETUP_FIELDS = tuple(re.findall(r"(\w+)=%d", read_define(LOG_HEADER, "CAPYBARA_PERF_SETUP_FORMAT")))
PHASE_BEFORE_ENTER = read_define(LOG_HEADER, "CAPYBARA_PERF_PHASE_BEFORE_ENTER")
PHASE_LOADED = read_define(LOG_HEADER, "CAPYBARA_PERF_PHASE_LOADED")
PHASE_PLAYING = read_define(LOG_HEADER, "CAPYBARA_PERF_PHASE_PLAYING")
STAGE_ANIMATE = read_define(LOG_HEADER, "CAPYBARA_PERF_STAGE_ANIMATE")
STAGE_SKIN = read_define(LOG_HEADER, "CAPYBARA_PERF_STAGE_SKIN")
STAGE_ANIMATION_STEP = read_define(LOG_HEADER, "CAPYBARA_PERF_STAGE_ANIMATION_STEP")
STAGE_RENDER = read_define(LOG_HEADER, "CAPYBARA_PERF_STAGE_RENDER")
STAGE_RENDER_DELTA = read_define(LOG_HEADER, "CAPYBARA_PERF_STAGE_RENDER_DELTA")

STAGES = ((STAGE_ANIMATE, "1"), (STAGE_ANIMATE, "2"), (STAGE_SKIN, "1"), (STAGE_SKIN, "2"),
          (STAGE_ANIMATION_STEP, "unknown"))
PHASES = (PHASE_BEFORE_ENTER, PHASE_LOADED, PHASE_PLAYING)
RENDER_KEYS = ((STAGE_RENDER, "0"), (STAGE_RENDER, "1"), (STAGE_RENDER_DELTA, ""))

def parse_capture(text):
    build = ""
    runs = {}
    current = None
    for line in text.splitlines():
        if line.startswith("BUILD_ID="):
            found = line.partition("=")[2]
            if build and found != build:
                raise ValueError("capture has mismatched build ids")
            build = found
        elif line.startswith("RUN="):
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
    if not build:
        raise ValueError("capture has no build id")
    if not runs:
        raise ValueError("capture has no runs")
    required = (*STAGES, *RENDER_KEYS, *(("phase", phase) for phase in PHASES))
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
            if key[0] == STAGE_SKIN:
                fields += SETUP_FIELDS
            for field in fields:
                try:
                    int(run[key][field])
                except (KeyError, ValueError) as error:
                    raise ValueError(f"run {number} {key} invalid {field}") from error
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
    if setup["joints"] <= 0 or setup["influences"] <= 0 or setup["lights"] < 0:
        raise ValueError("invalid skin setup")
    if setup["lighting"] not in LIGHTING_MODES:
        raise ValueError("invalid lighting")
    return {
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


def budget(text):
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
        cells = [f"{stages[(stage, core)]:g} µs ({costs[(stage, core)]:.3f} µs / vertex)" for core in ("1", "2")]
        table.append(f"| {label} | {' | '.join(cells)} |")
    step = (STAGE_ANIMATION_STEP, "unknown")
    table.append(f"| Whole animation step (cores not recorded) | {stages[step]:g} µs ({costs[step]:.3f} µs / vertex) | |")
    playing, paused, delta = data["render"]
    table.append(f"| Scene render, playing vs paused | | +{delta:g} µs ({playing:g} vs {paused:g} µs) |")
    for key, count in data["stage_counts"].items():
        if count < data["run_count"]:
            table.extend(("", f"{key[0]}, {key[1]} core(s): {count} of {data['run_count']} runs."))
    for (stage, slot), count in zip(RENDER_KEYS, data["render_counts"]):
        if count < data["run_count"]:
            table.extend(("", f"{stage}, paused={slot}: {count} of {data['run_count']} runs."))
    within = [costs[(STAGE_SKIN, core)] <= limit for core in ("1", "2")]
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
                  "To repeat: `autana flash diag --perf-scope`, then",
                  "`autana suite run_capybara_perf_suite`. Skinning splits by cluster;",
                  "animating one character gains nothing from the split."))
    return "\n".join(table)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("document", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        body = budget(args.capture.read_text(encoding="utf-8"))
        command = (f"python launcher/tools/r3d/skin_light/{pathlib.Path(__file__).name} "
                   f"{args.capture.as_posix()} {args.document.as_posix()} --check")
        changed = replace_block(args.document, "skinning-budget", body, check=args.check, command=command)
        if args.check and changed:
            print(f"{args.document}: skinning-budget differs; run the generator")
        return int(args.check and changed)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
