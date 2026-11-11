"""Fit stage costs on stress replay poses and score held-out scene poses."""
import argparse
import pathlib
import sys

import numpy as np
from scipy.optimize import nnls

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path[:0] = [str(ROOT / "launcher/tools/perf"), str(ROOT / "launcher/tools")]
from frame_profile import replays
from render.cull_counts import size_fields

STAGES = ("r3d.cull", "r3d.transform", "r3d.draw", "r3d.upscale")
FRAME = "frame"
ALL_STAGES = (*STAGES, FRAME)
# Each stage's own work: what a stage does sets the counts it is fitted on.
STAGE_INPUTS = {
    "r3d.cull": ("nodes_visited", "nodes_cut", "clusters_tested", "clusters_off_frustum", "clusters_kept"),
    "r3d.transform": ("clusters_kept", "vertices_transformed"),
    "r3d.draw": (*size_fields(), "pixels_face", "pixels_uniform", "pixels_shaded", "triangles_facing_away",
                 "triangles_missed", "triangles_behind", "triangles_clipped"),
    "r3d.upscale": (),
}
TERMS = tuple(dict.fromkeys(name for names in STAGE_INPUTS.values() for name in names))
PERCENTILE = 95
ERROR_LIMIT = 0.10
PERCENT = 100
SUPPORT_TOLERANCE = 1e-7


def load(paths, quarter, rows=(), names=TERMS):
    found = []
    for path in paths:
        poses = {(p["row"], p["t_ms"]): p
                 for p in replays(path.read_text(encoding="utf-8", errors="replace"))
                 if p["quarter"] == quarter and (not rows or p["row"] in rows)}
        for pose in poses.values():
            missing = set(names) - pose["counts"].keys()
            if missing:
                raise ValueError(f"{path}: {pose['row']}: missing counts: {', '.join(sorted(missing))}")
            absent = set(STAGES) - pose["stages"].keys()
            if absent:
                raise ValueError(f"{path}: {pose['row']}: missing stages: {', '.join(sorted(absent))}")
        found.extend(poses.values())
    missing_rows = set(rows) - {p["row"] for p in found}
    if missing_rows:
        raise ValueError(f"missing scene rows: {', '.join(sorted(missing_rows))}")
    if not found:
        raise ValueError(f"{paths}: no frame_replay lines in {quarter}")
    return found


def design(poses, names):
    return np.array([[1.0, *(p["counts"][name] for name in names)] for p in poses], dtype=float)


def fit(poses, inputs=STAGE_INPUTS):
    """Per stage, stage_ms = c + sum(beta * count) over that stage's own inputs, by
    non-negative least squares: no count makes a stage cheaper."""
    weights, notes, untrained = {}, [], set()
    for stage, names in inputs.items():
        matrix = design(poses, names)
        scales = np.maximum(np.max(np.abs(matrix), axis=0), 1)
        solved, _ = nnls(matrix / scales, np.array([p["stages"][stage] for p in poses], dtype=float))
        weights[stage] = solved / scales
        flat = [name for col, name in enumerate(names, 1) if np.ptp(matrix[:, col]) == 0]
        untrained.update(flat)
        notes.extend(f"{stage}: {name} never varies in training" for name in flat)
    return {"inputs": dict(inputs), "weights": weights, "notes": notes, "untrained": untrained,
            "trained_values": {name: {p["counts"][name] for p in poses} for name in untrained}}


def estimates(model, poses):
    return {stage: design(poses, names) @ model["weights"][stage] for stage, names in model["inputs"].items()}


def unsupported_terms(model, poses):
    """Inputs the training never varied that these poses take another value of."""
    return sorted(name for name in model["untrained"]
                  if any(p["counts"][name] not in model["trained_values"][name] for p in poses))


def stage_values(poses):
    return {stage: np.array([p["stages"][stage] for p in poses]) for stage in STAGES}


def with_frame(values):
    return {**values, FRAME: sum(values[stage] for stage in STAGES)}


