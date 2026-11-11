"""Predict authored scenes' frame change from a stress preset A/B.

    python stress_predict.py --a A.log [A2.log ...] --b B.log [B2.log ...] \
        --scenes frame-profile.log [--window NAME LOG TEXT] [--scene ROW ...]

A and B are stress_perf captures of two builds (tests/suite_stress_perf.c).
Each preset's change in a stage is its B mean over its A mean, over every
replayed pose of every capture given. A scene's predicted change is the sum,
over the r3d stages, of that ratio less one times the scene's own stage
time, all in one quarter: from the replayed poses of a frame profile capture, or from the
frame-cost windows of any log's lines that hold TEXT (--window). The last
column pools every preset: each stage's B sum over its A sum. With --bins,
the bins column scales the other pooled stages and fits draw changes to
set-up triangle counts from A's replay lines. Empty bins drop; bins the
presets cannot distinguish share a fitted cost and are listed as merged.
"""
import argparse
import json
import pathlib
import statistics
import sys

import numpy as np

sys.path[:0] = [str(pathlib.Path(__file__).resolve().parents[4] / "tools" / "perf"),
                str(pathlib.Path(__file__).resolve().parents[4] / "tools")]
from frame_cost import frame_cost_window
from frame_profile import replays
from render.cull_counts import size_fields

BIN_FIELDS = size_fields()

STAGES = ("r3d.cull", "r3d.transform", "r3d.draw", "r3d.upscale")
POOLED = "all"


def preset_stages(paths, quarter):
    """{preset: {stage: [ms per replayed pose]}} over every capture, in `quarter`. A pose a
    capture replays twice (the harness reruns a case) counts once, as its last replay."""
    found = {}
    for path in paths:
        poses = {(pose["row"], pose["t_ms"]): pose
                 for pose in replays(pathlib.Path(path).read_text(encoding="utf-8", errors="replace"))
                 if pose["quarter"] == quarter}
        for (row, _), pose in poses.items():
            stages = found.setdefault(row, {})
            for stage in STAGES:
                stages.setdefault(stage, []).append(pose["stages"].get(stage, 0.0))
    if not found:
        raise SystemExit(f"{' '.join(map(str, paths))}: no frame_replay lines in {quarter}")
    return found


def ratios(a, b):
    """{preset: {stage: (A mean ms, B mean ms)}}, with the pooled row last."""
    missing = sorted(set(a) ^ set(b))
    if missing:
        raise SystemExit(f"presets in one arm only: {', '.join(missing)}")
    table = {preset: {stage: (statistics.mean(a[preset][stage]), statistics.mean(b[preset][stage]))
                      for stage in STAGES} for preset in a}
    table[POOLED] = {stage: (sum(row[stage][0] for row in table.values()),
                             sum(row[stage][1] for row in table.values())) for stage in STAGES}
    return table


def scene_stages(frame_logs, windows, rows, quarter):
    """{scene: {stage: mean ms}}: frame-profile rows in `quarter`, then each --window."""
    scenes = {}
    for path in frame_logs:
        poses = {}
        for pose in replays(pathlib.Path(path).read_text(encoding="utf-8", errors="replace")):
            if pose["quarter"] == quarter and (not rows or pose["row"] in rows):
                poses.setdefault(pose["row"], []).append(pose["stages"])
        for row, found in poses.items():
            scenes.setdefault(row, []).extend(found)
    for name, path, text in windows:
        found = [window["brackets"] for line in pathlib.Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
                 if text in line and (window := frame_cost_window(line)) is not None]
        if not found:
            raise SystemExit(f"{path}: no frame-cost window on a line holding {text!r}")
        scenes[name] = [{stage: avg for stage, (avg, _) in brackets.items()} for brackets in found]
    return {scene: {stage: statistics.mean(s.get(stage, 0.0) for s in found) for stage in STAGES}
            for scene, found in scenes.items()}


def predict(change, scene):
    """The scene's frame change in ms: each stage's ratio less one times the scene's stage time.
    A stage the presets never spend time on in A has no ratio and adds nothing."""
    return sum((b / a - 1.0) * scene[stage] for stage, (a, b) in change.items() if a > 0)


def preset_bins(paths, quarter):
    """Mean bin counts over the same deduplicated replay poses as the stage means."""
    found = {}
    for path in paths:
        poses = {(pose["row"], pose["t_ms"]): pose for pose in replays(path.read_text(encoding="utf-8"))
                 if pose["quarter"] == quarter}
        for (row, _), pose in poses.items():
            missing = set(BIN_FIELDS) - pose["counts"].keys()
            if missing:
                raise ValueError(f"{path}: {row}: missing bins: {', '.join(sorted(missing))}")
            found.setdefault(row, []).append(pose["counts"])
    return {row: {name: statistics.mean(p[name] for p in poses) for name in BIN_FIELDS}
            for row, poses in found.items()}


