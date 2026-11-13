"""The fit stage of a fitted renderer (bake/bake.py STAGES): its start and its reference set in, its mesh out.

fitted_variant.py prepares the start and the references; this fits the start to them with the renderer's
recipe (appearance_simplify.py) and writes the renderer's mesh. Everything the fit reads is a named input:
the start's bytes and the reference set (training, held-out and pruning poses), both in the fit's bake key.
"""

import pathlib

from r3d import mesh_bundle, mesh_asset, log
from r3d.reference_set import fit_sets, shapes_fit

# A carved start's rest is fitted with the reference G-buffer terms (fit/gbuffer.py) at these weights: the front
# term pushes it behind the copies, the depth term holds it to the source's surface. Picked on street_fitted: front
# 1e4 cuts the hidden copy pixels about tenfold; depth 30 then lowers held-out dE, heavier depth raised it.
FRONT_WEIGHT = 1.0e4
DEPTH_WEIGHT = 30.0


def prune_budget(ceiling, floor, copies):
    """The triangles the fit prunes its rest to: the ceiling less the copy shapes' triangles (each once), never
    below the recipe's budget `floor`, the room the carve left the rest (mesh_import.carve_room); the recipe's
    budget when the variant has no ceiling."""
    return floor if ceiling is None else max(floor, ceiling - copies)


def fit(scene_path, scene, job, work, budget=None, cost_weight=0.0, smoke=False, target=None, inputs=None,
        start=None):
    """Fit `start` (the variant's start in `inputs` when None) to the reference set in `inputs` (`work` when
    None) and write the renderer's mesh to `target`. A carved start bundle fits its rest with its copies drawn,
    and passes the copy-coverage gate over the held-out poses; its copies are written back unchanged. The
    variant's ceiling caps the fitted bundle, not the start: the rest is pruned to the ceiling less the copy
    shapes (each once), or to the recipe's budget when the variant has no ceiling; `budget` overrides both."""
    from r3d.appearance_simplify import main as fit_main
    from r3d.fit.copies import finest_triangles
    from r3d.simplify import check_ceiling

    renderer = job.renderer
    variant, recipe = renderer.variant, renderer.fit
    inputs = pathlib.Path(work) if inputs is None else pathlib.Path(inputs)
    start = inputs / f"{variant.name}.bundle" if start is None else pathlib.Path(start)
    bundle = start
    bundle_entries, copies = mesh_bundle.decode(start.read_bytes())
    bundle_entries = mesh_bundle.rename_rest(bundle_entries, copies, job.asset_name)
    rest = next(data for name, kind, data in bundle_entries if name == job.asset_name)
    shapes = [row["mesh"] for row in copies]
    ceiling = getattr(variant, "ceiling", None)
    if budget is None:
        budget = prune_budget(ceiling, recipe.budget, finest_triangles(bundle_entries, shapes))
    start = pathlib.Path(work) / f"{job.asset_name}{mesh_bundle.SUFFIX}"
    start.write_bytes(mesh_bundle.encode([(job.asset_name, mesh_asset.TYPE, rest)], []))
    out = work / "fitted"
    steps = min(recipe.steps, 8) if smoke else recipe.steps
    shaping = job.settings.fit
    command = ["--scene", str(scene_path), "--start", str(start)]
    sets = fit_sets(scene.camera.component.path is not None, shaping.probes is not None, shapes_fit(shaping))
    for poses, reference in sets.training:
        command += ["--poses", str(inputs / poses), "--reference", str(inputs / reference)]
    for poses in sets.coverage:
        command += ["--coverage-poses", str(inputs / poses)]
    # A fit shaped by the import counts its holes over the held-out views; settle picks its budget by them.
    for poses, reference in sets.scored:
        command += ["--held-out-poses", str(inputs / poses), "--held-out-reference", str(inputs / reference)]
    if shaping.budget is not None:
        command += ["--settle", str(shaping.budget.step), str(shaping.budget.settle)]
    command += ["--out", str(out), "--budget", str(budget),
                "--steps", str(steps), "--batch", str(recipe.batch),
                "--laplacian", str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight),
                "--object", job.object.name, "--surface-weight", str(recipe.surface_weight),
                "--surface-margin", str(recipe.surface_margin)]
    if copies:
        # The copy gate checks the path's held-out views, or the probes' when the camera has no path.
        command += ["--copies", str(bundle), "--copy-poses", str(inputs / sets.held_out[0][0]),
                    "--front-weight", str(FRONT_WEIGHT), "--depth-weight", str(DEPTH_WEIGHT)]
    if cost_weight:
        command += ["--cost-model", str(pathlib.Path(__file__).parents[1] / "board_cost_weights.txt"),
                    "--cost-weight", str(cost_weight)]
    fit_main(command)
    if target is None:
        target = pathlib.Path(work) / f"{job.asset_name}.bundle"
    fitted_entries, _ = mesh_bundle.decode((out / start.name).read_bytes())
    fitted = next(data for name, kind, data in fitted_entries if name == job.asset_name)
    entries = [(name, kind, fitted if name == job.asset_name else data) for name, kind, data in bundle_entries]
    check_ceiling(finest_triangles(entries, [*shapes, job.asset_name]), ceiling)
    target.write_bytes(mesh_bundle.encode(entries, copies))
    log(f"wrote {target}")
    return target


def fit_point(point, point_dir, scene_path, scene, job, inputs, smoke=False, target=None, recorder=None):
    import contextlib
    import traceback
    from r3d.process_budget import NULL_RECORDER
    from r3d.lit_mesh import read_lit_mesh
    target = pathlib.Path(point_dir).parent / f"{pathlib.Path(point_dir).name}.bundle" if target is None else target
    log_path = pathlib.Path(point_dir) / "fit.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    recording = recorder is not None
    recorder = NULL_RECORDER if recorder is None else recorder
    triangles = len(read_lit_mesh(pathlib.Path(inputs) / f"{job.renderer.variant.name}.bundle").tris) if recording else 0
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
