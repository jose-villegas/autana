#!/usr/bin/env python3
"""Make a fitted mesh from the recipe its scene renderer records.

    python launcher/tools/r3d/fitted_variant.py SCENE.scene.toml --mesh NAME --work DIR prepare
    python launcher/tools/r3d/fitted_variant.py SCENE.scene.toml --mesh NAME --work DIR fit
    python launcher/tools/r3d/fitted_variant.py sweep SCENE.scene.toml --variant NAME --budgets 4000,6000 --out DIR

A scene renderer with a `fit` table is not baked by mesh_import.py.
`prepare`, in the r3d environment, bakes its start (the import's geometry
steps at the variant's `triangles`, lit by the scene's bake, flat when the
renderer's shading is), samples the
scene camera's path for training,
held-out and pruning poses, and renders the training references with their
normals into DIR. `fit`, in the GPU environment of appearance_simplify.py,
prunes the start to the recipe's budget, fits it with the recipe's settings
and writes the renderer's mesh into DIR. bake/bake.py runs both into the bake
cache, keyed, and its lock records what the fit made.
"""

import argparse
import csv
import hashlib
import json
import math
import pathlib
import shutil
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.import_settings import SettingsError, load_scene, source_digest  # noqa: E402

CSV_FIELDS = ("budget", "cost_weight", "triangles", "mean_delta_e", "p95_delta_e", "predicted_ms", "board_ms")
BOARD_SCALE = 2
# The host renderer that draws any scene file's objects along its camera path.
VIEWER = pathlib.Path(__file__).resolve().parents[1] / "render/scene_viewer.sh"


def placed_variant(scene, name):
    """The fitted renderer named by its scene object, its scene output or,
    when only one renderer fits it, its variant."""
    fitted = [item for item in scene.renderers if item.renderer.fit]
    jobs = [item for item in fitted if name in (item.object.name, item.asset_name)]
    jobs = jobs or [item for item in fitted if item.renderer.variant.name == name]
    if not jobs:
        raise SettingsError(f"the scene places no fitted variant {name!r}")
    if len(jobs) > 1:
        raise SettingsError(f"{name!r} is fitted by {', '.join(item.object.name for item in jobs)}; name the object")
    return jobs[0]


def poses_text(width, height, lens, near, poses):
    lines = [f"size {width} {height}", f"lens {lens!r} {near!r}"]
    lines += ["pose " + " ".join("%.9g" % value for value in pose) for pose in poses]
    return "\n".join(lines) + "\n"


def split_poses(fit, poses):
    """(training, held out): the poses sampled every train_every_ms, those at
    multiples of held_out_every_ms (time 0 aside) held out."""
    training, held_out = [], []
    for index, pose in enumerate(poses):
        time = index * fit.train_every_ms
        (held_out if time and time % fit.held_out_every_ms == 0 else training).append(pose)
    return training, held_out


def viewer_args(job, frames, dt_ms, scene):
    """The scene viewer's arguments that draw `job`'s object alone, portrait,
    for `frames` frames `dt_ms` apart along the scene camera's path."""
    from r3d.scene_asset import scene_id
    return f"--scene {scene_id(scene)} --object {job.object.name} --quarter 0 --frames {frames} --dt {dt_ms}"


def canonical(value):
    """A JSON-ready form of parsed settings, independent of parser layout.
    The digest hashes parser fields, so an explicit default differs from an omitted one; the stamp test catches it."""
    if isinstance(value, pathlib.Path):
        return None
    if isinstance(value, SimpleNamespace):
        return {name: canonical(item) for name, item in sorted(vars(value).items())
                if name != "seed_given" and not isinstance(item, pathlib.Path)}
    if isinstance(value, dict):
        return {name: canonical(item) for name, item in sorted(value.items())}
    if isinstance(value, (list, tuple)):
        return [canonical(item) for item in value]
    if isinstance(value, (set, frozenset)):
        return sorted(canonical(item) for item in value)
    return value


def camera_clip(scene):
    """The TRCK entry the scene camera's clip bakes to; the camera path's
    poses are sampled from it."""
    from anim import tracks_asset

    return tracks_asset.bake(scene.camera.component.path.animation)


