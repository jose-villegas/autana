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
import json
import pathlib
import shutil
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.import_settings import SettingsError, load_scene  # noqa: E402


def placed_variant(scene, name):
    """The fitted renderer named by its variant or scene output."""
    jobs = [item for item in scene.renderers if name in (item.renderer.variant.name, item.asset_name) and item.renderer.fit]
    if not jobs:
        raise SettingsError(f"the scene places no fitted variant {name!r}")
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


def recipe_digest(settings, renderer, scene):
    """SHA-256 over the parsed effective recipe and its camera tracks."""
    from r3d.poses import tracks_file

    tracks = hashlib.sha256(tracks_file(settings, scene).read_bytes()).hexdigest()
    settings = SimpleNamespace(**{name: value for name, value in vars(settings).items()
                                  if name not in ("path", "out_dir", "mesh_dir", "named", "variants")})
    fit = SimpleNamespace(**vars(renderer.fit))
    del fit.sha256
    del fit.recipe_sha256
    entry = SimpleNamespace(**{name: value for name, value in vars(renderer).items() if name != "settings"})
    entry.fit = fit
    bake = SimpleNamespace(**vars(scene.bake))
    look = scene.indirect
    if not renderer.indirect:
        bake.indirect, look = None, None
    scene_recipe = SimpleNamespace(lights=scene.lights, tonemap_white=scene.tonemap_white, bake=bake, indirect=look)
    return hashlib.sha256(json.dumps([canonical(settings), canonical(entry), canonical(scene_recipe), tracks], sort_keys=True).encode()).hexdigest()


def prepare(scene_path, scene, job, work):
    from r3d.lit_mesh import write_lit_mesh
    from r3d.mesh_import import bake_geometry, camera_path_poses
    from r3d.reference_render import main as reference_main

    settings, renderer = job.settings, job.renderer
    variant, fit, visibility = renderer.variant, renderer.fit, renderer.visibility
    if visibility is None or visibility.source != "camera_path":
        raise SettingsError(f"{variant.name} needs camera_path visibility: its poses come from the path")
    work.mkdir(parents=True, exist_ok=True)
    geometry = bake_geometry(settings, renderer, scene)
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


def fit(scene_path, scene, job, work):
    from r3d.appearance_simplify import main as fit_main

    settings, renderer = job.settings, job.renderer
    variant, recipe = renderer.variant, renderer.fit
    start = work / f"{variant.name}.mesh"
    out = work / "fitted"
    fit_main(["--scene", str(scene_path), "--start", str(start), "--poses", str(work / "train.txt"), "--reference",
              str(work / "reference"), "--poses", str(work / "train_landscape.txt"), "--reference",
              str(work / "reference_landscape"), "--out", str(out), "--budget", str(recipe.budget), "--coverage-poses",
              str(work / "coverage.txt"), "--steps", str(recipe.steps), "--batch", str(recipe.batch), "--laplacian",
              str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight)])
    target = job.asset_path
    shutil.copyfile(out / f"{variant.name}.mesh", target)
    print(f"{target.name}: sha256 = \"{hashlib.sha256(target.read_bytes()).hexdigest()}\", "
          f"recipe_sha256 = \"{recipe_digest(settings, renderer, scene)}\"")


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
