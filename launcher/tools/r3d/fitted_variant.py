#!/usr/bin/env python3
"""Make a fitted variant from the recipe its import file records.

    python launcher/tools/r3d/fitted_variant.py SCENE.scene.toml --mesh NAME --work DIR prepare
    python launcher/tools/r3d/fitted_variant.py SCENE.scene.toml --mesh NAME --work DIR fit
    python launcher/tools/r3d/fitted_variant.py sweep SCENE.scene.toml --variant NAME --budgets 4000,6000 --out DIR

A variant with a `fit` table is not baked by mesh_import.py. `prepare`, in
the r3d environment, bakes its start (the import's own steps at the
variant's `triangles`), samples the scene camera's path for training,
held-out and pruning poses, and renders the training references with their
normals into DIR. `fit`, in the GPU environment of appearance_simplify.py,
prunes the start to the recipe's budget, fits it with the recipe's settings
and writes NAME.mesh beside the import, printing the mesh's and the recipe's
SHA-256, which the variant's `fit` table then records.
"""

import argparse
import csv
import hashlib
import json
import math
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.import_settings import SettingsError, load_scene  # noqa: E402

CSV_FIELDS = ("budget", "cost_weight", "triangles", "mean_delta_e", "p95_delta_e", "predicted_ms", "board_ms")
BOARD_SCALE = 2


def placed_variant(scene, name):
    """(settings, variant) of the fitted variant `name` the scene places."""
    jobs = [item for item in scene.renderers if item.variant.name == name and item.variant.fit]
    if not jobs:
        raise SettingsError(f"the scene places no fitted variant {name!r}")
    return jobs[0].settings, jobs[0].variant


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


def recipe_digest(settings, variant, scene):
    """SHA-256 over what a fit is made from: the import's settings, the
    variant without the hashes it records, and the camera's baked tracks."""
    import json
    import tomllib

    from r3d.poses import tracks_file

    with open(settings.path, "rb") as source:
        values = tomllib.load(source)
    entry = [dict(item) for item in values.pop("variants", []) if item.get("name") == variant.name][0]
    entry["fit"] = {key: value for key, value in entry["fit"].items() if key not in ("sha256", "recipe_sha256")}
    if entry.pop("indirect", True) is False:
        values.get("process", {}).get("light", {}).pop("indirect", None)
    tracks = hashlib.sha256(tracks_file(settings, scene).read_bytes()).hexdigest()
    return hashlib.sha256(json.dumps([values, entry, tracks], sort_keys=True).encode()).hexdigest()


def prepare(scene_path, scene, settings, variant, work):
    from r3d.lit_mesh import write_lit_mesh
    from r3d.mesh_import import bake_geometry, camera_path_poses
    from r3d.reference_render import main as reference_main

    fit, visibility = variant.fit, variant.visibility or settings.visibility
    if visibility is None or visibility.source != "camera_path":
        raise SettingsError(f"{variant.name} needs camera_path visibility: its poses come from the path")
    work.mkdir(parents=True, exist_ok=True)
    geometry = bake_geometry(settings, variant, scene)
    write_lit_mesh(work, variant.name, geometry.positions, geometry.rgb, geometry.tris, geometry.tri_double, **geometry.scale)
    w, h, lens, near, poses = camera_path_poses(settings, scene, visibility, fit.train_every_ms, either_way_up=False)
    training, held_out = split_poses(fit, poses)
    (work / "train.txt").write_text(poses_text(w, h, lens, near, training))
    (work / "held_out.txt").write_text(poses_text(w, h, lens, near, held_out))
    (work / "train_landscape.txt").write_text(poses_text(h, w, lens, near, training))
    (work / "coverage.txt").write_text(poses_text(*camera_path_poses(settings, scene, visibility, fit.coverage_every_ms)))
    for poses, reference in (("train.txt", "reference"), ("train_landscape.txt", "reference_landscape"),
                             ("held_out.txt", "reference_held_out")):
        reference_main([str(scene_path), "--import", str(settings.path), "--variant", variant.name, "--poses",
                        str(work / poses), "--out", str(work / reference), "--normals"])
    log(f"prepared {variant.name}: start of {len(geometry.tris)} triangles, {len(training)} training poses")


def fit(scene_path, scene, settings, variant, work, budget=None, cost_weight=0.0, smoke=False, target=None):
    from r3d.appearance_simplify import main as fit_main

    recipe = variant.fit
    start = work / f"{variant.name}.mesh"
    out = work / "fitted"
    steps = min(recipe.steps, 8) if smoke else recipe.steps
    command = ["--scene", str(scene_path), "--start", str(start), "--poses", str(work / "train.txt"), "--reference",
              str(work / "reference"), "--poses", str(work / "train_landscape.txt"), "--reference",
              str(work / "reference_landscape"), "--out", str(out), "--budget", str(recipe.budget if budget is None else budget),
              "--coverage-poses", str(work / "coverage.txt"), "--steps", str(steps), "--batch", str(recipe.batch), "--laplacian",
              str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight)]
    if cost_weight:
        command += ["--cost-model", str(pathlib.Path(__file__).with_name("board_cost_weights.txt")), "--cost-weight", str(cost_weight)]
    fit_main(command)
    committed = target is None
    if target is None:
        target = settings.mesh_dir / f"{variant.name}.mesh"
    shutil.copyfile(out / f"{variant.name}.mesh", target)
    if committed:
        print(f"{target.name}: sha256 = \"{hashlib.sha256(target.read_bytes()).hexdigest()}\", "
              f"recipe_sha256 = \"{recipe_digest(settings, variant, scene)}\"")
    return target


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


