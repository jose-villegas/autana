"""Generate performance tables from board captures."""
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
from capture import build_id, QUARTERS, capture_quarters, select_quarter
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[5] / "scripts/device"))
from device_report import BUDGET_TABLE_HEADER, parse_perf_targets, read_capture_text
from frame_profile import PATH_POSES, PEAK_PERCENT, judged, peaks, profiles, replays, summaries
from frame_cost import MS_PER_SECOND, US_PER_MS

LOG_HEADER = pathlib.Path(__file__).resolve().parents[1] / "tests/skin_perf_log.h"
PREFIX = read_define(LOG_HEADER, "SKIN_PERF_PREFIX")
BUDGET_NS_PER_VERTEX = read_define(LOG_HEADER, "SKIN_PERF_BUDGET_NS_PER_VERTEX")
NANOSECONDS_PER_MICROSECOND = read_define(LOG_HEADER, "SKIN_PERF_NS_PER_US")
CORES_ONE = read_define(LOG_HEADER, "SKIN_PERF_CORES_ONE")
CORES_TWO = read_define(LOG_HEADER, "SKIN_PERF_CORES_TWO")
CORES_UNKNOWN = read_define(LOG_HEADER, "SKIN_PERF_CORES_UNKNOWN")
PERCENT_SCALE = read_define(LOG_HEADER.parents[4] / "test/perf_budget.h", "PERF_BUDGET_PERCENT")
SKIN_HEADER = LOG_HEADER.parents[3] / "render/r3d_skin_format.h"
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
    observed = capture_quarters(text)
    return _parse_capture(select_quarter(text, quarter, observed), bool(observed),
                          quarter=quarter if observed else None)


def _capture_runs(text):
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
                current[(fields["stage"], fields.get("cores", fields.get("paused", "")), fields.get("quarter"))] = fields
    return runs


def _parse_capture(text, tagged, source=None, quarter=None):
    render_stages = {STAGE_RENDER, STAGE_FRAME, STAGE_RENDER_DELTA, STAGE_FRAME_DELTA}
    build = build_id(text)
    source = _capture_runs(text) if source is None else source
    runs = {}
    for number, raw in source.items():
        current = runs[number] = {}
        for key, fields in raw.items():
            if key[0] in render_stages and fields.get("quarter") != quarter:
                continue
            current[key[:2]] = fields
    if not runs:
        raise ValueError("capture has no runs")
    required = (*STAGES, *RENDER_KEYS, *(FRAME_KEYS if tagged else ()), *(("phase", phase) for phase in PHASES))
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
    observed = capture_quarters(text)
    if observed:
        capture_quarters(text, expected=QUARTERS)
    quarters = [q for q in QUARTERS if q in observed] or [None]
    source = _capture_runs(text)
    parsed = {q: _parse_capture(select_quarter(text, q or "portrait", observed), bool(observed), source, q)
              for q in quarters}
    data = parsed[quarters[0]]
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
    for quarter, rendered in parsed.items():
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


def budget_row(name, goal, measured):
    headroom = f"{(goal - measured) * PERCENT_SCALE / goal:+.1f}%" if goal else "n/a"
    status = "MET" if measured <= goal else "UNMET"
    return f"| {name} | {goal:g} | {measured:g} | {headroom} | {status} |"


def frame_report(text, rows, playback):
    if playback:
        lines = ["## Frame rate", "",
                 "| Scene | Quarter | Frames | Mean fps | Low fps (1/p95) | p50 ms | p95 ms | Worst ms |",
                 "|---|---|---:|---:|---:|---:|---:|---:|"]
    else:
        lines = ["## Frame cost along the flythrough", "",
                 "| Row | Quarter | Mean fps (before present) | Mean ms | p50 ms | p95 ms | Worst ms |",
                 "|---|---|---:|---:|---:|---:|---:|"]
    for row in rows:
        cells = [row["row"], row["quarter"]]
        if playback:
            cells.extend((str(row["frames"]), f"{row['fps_mean']:.1f}", f"{row['fps_low']:.1f}"))
        else:
            cells.extend((f"{row['fps_mean']:.1f}", f"{row['mean'] / US_PER_MS:.1f}"))
        cells.extend(f"{row[name] / US_PER_MS:.1f}" for name in ("med", "p95", "worst"))
        lines.append("| " + " | ".join(cells) + " |")
    lines.extend(("", BUDGET_TABLE_HEADER, "|---|---:|---:|---:|---|"))
    targets, _ = parse_perf_targets(text)
    for target in targets:
        measured, goal = int(target["measured"]), int(target["goal"])
        lines.append(budget_row(target["name"], goal, measured))
    return "\n".join(lines) + "\n"


# The counts each frame peak lists, as (column, count names); the "why" weighs them all.
PEAK_COUNTS = (("Clusters tested / kept / hidden / drawn",
                 ("clusters_tested", "clusters_kept", "clusters_hidden", "clusters_drawn")),
                ("Triangles submitted / set up / cover / miss / away",
                 ("triangles_submitted", "triangles_set_up", "triangles_covering", "triangles_missed",
                  "triangles_facing_away")))


