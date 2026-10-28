"""Generate the water scene's stage tables from a committed board capture of run_water_perf_suite."""
import argparse
import pathlib
import re
import shlex
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
TOOLS = ROOT / "launcher/tools"
sys.path[:0] = [str(TOOLS), str(TOOLS / "perf"), str(TOOLS / "render")]
from build.c_header import read_define
from capture import build_id
from frame_cost import US_PER_MS, frame_cost_window
from generated_blocks import replace_block

SCRIPT = pathlib.Path(__file__).resolve().relative_to(ROOT).as_posix()
DOCUMENT = "launcher/main/apps/render_lab/tools/README.md"
CAPTURE = "docs/render/data/water-stages-board.log"
BLOCK = "water-stages"
LOG_HEADER = pathlib.Path(__file__).resolve().parents[1] / "tests/water_perf_log.h"
PREFIX = read_define(LOG_HEADER, "WATER_PERF_PREFIX")
FRAME_MS = read_define(LOG_HEADER, "WATER_PERF_FRAME_MS")
SCENE = read_define(LOG_HEADER, "WATER_PERF_STAGES_FROM")
UPDATE = read_define(LOG_HEADER, "WATER_PERF_UPDATE")
BEFORE, RUNNING, AFTER = (read_define(LOG_HEADER, "WATER_PERF_" + name) for name in ("BEFORE", "RUNNING", "AFTER"))
BASE = "base"
RUN_MARK = "RUN="
FIELD = re.compile(r"(\w+)=(\S+)")
TRIPLE = ("mean", "p95", "worst")
R3D = "r3d."
BYTES_PER_KB = 1024


def runs(text):
    """The capture split at its RUN= lines; one run when it has none."""
    parts, current = [], []
    for line in text.splitlines():
        if line.startswith(RUN_MARK):
            if current:
                parts.append(current)
            current = []
        else:
            current.append(line)
    parts.append(current)
    return [part for part in parts if any(PREFIX in line for line in part)]


def parse(lines):
    """Rows by (config, phase), with their stages and r3d brackets, and heap readings by (config, phase)."""
    rows, heaps = {}, {}
    for line in lines:
        if PREFIX not in line:
            continue
        body = line.split(PREFIX, 1)[1]
        window = frame_cost_window(body)
        fields = dict(FIELD.findall(body.split("ms/frame", 1)[0]))
        if "internal_free" in fields:
            heaps[fields["config"], fields["phase"]] = (int(fields["internal_free"]), int(fields["internal_largest"]))
            continue
        row = rows.setdefault((fields["config"], fields["row"]), {"stages": {}, "scene": {}, "r3d": {}})
        if "frames" in fields and "frames" in row:
            raise ValueError(f"{fields['config']} {fields['row']} twice in one run: start each run with a {RUN_MARK} line")
        if window is not None:
            brackets = window["brackets"].items()
            row["scene"] = {name: avg for name, (avg, _) in brackets if name.startswith(SCENE) and name != UPDATE}
            row["r3d"] = {name: avg for name, (avg, _) in brackets if name.startswith(R3D)}
        elif "stage" in fields:
            row["stages"][fields["stage"]] = int(fields["us"]) / US_PER_MS
        elif "frames" in fields:
            row.update(frames=int(fields["frames"]), substeps=int(fields["substeps"]),
                       remeshed=int(fields["remeshed"]), tris=int(fields["tris"]), particles=int(fields["particles"]))
        else:
            for part in ("frame", "update", "draw"):
                row[part] = dict(zip(TRIPLE, (int(v) / US_PER_MS for v in fields[part].split("/"))))
    return rows, heaps


def worst(readings, pick):
    return max(pick(reading) for reading in readings)


def mean(readings, pick):
    return sum(pick(reading) for reading in readings) / len(readings)