def run_sweep_points(out, points, run_fit):
    """Run unfinished points serially; a result record is the resume marker."""
    completed = 0
    for point in points:
        point_dir = pathlib.Path(out) / point_name(point)
        result = point_dir / "result.json"
        if result.is_file():
            continue
        point_dir.mkdir(parents=True, exist_ok=True)
        row = {**point, **run_fit(point, point_dir)}
        temporary = result.with_suffix(".tmp")
        temporary.write_text(json.dumps(row, sort_keys=True) + "\n")
        temporary.replace(result)
        completed += 1
    return completed


def sweep_rows(out, points):
    """Completed rows in command-line order."""
    return [json.loads((pathlib.Path(out) / point_name(point) / "result.json").read_text()) for point in points]


def held_out_score(variant, mesh_path, work, host):
    """Mean and p95 DeltaE76 from the host renderer against held-out references."""
    from types import SimpleNamespace

    from r3d.bake_fidelity import score, write_pack
    from r3d.poses import read_poses

    _width, _height, _lens, _near, poses = read_poses(work / "held_out.txt")
    score_dir = work / "score"
    score_dir.mkdir(exist_ok=True)
    args = SimpleNamespace(render_args=f"--quarter 0 --no-hud --scene {variant.name} --frames {len(poses)} "
                                       f"--dt {variant.fit.held_out_every_ms}", reference=work / "reference_held_out",
                           reference_scale=BOARD_SCALE)
    return score(args, host, write_pack(variant.name, mesh_path, score_dir), score_dir)[:2]


def board_poses(work):
    """The held-out camera path at the fit renderer's device-pixel scale."""
    from r3d.poses import read_poses

    width, height, lens, near, poses = read_poses(work / "held_out.txt")
    path = work / "board_held_out.txt"
    path.write_text(poses_text(width * BOARD_SCALE, height * BOARD_SCALE, lens, near, poses))
    return path


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
    parser.add_argument("--variant", required=True, help="the fitted variant the scene places")
    parser.add_argument("--budgets", required=True, type=lambda text: comma_numbers(text, int))
    parser.add_argument("--cost-weights", default="0", type=lambda text: comma_numbers(text, float))
    parser.add_argument("--board-ms", type=lambda text: comma_numbers(text, float), help="measurements in budget, cost-weight order")
    parser.add_argument("--out", required=True)
    parser.add_argument("--smoke", action="store_true", help="fit eight steps at each point")
    args = parser.parse_args(argv)
    scene_path = pathlib.Path(args.scene).resolve()
    try:
        scene = load_scene(scene_path)
        settings, variant = placed_variant(scene, args.variant)
    except SettingsError as error:
        parser.error(str(error))
    points = [{"budget": budget, "cost_weight": weight} for budget in args.budgets for weight in args.cost_weights]
    if any(budget > variant.triangles for budget in args.budgets):
        parser.error("--budgets cannot exceed the variant's start triangle count")
    if args.board_ms is not None and len(args.board_ms) != len(points):
        parser.error("--board-ms needs one value per budget and cost-weight point")
    out = pathlib.Path(args.out).resolve()
    host = None

    def run_point(point, point_dir):
        nonlocal host
        if host is None:
            from r3d.bake_fidelity import build_host
            from r3d.mesh_import import REPO

            host = build_host(REPO / "launcher/main/apps/render_lab/tools/render_lab_render_host.sh", out / "host")
        work = point_dir / "work"
        prepare(scene_path, scene, settings, variant, work)
        mesh = fit(scene_path, scene, settings, variant, work, budget=point["budget"], cost_weight=point["cost_weight"],
                   smoke=args.smoke, target=point_dir / f"{variant.name}.mesh")
        mean, p95 = held_out_score(variant, mesh, work, host)
        from r3d.cost_model import load, mesh_rows, predict
        from r3d.lit_mesh import finest_triangles, read_lit_mesh

        weights, _rows, _ms, _labels = load(pathlib.Path(__file__).with_name("board_cost_weights.txt"))
        predicted = float(predict(weights, mesh_rows(mesh, board_poses(work))).mean())
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
    parser.add_argument("--mesh", required=True, help="the fitted variant's name")
    parser.add_argument("--work", required=True, help="scratch directory the two steps share")
    parser.add_argument("step", choices=("prepare", "fit"))
    args = parser.parse_args(argv)
    scene_path = pathlib.Path(args.scene).resolve()
    try:
        scene = load_scene(scene_path)
        settings, variant = placed_variant(scene, args.mesh)
    except SettingsError as error:
        parser.error(str(error))
    work = pathlib.Path(args.work).resolve()
    if args.step == "prepare":
        prepare(scene_path, scene, settings, variant, work)
    else:
        fit(scene_path, scene, settings, variant, work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