def prepare(scene_path, scene, job, work, recorder=None):
    from r3d.mesh_import import camera_path_poses, write_baked
    from r3d.reference_render import main as reference_main

    renderer = job.renderer
    variant, fit, visibility = renderer.variant, renderer.fit, renderer.visibility
    if visibility is None or visibility.source != "camera_path":
        raise SettingsError(f"{variant.name} needs camera_path visibility: its poses come from the path")
    work.mkdir(parents=True, exist_ok=True)
    start = write_baked(job, scene, work, variant.name, recorder=recorder)
    w, h, lens, near, poses = camera_path_poses(scene, visibility, fit.train_every_ms, either_way_up=False)
    training, held_out = split_poses(fit, poses)
    (work / "train.txt").write_text(poses_text(w, h, lens, near, training))
    (work / "held_out.txt").write_text(poses_text(w, h, lens, near, held_out))
    (work / "train_landscape.txt").write_text(poses_text(h, w, lens, near, training))
    (work / "coverage.txt").write_text(poses_text(*camera_path_poses(scene, visibility, fit.coverage_every_ms)))
    for poses, reference in (("train.txt", "reference"), ("train_landscape.txt", "reference_landscape"),
                             ("held_out.txt", "reference_held_out")):
        reference_main([str(scene_path), "--object", job.object.name, "--poses",
                        str(work / poses), "--out", str(work / reference), "--normals"])
    log(f"prepared {variant.name}: start of {len(start.tris)} triangles, {len(training)} training poses")


def reference_digest(job, scene):
    """A sweep's references are the fit's reference stage: its key, from bake/bake.py, the one owner of
    bake keys."""
    from bake import bake

    return bake.stage_keys(job, scene, {stage: bake.tool_digest(stage) for stage in bake.STAGES})["reference"]


def sweep_references(scene_path, scene, job, work):
    """Prepare one reusable reference set for a sweep's unchanged inputs."""
    marker = pathlib.Path(work) / "references.json"
    digest = reference_digest(job, scene)
    if marker.is_file() and json.loads(marker.read_text()).get("digest") == digest:
        return
    prepare(scene_path, scene, job, work)
    marker.write_text(json.dumps({"digest": digest}, sort_keys=True) + "\n")


def fit(scene_path, scene, job, work, budget=None, cost_weight=0.0, smoke=False, target=None, inputs=None):
    from r3d.appearance_simplify import main as fit_main

    renderer = job.renderer
    variant, recipe = renderer.variant, renderer.fit
    inputs = pathlib.Path(work) if inputs is None else pathlib.Path(inputs)
    start = inputs / f"{variant.name}.mesh"
    out = work / "fitted"
    steps = min(recipe.steps, 8) if smoke else recipe.steps
    command = ["--scene", str(scene_path), "--start", str(start), "--poses", str(inputs / "train.txt"), "--reference",
              str(inputs / "reference"), "--poses", str(inputs / "train_landscape.txt"), "--reference",
              str(inputs / "reference_landscape"), "--out", str(out), "--budget", str(recipe.budget if budget is None else budget),
              "--coverage-poses", str(inputs / "coverage.txt"), "--steps", str(steps), "--batch", str(recipe.batch), "--laplacian",
              str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight)]
    if cost_weight:
        command += ["--cost-model", str(pathlib.Path(__file__).with_name("board_cost_weights.txt")), "--cost-weight", str(cost_weight)]
    fit_main(command)
    if target is None:
        target = pathlib.Path(work) / f"{job.asset_name}.mesh"
    shutil.copyfile(out / f"{variant.name}.mesh", target)
    log(f"wrote {target}")
    return target


def fit_point(point, point_dir, scene_path, scene, job, inputs, smoke=False, target=None, recorder=None):
    import contextlib
    import traceback
    from r3d.process_budget import NULL_RECORDER
    from r3d.lit_mesh import read_lit_mesh
    target = pathlib.Path(point_dir).parent / f"{pathlib.Path(point_dir).name}.mesh" if target is None else target
    log_path = pathlib.Path(point_dir) / "fit.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    recording = recorder is not None
    recorder = NULL_RECORDER if recorder is None else recorder
    triangles = len(read_lit_mesh(pathlib.Path(inputs) / f"{job.renderer.variant.name}.mesh").tris) if recording else 0
    try:
        with recorder.step("fit", triangles, gpu=True) as step:
            with log_path.open("w") as output, contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                mesh = fit(scene_path, scene, job, pathlib.Path(point_dir), budget=point.get("budget"),
                       cost_weight=point.get("cost_weight", 0.0), smoke=smoke, target=target, inputs=inputs)
            if recording:
                step["triangles_out"] = len(read_lit_mesh(mesh).tris)
        return {"mesh": str(mesh)}
    except Exception as error:
        with log_path.open("a") as output:
            traceback.print_exc(file=output)
        raise RuntimeError(f"fit failed: {log_path}\n" + "\n".join(log_path.read_text().splitlines()[-30:])) from error