def paired(a, b):
    """The poses both captures replayed, by (scene, path time): a frame profile's
    peak poses are each run's own, its path poses shared."""
    def keyed(poses):
        return {(p["row"], p["t_ms"]): p for p in poses}
    left, right = keyed(a), keyed(b)
    if len(left) != len(a) or len(right) != len(b):
        raise ValueError("paired captures require unique pose keys; supply one capture per arm")
    shared = sorted(left.keys() & right.keys())
    if not shared:
        raise ValueError("captures share no pose (scene, path time)")
    return [left[key] for key in shared], [right[key] for key in shared]


def noise_floor(a, b):
    left, right = paired(a, b)
    av, bv = with_frame(stage_values(left)), with_frame(stage_values(right))
    return {stage: float(np.percentile(np.abs(bv[stage] - av[stage]), PERCENTILE)) for stage in ALL_STAGES}


def score_row(stage, scene, measured, predicted, floor, unsupported):
    denominator = np.maximum(np.abs(measured), floor / ERROR_LIMIT)
    absolute = np.abs(predicted - measured)
    errors = np.divide(absolute, denominator, out=np.full_like(absolute, np.inf), where=denominator > 0)
    errors[(denominator == 0) & (absolute <= SUPPORT_TOLERANCE)] = 0
    p95 = float(np.percentile(errors, PERCENTILE))
    return {"stage": stage, "scene": scene, "measured": float(np.mean(measured)),
            "predicted": float(np.mean(predicted)), "error": PERCENT * p95,
            "pass": p95 <= ERROR_LIMIT + SUPPORT_TOLERANCE and not unsupported,
            "unsupported": unsupported}


def scene_scores(scene, poses, predicted, measured, floors, models):
    selected = [i for i, p in enumerate(poses) if p["row"] == scene]
    subset = [poses[i] for i in selected]
    unsupported = sorted(set(name for model in models for name in unsupported_terms(model, subset)))
    return [score_row(stage, scene, measured[stage][selected], predicted[stage][selected],
                      floors.get(stage, 0), unsupported) for stage in ALL_STAGES]


def score(model, scenes, other=None, scenes_b=None, floors=None):
    if other is not None:
        scenes, scenes_b = paired(scenes, scenes_b)
    predicted, measured = estimates(model, scenes), stage_values(scenes)
    models = [model]
    if other is not None:
        predicted_b, measured_b = estimates(other, scenes), stage_values(scenes_b)
        predicted = {stage: predicted_b[stage] - predicted[stage] for stage in STAGES}
        measured = {stage: measured_b[stage] - measured[stage] for stage in STAGES}
        models.append(other)
    pv, mv = with_frame(predicted), with_frame(measured)
    return [row for scene in sorted({p["row"] for p in scenes})
            for row in scene_scores(scene, scenes, pv, mv, floors or {}, models)]


def layout_rows(a, b, floors):
    left, right = paired(a, b)
    av, bv = with_frame(stage_values(left)), with_frame(stage_values(right))
    rows = []
    for preset in sorted({p["row"] for p in left}):
        selected = [i for i, pose in enumerate(left) if pose["row"] == preset]
        for stage in ALL_STAGES:
            changes = (bv[stage] - av[stage])[selected]
            magnitude = float(np.percentile(np.abs(changes), PERCENTILE))
            rows.append({"stage": stage, "scene": preset, "change": float(np.mean(changes)),
                         "p95": magnitude, "floor": floors[stage],
                         "seen": "seen" if magnitude > floors[stage] + SUPPORT_TOLERANCE else "not seen"})
    return rows


def model_lines(model, label):
    lines = [f"Model {label}: per stage, stage_ms = c + sum(beta * count), beta >= 0", *model["notes"],
             "stage term beta_ms"]
    for stage, names in model["inputs"].items():
        lines.extend(f"{stage} {term} {weight:.9g}"
                     for term, weight in zip(("intercept", *names), model["weights"][stage]))
    return lines


