#!/usr/bin/env python3
"""Make a fitted variant from the recipe its import file records.

    python launcher/tools/r3d/fitted_variant.py SCENE.scene.toml --mesh NAME --work DIR prepare
    python launcher/tools/r3d/fitted_variant.py SCENE.scene.toml --mesh NAME --work DIR fit

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
import hashlib
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.import_settings import SettingsError, load_scene  # noqa: E402


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
    for poses, reference in (("train.txt", "reference"), ("train_landscape.txt", "reference_landscape")):
        reference_main([str(scene_path), "--import", str(settings.path), "--variant", variant.name, "--poses",
                        str(work / poses), "--out", str(work / reference), "--normals"])
    log(f"prepared {variant.name}: start of {len(geometry.tris)} triangles, {len(training)} training poses")


def fit(scene_path, scene, settings, variant, work):
    from r3d.appearance_simplify import main as fit_main

    recipe = variant.fit
    start = work / f"{variant.name}.mesh"
    out = work / "fitted"
    fit_main(["--scene", str(scene_path), "--start", str(start), "--poses", str(work / "train.txt"), "--reference",
              str(work / "reference"), "--poses", str(work / "train_landscape.txt"), "--reference",
              str(work / "reference_landscape"), "--out", str(out), "--budget", str(recipe.budget), "--coverage-poses",
              str(work / "coverage.txt"), "--steps", str(recipe.steps), "--batch", str(recipe.batch), "--laplacian",
              str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight)])
    target = settings.mesh_dir / f"{variant.name}.mesh"
    shutil.copyfile(out / f"{variant.name}.mesh", target)
    print(f"{target.name}: sha256 = \"{hashlib.sha256(target.read_bytes()).hexdigest()}\", "
          f"recipe_sha256 = \"{recipe_digest(settings, variant, scene)}\"")


def main(argv=None):
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