def tables(text):
    parsed = [parse(lines) for lines in runs(text)]
    if not parsed:
        raise ValueError(f"no {PREFIX} lines")
    first, heaps = parsed[0]
    keys = list(first)
    if any(list(rows) != keys for rows, _ in parsed):
        raise ValueError("runs cover different configurations or phases")
    configs = list(dict.fromkeys(config for config, _ in keys))
    phases = list(dict.fromkeys(phase for _, phase in keys))
    if BASE not in configs:
        raise ValueError(f"no {BASE} configuration")
    over = lambda config, phase: [rows[config, phase] for rows, _ in parsed]
    stage_keys = list(first[BASE, phases[0]]["stages"])
    scene_keys = list(dict.fromkeys(name for phase in phases for name in first[BASE, phase]["scene"]))
    r3d_keys = list(dict.fromkeys(name for phase in phases for name in first[BASE, phase]["r3d"]))
    count = len(parsed)
    lines = ["### Measured stages", "",
             f"The scripted tour at {FRAME_MS} ms of water per frame, one row per phase, from a board capture "
             f"({count} run{'s' if count != 1 else ''}: means are the runs' mean, p95 and worst their worst). "
             "Update is the simulation and the surface; draw is r3d drawing the water's dynamic mesh.", "",
             "| Phase | Frames | Substeps/frame | Triangles | Particles | Frame mean / p95 ms "
             "| Update mean / p95 ms | Draw mean / p95 ms |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for phase in phases:
        r = over(BASE, phase)
        cells = [phase, str(r[0]["frames"]), f"{r[0]['substeps'] / r[0]['frames']:.1f}", str(r[0]["tris"]),
                 str(r[0]["particles"])]
        for part in ("frame", "update", "draw"):
            cells.append(f"{mean(r, lambda x: x[part]['mean']):.1f} / {worst(r, lambda x: x[part]['p95']):.1f}")
        lines.append("| " + " | ".join(cells) + " |")
    lines += ["", f"Where the `{BASE}` update and draw go, ms per frame:", "",
              "| Stage | " + " | ".join(phases) + " |", "|---|" + "---:|" * len(phases)]
    for name in scene_keys:
        lines.append(f"| {name} | " + " | ".join(
            f"{mean(over(BASE, phase), lambda x: x['scene'].get(name, 0.0)):.2f}" for phase in phases) + " |")
    for stage in stage_keys:
        lines.append(f"| step: {stage} | " + " | ".join(
            f"{mean(over(BASE, phase), lambda x: x['stages'][stage]):.2f}" for phase in phases) + " |")
    for name in r3d_keys:
        lines.append(f"| {name} | " + " | ".join(
            f"{mean(over(BASE, phase), lambda x: x['r3d'].get(name, 0.0)):.2f}" for phase in phases) + " |")
    remeshed = any(over(config, phase)[0]["remeshed"] for config in configs for phase in phases)
    lines += ["", "Each experiment against the base, update mean ms per frame"
              + (" (frames meshed again whole on one core in brackets):" if remeshed else ":"), "",
              "| Configuration | " + " | ".join(phases) + " |", "|---|" + "---:|" * len(phases)]
    for config in configs:
        cells = []
        for phase in phases:
            r = over(config, phase)
            cell = f"{mean(r, lambda x: x['update']['mean']):.1f}"
            if r[0]["remeshed"]:
                cell += f" ({r[0]['remeshed']})"
            cells.append(cell)
        lines.append(f"| {config} | " + " | ".join(cells) + " |")
    lines += ["", "Internal RAM around each configuration, KB free / largest block:", "",
              f"| Configuration | {BEFORE} | {RUNNING} | {AFTER} |", "|---|---:|---:|---:|"]
    for config in configs:
        lines.append(f"| {config} | " + " | ".join(
            "{} / {}".format(*(v // BYTES_PER_KB for v in heaps[config, phase])) for phase in (BEFORE, RUNNING, AFTER))
            + " |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=pathlib.Path, default=ROOT / CAPTURE)
    parser.add_argument("--document", type=pathlib.Path, default=ROOT / DOCUMENT)
    parser.add_argument("--check", action="store_true")
    argv = sys.argv[1:]
    args = parser.parse_args(argv)
    command = shlex.join(["python", SCRIPT, *(arg for arg in argv if arg != "--check"), "--check"])
    try:
        text = args.capture.read_text(encoding="utf-8")
        build_id(text)
        body = tables(text) + f"\n\nSource: `{args.capture.name}`"
        changed = replace_block(args.document, BLOCK, body, check=args.check, command=command)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.check and changed:
        print(f"{args.document}: {BLOCK} differs; run the generator")
    return int(args.check and changed)


if __name__ == "__main__":
    sys.exit(main())