def table_lines(rows, label):
    lines = [label, "stage scene measured_mean_ms predicted_mean_ms p95_error_% result"]
    lines.extend(f"{r['stage']} {r['scene']} {r['measured']:.6f} {r['predicted']:.6f} "
                 f"{r['error']:.3f} {'PASS' if r['pass'] else 'FAIL'}" for r in rows)
    unsupported = sorted({name for r in rows for name in r["unsupported"]})
    if unsupported:
        lines.append("Inputs outside training (never varied there): " + ", ".join(unsupported))
    frame_ok = all(r["pass"] for r in rows if r["stage"] == FRAME)
    lines.append(f"{label} frame rule: {'PASS' if frame_ok else 'FAIL'}")
    return lines


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a", nargs="+", required=True, type=pathlib.Path, help="stress training captures")
    for option in ("b", "scenes", "scenes-a", "scenes-b", "noise-a", "noise-b", "layout-a", "layout-b"):
        parser.add_argument("--" + option, nargs="+", type=pathlib.Path)
    parser.add_argument("--scene", nargs="+", default=[])
    parser.add_argument("--quarter", default="portrait")
    parser.add_argument("--scene-quarter", help="the held-out scenes' quarter (default: --quarter)")
    args = parser.parse_args(argv)
    args.scene_quarter = args.scene_quarter or args.quarter
    for first, second in (("noise_a", "noise_b"), ("layout_a", "layout_b"), ("scenes_a", "scenes_b")):
        if bool(getattr(args, first)) != bool(getattr(args, second)):
            parser.error(f"--{first.replace('_', '-')} and --{second.replace('_', '-')} are a pair")
    if args.b and not args.scenes_a:
        parser.error("--b requires --scenes-a and --scenes-b for measured deltas")
    if (args.b or args.layout_a) and not args.noise_a:
        parser.error("delta and layout scoring require --noise-a and --noise-b (same-build pair)")
    if args.scenes_a and not args.b:
        parser.error("--scenes-a/--scenes-b require --b")
    if not (args.scenes or args.scenes_a):
        parser.error("supply held-out --scenes or --scenes-a/--scenes-b")
    return args


def main(argv=None):
    args = arguments(argv)
    model = fit(load(args.a, args.quarter))
    lines, judged = model_lines(model, "A"), []
    scenes = load(args.scenes or args.scenes_a, args.scene_quarter, args.scene)
    rows = score(model, scenes)
    lines.extend(table_lines(rows, "Absolute"))
    judged.extend(rows)
    floors = {}
    if args.noise_a:
        floors = noise_floor(load(args.noise_a, args.quarter), load(args.noise_b, args.quarter))
        lines.append("Same-build p95 absolute noise floor (ms): " + " ".join(f"{s}={v:.6f}" for s, v in floors.items()))
    if args.b:
        other = fit(load(args.b, args.quarter))
        lines.extend(model_lines(other, "B"))
        scene_a = load(args.scenes_a, args.scene_quarter, args.scene)
        scene_b = load(args.scenes_b, args.scene_quarter, args.scene)
        absolute_b = score(other, scene_b)
        lines.extend(table_lines(absolute_b, "Absolute B"))
        judged.extend(absolute_b)
        rows = score(model, scene_a, other, scene_b, floors)
        lines.extend(table_lines(rows, "Option-flip delta"))
        judged.extend(rows)
    if args.layout_a:
        layouts = layout_rows(load(args.layout_a, args.quarter), load(args.layout_b, args.quarter), floors)
        lines.append("Layout: stage preset mean_change_ms p95_abs_change_ms noise_ms result")
        lines.extend(f"{r['stage']} {r['scene']} {r['change']:+.6f} {r['p95']:.6f} {r['floor']:.6f} {r['seen']}"
                     for r in layouts)
    passed = all(r["pass"] for r in judged)
    lines.append(f"Verdict: {'PASS' if passed else 'FAIL'} — every stage and every scene {'passes' if passed else 'does not pass'}.")
    print("\n".join(lines))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
