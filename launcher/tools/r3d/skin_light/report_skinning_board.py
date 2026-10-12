"""Generate skinning budget tables from committed board captures."""
import argparse
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "render"))
from generated_blocks import replace_block

STAGES = (("animate", "1"), ("animate", "2"), ("skin", "1"), ("skin", "2"),
          ("animation_step", "unknown"))
PHASES = ("before_enter", "loaded", "playing")
RENDER_KEYS = (("render", "0"), ("render", "1"), ("render_delta", ""))

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
        elif "capybara_perf:" in line:
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
            for field in fields:
                try:
                    int(run[key][field])
                except (KeyError, ValueError) as error:
                    raise ValueError(f"run {number} {key} invalid {field}") from error
    vertices = {int(run[key]["vertices"]) for run in runs.values() for key in STAGES if key in run}
    samples = {int(run[key]["samples"]) for run in runs.values() for key in STAGES if key in run}
    if len(vertices) != 1 or min(vertices) <= 0 or len(samples) != 1 or min(samples) <= 0:
        raise ValueError("inconsistent vertices or samples")
    return {
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
    table = ["The skin stage's budget is about 1 Âµs per vertex. The board capture",
             "measures the animated subject playing, including runtime lighting.", "",
             "| Stage, per frame | One core | Two cores |", "|---|---:|---:|"]
    for stage, label in (("animate", "Animate: sample, blend, palette"), ("skin", "Skin and light")):
        cells = [f"{stages[(stage, core)]:g} Âµs ({costs[(stage, core)]:.3f} Âµs / vertex)" for core in ("1", "2")]
        table.append(f"| {label} | {' | '.join(cells)} |")
    step = ("animation_step", "unknown")
    table.append(f"| Whole animation step (cores not recorded) | {stages[step]:g} Âµs ({costs[step]:.3f} Âµs / vertex) | |")
    playing, paused, delta = data["render"]
    table.append(f"| Scene render, playing vs paused | | +{delta:g} Âµs ({playing:g} vs {paused:g} Âµs) |")
    for key, count in data["stage_counts"].items():
        if count < data["run_count"]:
            table.extend(("", f"{key[0]}, {key[1]} core(s): {count} of {data['run_count']} runs."))
    for (stage, slot), count in zip(RENDER_KEYS, data["render_counts"]):
        if count < data["run_count"]:
            table.extend(("", f"{stage}, paused={slot}: {count} of {data['run_count']} runs."))
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