def path_poses(text):
    """{(row, quarter): (the evenly spaced replays read, the frame peaks' replays)} in capture order;
    fails unless each frame peak has its replay. A path replay lost in transfer leaves its curve a point short."""
    found = {}
    for pose in replays(text):
        found.setdefault((pose["row"], pose["quarter"]), []).append(pose)
    ranked = {}
    for frame in peaks(text):
        ranked.setdefault((frame["row"], frame["quarter"]), []).append(frame)
    poses = {}
    for key, frames in ranked.items():
        replayed = found.get(key, [])
        path, tail = replayed[:len(replayed) - len(frames)], replayed[len(replayed) - len(frames):]
        if not path or len(path) > PATH_POSES or [p["t_ms"] for p in tail] != [f["t_ms"] for f in frames]:
            raise ValueError(f"{' '.join(key)}: {len(replayed)} replays do not end with its {len(frames)} frame peaks")
        poses[key] = (path, list(zip(frames, tail)))
    return poses


def why(pose, path):
    """The stage that most exceeds its path median, and the count most above its path median."""
    stages = {name: ms - statistics.median(p["stages"].get(name, 0) for p in path)
              for name, ms in pose["stages"].items()}
    stage = max(stages, key=stages.get)
    ratios = {}
    for _, names in PEAK_COUNTS:
        for name in names:
            median = statistics.median(p["counts"][name] for p in path)
            if median > 0:
                ratios[name] = pose["counts"][name] / median
    reason = f"{stage} {stages[stage]:+.1f} ms"
    if not ratios:
        return reason
    count = max(ratios, key=ratios.get)
    return f"{reason}; {count.replace('_', ' ')} ×{ratios[count]:.2f} median"


def frame_peaks(text):
    """The frame peaks of each traced row and quarter: when along the path, the played frame's
    time and prominence, the paused replay's stage split and counts, and why, against the path's median pose."""
    poses = path_poses(text)
    if not poses:
        return ""
    stages = list(dict.fromkeys(name for path, _ in poses.values() for pose in path for name in pose["stages"]))
    lines = ["## Frame peaks", "",
             "One played frame per peak along the looping path, its slowest, ranked by wall time. A peak counts",
             f"when it stands {PEAK_PERCENT}% of its height above the higher of its two saddles.",
             "Stage ms and counts come from replaying each pose paused;",
             f"Why compares them with the median of the {PATH_POSES} poses spaced evenly along the path"
             f" ({sum(len(path) for path, _ in poses.values())} of {PATH_POSES * len(poses)} read).", "",
             "| Scene | Quarter | Rank | Path s | Frame ms | Prominence ms | " + " | ".join(f"{name} ms" for name in stages)
             + " | " + " | ".join(column for column, _ in PEAK_COUNTS) + " | Why |",
             "|---|---|---:|---:|---:|---:|" + "---:|" * len(stages) + "---|" * len(PEAK_COUNTS) + "---|"]
    for (row, quarter), (path, ranked) in poses.items():
        for frame, pose in ranked:
            cells = [row, quarter, str(frame["rank"]), f"{frame['t_ms'] / MS_PER_SECOND:.2f}",
                     f"{frame['frame_us'] / US_PER_MS:.1f}", f"{frame['prominence_us'] / US_PER_MS:.1f}"]
            cells.extend(f"{pose['stages'][name]:.2f}" if name in pose["stages"] else "–" for name in stages)
            cells.extend(" / ".join(str(pose["counts"][name]) for name in names) for _, names in PEAK_COUNTS)
            cells.append(why(pose, path))
            lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def performance_report(text):
    text = judged(text)
    if rows := profiles(text):
        return frame_report(text, rows, playback=True) + (f"\n{section}" if (section := frame_peaks(text)) else "")
    if rows := summaries(text):
        return frame_report(text, rows, playback=False)
    if not re.search(r"^RUN=", text, re.MULTILINE):
        text = "RUN=1\n" + text
    observed = capture_quarters(text)
    quarter = next((q for q in QUARTERS if q in observed), "portrait")
    data = parse_capture(text, quarter)
    goal = data["vertices"] * BUDGET_NS_PER_VERTEX / NANOSECONDS_PER_MICROSECOND
    lines = [BUDGET_TABLE_HEADER, "|---|---:|---:|---:|---|"]
    for core in (CORES_ONE, CORES_TWO):
        measured = data["stages"][(STAGE_SKIN, core)]
        lines.append(budget_row(f"Skin, {core} core(s)", goal, measured))
    return "\n".join(lines) + "\n\n" + budget(text) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("document", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--table", action="store_true")
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--repeat", default="autana suite run_capybara_perf_suite --flash --perf-scope")
    args = parser.parse_args()
    try:
        text = read_capture_text(args.capture)
        if args.table:
            from layout_measure import parse_timings
            rows = parse_timings("\n".join(line for line in text.splitlines() if PREFIX in line))
            if not rows:
                raise ValueError("capture has no stage timings")
            lines = ["| Name | Mean us |", "|---|---:|"]
            lines.extend(f"| {name} | {value} |" for name, value in rows.items())
            args.document.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return 0
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
