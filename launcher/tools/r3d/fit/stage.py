"""The fit stage of a fitted renderer (bake/bake.py STAGES): its start and its reference set in, its mesh out.

fitted_variant.py prepares the start and the references; this fits the start to them with the renderer's
recipe (appearance_simplify.py) and writes the renderer's mesh. Everything the fit reads is a named input:
the start's bytes and the reference set (training, held-out and pruning poses), both in the fit's bake key.
"""

import pathlib
import shutil

from r3d import mesh_bundle, log
from r3d.import_settings import SettingsError

# The reference set's files the fit reads (fitted_variant.prepare_references writes them): the training
# (poses, references) pairs, the poses the prune counts shown pixels over, and the held-out poses the
# copy-coverage gate checks.
TRAINING = (("train.txt", "reference"), ("train_landscape.txt", "reference_landscape"))
COVERAGE = "coverage.txt"
HELD_OUT = "held_out.txt"
# A carved start's rest is fitted with the reference G-buffer terms (fit/gbuffer.py) at these weights: the front
# term pushes it behind the copies, the depth term holds it to the source's surface.
FRONT_WEIGHT = 0.0
DEPTH_WEIGHT = 0.0


def fit(scene_path, scene, job, work, budget=None, cost_weight=0.0, smoke=False, target=None, inputs=None,
        start=None):
    """Fit `start` (the variant's start in `inputs` when None) to the reference set in `inputs` (`work` when
    None) and write the renderer's mesh to `target`. A carved start bundle fits its rest with its copies drawn,
    and passes the copy-coverage gate over the held-out poses; its copies are written back unchanged."""
    from r3d.appearance_simplify import main as fit_main

    renderer = job.renderer
    variant, recipe = renderer.variant, renderer.fit
    inputs = pathlib.Path(work) if inputs is None else pathlib.Path(inputs)
    start = inputs / f"{variant.name}.mesh" if start is None else pathlib.Path(start)
    bundle_entries = copies = None
    bundle = start
    if start.suffix == mesh_bundle.SUFFIX:
        bundle_entries, copies = mesh_bundle.decode(start.read_bytes())
        rest = next((data for name, kind, data in bundle_entries if name == job.asset_name), None)
        if rest is None:
            raise SettingsError("a fitted carved renderer needs a rest mesh")
        start = pathlib.Path(work) / f"{job.asset_name}.mesh"
        start.write_bytes(rest)
    out = work / "fitted"
    steps = min(recipe.steps, 8) if smoke else recipe.steps
    command = ["--scene", str(scene_path), "--start", str(start)]
    for poses, reference in TRAINING:
        command += ["--poses", str(inputs / poses), "--reference", str(inputs / reference)]
    command += ["--out", str(out), "--budget", str(recipe.budget if budget is None else budget),
                "--coverage-poses", str(inputs / COVERAGE), "--steps", str(steps), "--batch", str(recipe.batch),
                "--laplacian", str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight),
                "--object", job.object.name, "--surface-weight", str(recipe.surface_weight),
                "--surface-margin", str(recipe.surface_margin)]
    if copies:
        command += ["--copies", str(bundle), "--copy-poses", str(inputs / HELD_OUT),
                    "--front-weight", str(FRONT_WEIGHT), "--depth-weight", str(DEPTH_WEIGHT)]
    if cost_weight:
        command += ["--cost-model", str(pathlib.Path(__file__).parents[1] / "board_cost_weights.txt"),
                    "--cost-weight", str(cost_weight)]
    fit_main(command)
    if target is None:
        target = pathlib.Path(work) / f"{job.asset_name}.mesh"
    if bundle_entries is None:
        shutil.copyfile(out / start.name, target)
    else:
        fitted = (out / start.name).read_bytes()
        target.write_bytes(mesh_bundle.encode([(name, kind, fitted if name == job.asset_name else data)
                                               for name, kind, data in bundle_entries], copies))
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