def fit_bins(counts, changes, names=BIN_FIELDS):
    """Signed least squares; empty columns drop and dependent columns share a cost."""
    rows = list(changes)
    active = [name for name in names if any(counts[row].get(name, 0) for row in rows)]
    notes = [f"dropped empty bin {name}" for name in names if name not in active]
    if not active:
        return {}, notes
    design = np.array([[counts[row][name] for name in active] for row in rows], dtype=float)
    groups = [[i] for i in range(len(active))]
    # Equal costs are the identifiable model when the observations cannot separate bins.
    while np.linalg.matrix_rank(design) < len(groups):
        _, _, vectors = np.linalg.svd(design, full_matrices=True)
        null = vectors[-1]
        first, second = sorted((int(np.argmax(null)), int(np.argmin(null))))
        groups[first] += groups.pop(second)
        design[:, first] += design[:, second]
        design = np.delete(design, second, axis=1)
    costs = np.linalg.lstsq(design, [changes[row] for row in rows], rcond=None)[0]
    weights = {}
    for group, cost in zip(groups, costs):
        members = [active[i] for i in group]
        weights.update((name, float(cost)) for name in members)
        if len(members) > 1:
            notes.append("merged bins: " + ", ".join(members))
    return weights, notes


def draw_delta(counts, weights):
    """Scene draw change in ms from mean set-up triangles and ms per triangle."""
    return sum(counts[name] * cost for name, cost in weights.items())


def predict_bins(change, stages, counts, weights):
    return predict({stage: times for stage, times in change.items() if stage != "r3d.draw"}, stages) + draw_delta(counts, weights)


def report(table, scenes, bins=None, weights=None, notes=()):
    lines = ["| Preset | " + " | ".join(f"{s} A → B ms" for s in STAGES) + " |",
             "|---|" + "---:|" * len(STAGES)]
    for preset, change in table.items():
        lines.append(f"| {preset} | " + " | ".join(f"{a:.3f} → {b:.3f}" for a, b in change.values()) + " |")
    columns = [*table, *(["bins"] if bins is not None else [])]
    lines += ["", "| Scene | " + " | ".join(columns) + " |", "|---|" + "---:|" * len(columns)]
    for scene, stages in scenes.items():
        lines.append(f"| {scene} | " + " | ".join(f"{predict(change, stages):+.2f}" for change in table.values())
                     + (f" | {predict_bins(table[POOLED], stages, bins[scene], weights):+.2f}" if bins is not None else "")
                     + " |")
    if weights is not None:
        lines += ["", "| Bin | δ ms/triangle |", "|---|---:|"]
        lines += [f"| {name} | {cost:+.8f} |" for name, cost in weights.items()]
        lines += ["", *notes]
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--a", nargs="+", required=True, type=pathlib.Path)
    parser.add_argument("--b", nargs="+", required=True, type=pathlib.Path)
    parser.add_argument("--scenes", nargs="*", default=[], type=pathlib.Path, help="frame profile captures")
    parser.add_argument("--scene", nargs="*", default=[], help="frame profile rows to keep; all when omitted")
    parser.add_argument("--window", nargs=3, action="append", default=[], metavar=("NAME", "LOG", "TEXT"))
    parser.add_argument("--quarter", default="portrait")
    parser.add_argument("--bins", type=pathlib.Path, help="scene mean triangle bins JSON")
    args = parser.parse_args(argv)
    scenes = scene_stages(args.scenes, args.window, set(args.scene), args.quarter)
    if hasattr(sys.stdout, "reconfigure"):  # a console's code page may lack the arrow
        sys.stdout.reconfigure(encoding="utf-8")
    table = ratios(preset_stages(args.a, args.quarter), preset_stages(args.b, args.quarter))
    bins, weights, notes = None, None, []
    if args.bins:
        if args.quarter != "portrait":
            raise ValueError("scene bins require portrait captures")
        data = json.loads(args.bins.read_text(encoding="utf-8"))
        if data["quarter"] != 0:
            raise ValueError("scene bins require quarter 0")
        bins = data["scenes"]
        counts = preset_bins(args.a, args.quarter)
        weights, notes = fit_bins(counts, {row: change["r3d.draw"][1] - change["r3d.draw"][0]
                                         for row, change in table.items() if row != POOLED})
    sys.stdout.write(report(table, scenes, bins, weights, notes))


if __name__ == "__main__":
    main()
