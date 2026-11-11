"""Predict authored scenes' frame change from a stress preset A/B.

    python stress_predict.py --a A.log [A2.log ...] --b B.log [B2.log ...] \
        --scenes frame-profile.log [--window NAME LOG TEXT] [--scene ROW ...]

A and B are stress_perf captures of two builds (tests/suite_stress_perf.c).
Each preset's change in a stage is its B mean over its A mean, over every
replayed pose of every capture given. A scene's predicted change is the sum,
over the r3d stages, of that ratio less one times the scene's own stage
time: from the replayed poses of a frame profile capture, or from the
frame-cost windows of any log's lines that hold TEXT (--window). The last
column pools every preset: each stage's B sum over its A sum.
"""
import argparse
import pathlib
import statistics
import sys

sys.path[:0] = [str(pathlib.Path(__file__).resolve().parents[4] / "tools" / "perf"),
                str(pathlib.Path(__file__).resolve().parents[4] / "tools")]
from frame_cost import frame_cost_window
from frame_profile import replays

STAGES = ("r3d.cull", "r3d.transform", "r3d.draw", "r3d.upscale")
POOLED = "all"


def preset_stages(paths):
    """{preset: {stage: [ms per replayed pose]}} over every capture."""
    found = {}
    for path in paths:
        for pose in replays(pathlib.Path(path).read_text(encoding="utf-8", errors="replace")):
            stages = found.setdefault(pose["row"], {})
            for stage in STAGES:
                stages.setdefault(stage, []).append(pose["stages"].get(stage, 0.0))
    if not found:
        raise SystemExit(f"{' '.join(map(str, paths))}: no frame_replay lines")
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


def report(table, scenes):
    lines = ["| Preset | " + " | ".join(f"{s} A → B ms" for s in STAGES) + " |",
             "|---|" + "---:|" * len(STAGES)]
    for preset, change in table.items():
        lines.append(f"| {preset} | " + " | ".join(f"{a:.3f} → {b:.3f}" for a, b in change.values()) + " |")
    lines += ["", "| Scene | " + " | ".join(table) + " |", "|---|" + "---:|" * len(table)]
    for scene, stages in scenes.items():
        lines.append(f"| {scene} | " + " | ".join(f"{predict(change, stages):+.2f}" for change in table.values())
                     + " |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--a", nargs="+", required=True, type=pathlib.Path)
    parser.add_argument("--b", nargs="+", required=True, type=pathlib.Path)
    parser.add_argument("--scenes", nargs="*", default=[], type=pathlib.Path, help="frame profile captures")
    parser.add_argument("--scene", nargs="*", default=[], help="frame profile rows to keep; all when omitted")
    parser.add_argument("--window", nargs=3, action="append", default=[], metavar=("NAME", "LOG", "TEXT"))
    parser.add_argument("--quarter", default="portrait")
    args = parser.parse_args(argv)
    scenes = scene_stages(args.scenes, args.window, set(args.scene), args.quarter)
    if hasattr(sys.stdout, "reconfigure"):  # a console's code page may lack the arrow
        sys.stdout.reconfigure(encoding="utf-8")
    sys.stdout.write(report(ratios(preset_stages(args.a), preset_stages(args.b)), scenes))


if __name__ == "__main__":
    main()
