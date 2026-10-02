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
and writes NAME.mesh beside the import, printing the SHA-256 the recipe then
records.
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


def either_way(width, height, lens, near, poses):
    """The poses as a square view as wide as the long side, so what pruning
    counts covers the panel held either way up."""
    side = max(width, height)
    return side, side, lens * side / min(width, height), near, poses


def prepare(scene_path, scene, settings, variant, work):
    from r3d.lit_mesh import write_lit_mesh
    from r3d.mesh_import import bake_geometry
    from r3d.poses import sample_camera_path
    from r3d.reference_render import main as reference_main

    fit, camera = variant.fit, scene.camera.component
    work.mkdir(parents=True, exist_ok=True)
    geometry = bake_geometry(settings, variant, scene)
    write_lit_mesh(work, variant.name, geometry.positions, geometry.rgb, geometry.tris, geometry.tri_double, **geometry.scale)
    tracks = settings.out_dir / f"{camera.path.tracks}_tracks_generated.c"
    width, height = settings.visibility.size if settings.visibility and settings.visibility.source == "camera_path" else (184, 224)

    def sampled(every_ms):
        return sample_camera_path(tracks, camera.path.tracks, camera.path.node, every_ms, width, height, camera.half_fov_short_tan,
                                  camera.near_z)

    w, h, lens, near, poses = sampled(fit.train_every_ms)
    training, held_out = split_poses(fit, poses)
    (work / "train.txt").write_text(poses_text(w, h, lens, near, training))
    (work / "held_out.txt").write_text(poses_text(w, h, lens, near, held_out))
    (work / "coverage.txt").write_text(poses_text(*either_way(*sampled(fit.coverage_every_ms))))
    reference_main([str(scene_path), "--import", str(settings.path), "--poses", str(work / "train.txt"),
                    "--out", str(work / "reference"), "--normals"])
    log(f"prepared {variant.name}: start of {len(geometry.tris)} triangles, {len(training)} training poses")


def fit(scene_path, settings, variant, work):
    from r3d.appearance_simplify import main as fit_main

    recipe = variant.fit
    start = work / f"{variant.name}.mesh"
    out = work / "fitted"
    fit_main(["--scene", str(scene_path), "--start", str(start), "--poses", str(work / "train.txt"), "--reference",
              str(work / "reference"), "--out", str(out), "--budget", str(recipe.budget), "--coverage-poses",
              str(work / "coverage.txt"), "--steps", str(recipe.steps), "--batch", str(recipe.batch), "--laplacian",
              str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight)])
    target = settings.mesh_dir / f"{variant.name}.mesh"
    shutil.copyfile(out / f"{variant.name}.mesh", target)
    print(f"{target.name}: sha256 = \"{hashlib.sha256(target.read_bytes()).hexdigest()}\"")


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
        fit(scene_path, settings, variant, work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