def non_dominated_front(points):
    """(front, knee) minimizing predicted milliseconds and mean DeltaE."""
    front = []
    for point in points:
        dominated = any(other is not point and other["predicted_ms"] <= point["predicted_ms"] and
                        other["mean_delta_e"] <= point["mean_delta_e"] and
                        (other["predicted_ms"] < point["predicted_ms"] or other["mean_delta_e"] < point["mean_delta_e"])
                        for other in points)
        if not dominated:
            front.append(point)
    front.sort(key=lambda point: (point["predicted_ms"], point["mean_delta_e"]))
    if len(front) < 3:
        return front, front[0] if front else None
    xs = [point["predicted_ms"] for point in front]
    ys = [point["mean_delta_e"] for point in front]
    dx, dy = max(xs) - min(xs), max(ys) - min(ys)
    if not dx or not dy:
        return front, front[0]
    ax, ay, bx, by = 0.0, (ys[0] - min(ys)) / dy, 1.0, (ys[-1] - min(ys)) / dy
    length = math.hypot(by - ay, bx - ax)
    distance = lambda point: abs((by - ay) * ((point["predicted_ms"] - min(xs)) / dx - ax) -
                                 (bx - ax) * ((point["mean_delta_e"] - min(ys)) / dy - ay)) / length
    return front, max(front[1:-1], key=distance)


def write_sweep_csv(path, rows):
    """Write the stable public sweep schema, leaving absent board readings blank."""
    with open(path, "w", newline="") as output:
        writer = csv.DictWriter(output, CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({name: "" if row.get(name) is None else row.get(name) for name in CSV_FIELDS})


def point_name(point):
    return "budget-%d-cost-%g" % (point["budget"], point["cost_weight"])


def run_sweep_points(out, points, run_fit, executor=None, deferred=None):
    """Submit unfinished fits or consume them in point order; result records are resume markers."""
    completed = 0
    for point in points:
        point_dir = pathlib.Path(out) / point_name(point)
        result = point_dir / "result.json"
        if result.is_file():
            continue
        point_dir.mkdir(parents=True, exist_ok=True)
        if executor is not None:
            if deferred is None:
                raise ValueError("an executor needs a deferred result mapping")
            from r3d.process_budget import FIT_BYTES
            deferred[point_name(point)] = executor.submit(run_fit, point, point_dir, estimates=FIT_BYTES)
            continue
        row = {**point, **run_fit(point, point_dir)}
        temporary = result.with_suffix(".tmp")
        temporary.write_text(json.dumps(row, sort_keys=True) + "\n")
        temporary.replace(result)
        completed += 1
    return completed


def sweep_rows(out, points):
    """Completed rows in command-line order."""
    return [json.loads((pathlib.Path(out) / point_name(point) / "result.json").read_text()) for point in points]


def _score_mesh(args, name, mesh_path, score_dir, host, scene):
    """Score a packed mesh through the host renderer."""
    from r3d.bake_fidelity import score, write_assets

    return score(args, host, write_assets(name, mesh_path, score_dir, scene), score_dir)[:2]


def held_out_score(job, mesh_path, work, host, scene, inputs=None):
    """Mean and p95 DeltaE76 from the host renderer against held-out references."""
    from types import SimpleNamespace
    from r3d.poses import read_poses

    fit = job.renderer.fit
    inputs = pathlib.Path(work) if inputs is None else pathlib.Path(inputs)
    _width, _height, _lens, _near, poses = read_poses(inputs / "held_out.txt")
    score_dir = work / "score"
    score_dir.mkdir(exist_ok=True)
    args = SimpleNamespace(render_args=viewer_args(job, len(poses), fit.held_out_every_ms, scene), reference=inputs / "reference_held_out",
                           reference_scale=BOARD_SCALE)
    return _score_mesh(args, job.asset_name, mesh_path, score_dir, host, scene)


def board_poses(work):
    """The held-out camera path at the board renderer's native size."""
    return pathlib.Path(work) / "held_out.txt"


def plot_pareto(path, rows):
    """Draw the sweep points, the non-dominated front and its chord-distance knee."""
    import matplotlib.pyplot as plt
    from matplotlib import patheffects

    front, knee = non_dominated_front(rows)
    figure, axis = plt.subplots(figsize=(8, 5), layout="constrained")
    for row in rows:
        axis.scatter(row["predicted_ms"], row["mean_delta_e"], color="tab:blue", zorder=2)
        label = "%d, %g" % (row["budget"], row["cost_weight"])
        axis.annotate(label, (row["predicted_ms"], row["mean_delta_e"]), xytext=(4, 4), textcoords="offset points",
                      fontsize=8, path_effects=[patheffects.withStroke(linewidth=3, foreground="white")])
    if front:
        axis.plot([row["predicted_ms"] for row in front], [row["mean_delta_e"] for row in front], color="tab:orange", zorder=1)
    if knee is not None:
        axis.scatter(knee["predicted_ms"], knee["mean_delta_e"], s=110, facecolors="none", edgecolors="black", linewidths=1.5,
                     zorder=3, label="knee")
        axis.legend()
    axis.set_xlabel("Predicted frame time (ms)")
    axis.set_ylabel("Held-out mean $\\Delta E_{76}$")
    figure.savefig(path, dpi=160)
    plt.close(figure)


def comma_numbers(text, cast):
    try:
        values = [cast(value) for value in text.split(",") if value]
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if not values:
        raise argparse.ArgumentTypeError("needs at least one comma-separated value")
    return values


def sweep_main(argv):
    parser = argparse.ArgumentParser(description="Fit and score a recipe over triangle budgets and cost weights.")
    parser.add_argument("scene")
    parser.add_argument("--variant", required=True, help="the fitted scene object, or its variant when only one object fits it")
    parser.add_argument("--budgets", required=True, type=lambda text: comma_numbers(text, int))
    parser.add_argument("--cost-weights", default="0", type=lambda text: comma_numbers(text, float))
    parser.add_argument("--board-ms", type=lambda text: comma_numbers(text, float), help="measurements in budget, cost-weight order")
    parser.add_argument("--out", required=True)
    parser.add_argument("--smoke", action="store_true", help="fit eight steps at each point")
    args = parser.parse_args(argv)
    scene_path = pathlib.Path(args.scene).resolve()
    try:
        scene = load_scene(scene_path)
        job = placed_variant(scene, args.variant)
    except SettingsError as error:
        parser.error(str(error))
    points = [{"budget": budget, "cost_weight": weight} for budget in args.budgets for weight in args.cost_weights]
    variant = job.renderer.variant
    if any(budget > variant.triangles for budget in args.budgets):
        parser.error("--budgets cannot exceed the variant's start triangle count")
    if args.board_ms is not None and len(args.board_ms) != len(points):
        parser.error("--board-ms needs one value per budget and cost-weight point")
    out = pathlib.Path(args.out).resolve()
    reference_work = out / "references"
    sweep_references(scene_path, scene, job, reference_work)
    host = None

    def run_point(point, point_dir):
        nonlocal host
        if host is None:
            from r3d.bake_fidelity import build_host

            host = build_host(VIEWER, out / "host", scene_path)
        work = point_dir / "work"
        mesh = fit(scene_path, scene, job, work, budget=point["budget"], cost_weight=point["cost_weight"],
                   smoke=args.smoke, target=point_dir / f"{variant.name}.mesh", inputs=reference_work)
        mean, p95 = held_out_score(job, mesh, work, host, inputs=reference_work, scene=scene_path)
        from r3d.cost_model import load, mesh_rows, predict
        from r3d.lit_mesh import finest_triangles, read_lit_mesh

        weights, _rows, _ms, _labels = load(pathlib.Path(__file__).with_name("board_cost_weights.txt"))
        predicted = float(predict(weights, mesh_rows(mesh, board_poses(reference_work))).mean())
        triangles = len(finest_triangles(read_lit_mesh(mesh))[2])
        return {"triangles": triangles, "mean_delta_e": mean, "p95_delta_e": p95, "predicted_ms": predicted}

    run_sweep_points(out, points, run_point)
    rows = sweep_rows(out, points)
    if args.board_ms is not None:
        for row, value in zip(rows, args.board_ms):
            row["board_ms"] = value
    write_sweep_csv(out / "sweep.csv", rows)
    plot_pareto(out / "pareto.png", rows)
    for row in rows:
        print("budget %(budget)d, cost %(cost_weight)g: %(triangles)d triangles, mean dE76 %(mean_delta_e).3f, p95 %(p95_delta_e).3f, %(predicted_ms).3f ms" % row)
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "sweep":
        return sweep_main(argv[1:])
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene")
    parser.add_argument("--mesh", required=True, help="the fitted scene object, or its variant when only one object fits it")
    parser.add_argument("--work", required=True, help="scratch directory the two steps share")
    parser.add_argument("step", choices=("prepare", "fit"))
    args = parser.parse_args(argv)
    scene_path = pathlib.Path(args.scene).resolve()
    try:
        scene = load_scene(scene_path)
        job = placed_variant(scene, args.mesh)
    except SettingsError as error:
        parser.error(str(error))
    work = pathlib.Path(args.work).resolve()
    if args.step == "prepare":
        prepare(scene_path, scene, job, work)
    else:
        fit(scene_path, scene, job, work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
