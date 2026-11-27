"""The fit stage of a fitted renderer (bake/bake.py STAGES): its start and its reference set in, its mesh out.

fitted_variant.py prepares the start and the references; this fits the start to them with the renderer's
recipe (appearance_simplify.py) and writes the renderer's mesh. Everything the fit reads is a named input:
the start's bytes and the reference set (training, held-out and pruning poses), both in the fit's bake key.
"""

import pathlib
from types import SimpleNamespace

from r3d import mesh_bundle, mesh_asset, log
from r3d.reference_set import fit_sets
from r3d.fit.copies import drawn_copies, hidden_materials

# The G-buffer front term pushes the rest behind copies; depth holds it to the source surface. Probe fits also
# use independent close geometry views: colour training views alone can miss rest movement over a nearby copy.
FRONT_WEIGHT = 3.0e4
DEPTH_WEIGHT = 30.0


def fit(scene_path, scene, job, work, budget=None, cost_weight=0.0, smoke=False, target=None, inputs=None,
        start=None):
    """Fit `start` (the variant's start in `inputs` when None) to the reference set in `inputs` (`work` when
    None) and write the renderer's mesh to `target`. A carved start bundle fits its rest with its copies drawn,
    and passes the copy-coverage gate over the held-out poses; its copies are written back unchanged. The
    variant's ceiling caps the fitted bundle. A probe fit keeps every start triangle and caps the split rest
    at the variant's triangles. Other fits prune to the ceiling less the copy prototypes, or to the recipe's
    budget when the variant has no ceiling. `budget` overrides those pruning targets."""
    from r3d.appearance_simplify import main as fit_main
    from r3d.objects.budget import bundle_count, ceiling_split, rest_target
    from r3d.import_settings import SettingsError
    from r3d.simplify import check_ceiling

    if budget is not None and job.settings.fit.probes is not None:
        raise ValueError("sweep budget is refused with geometry.probes; the variant's triangles is the budget")
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
    copied = bundle_count(bundle_entries, shapes)
    if budget is None and job.settings.fit.probes is None:
        budget = rest_target(ceiling_split(renderer), copied)
    if budget is not None and budget < 1:
        raise SettingsError(f"copies total {copied} leave the fitted rest no triangle under ceiling {ceiling}")
    start = pathlib.Path(work) / f"{job.asset_name}{mesh_bundle.SUFFIX}"
    start.write_bytes(mesh_bundle.encode([(job.asset_name, mesh_asset.TYPE, rest)], []))
    out = work / "fitted"
    steps = min(recipe.steps, 8) if smoke else recipe.steps
    shaping = job.settings.fit
    command = ["--scene", str(scene_path), "--start", str(start),
               "--hidden-materials", *sorted(hidden_materials(bundle_entries, copies))]
    sets = fit_sets(scene.camera.component.path is not None, shaping.probes is not None)
    for poses, reference in sets.training:
        command += ["--poses", str(inputs / poses), "--reference", str(inputs / reference)]
    for poses in sets.coverage:
        command += ["--coverage-poses", str(inputs / poses)]
    # Every fit logs its held-out dE76; a probe fit also gates the surface it loses there.
    for poses, reference in sets.held_out:
        command += ["--held-out-poses", str(inputs / poses), "--held-out-reference", str(inputs / reference)]
    if shaping.probes is not None:
        command += ["--closure"]
    if budget is not None:
        command += ["--budget", str(budget)]
    command += ["--out", str(out),
                "--steps", str(steps), "--batch", str(recipe.batch),
                "--laplacian", str(recipe.laplacian), "--normal-weight", str(recipe.normal_weight),
                "--object", job.object.name, "--surface-weight", str(recipe.surface_weight),
                "--surface-margin", str(recipe.surface_margin)]
    if drawn_copies(bundle_entries, copies):
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
    if shaping.probes is not None:
        rest_name = [job.asset_name]
        fitted_count = bundle_count(fitted_entries, rest_name)
        if fitted_count > variant.triangles:
            start_count = bundle_count([(job.asset_name, mesh_asset.TYPE, rest)], rest_name)
            raise SettingsError(f"{variant.name}: start rest {start_count}, split added {fitted_count - start_count}, "
                                f"fitted rest {fitted_count} exceeds triangles {variant.triangles} "
                                f"with split_reserve {variant.split_reserve}; raise split_reserve")
    fitted = next(data for name, kind, data in fitted_entries if name == job.asset_name)
    entries = [(name, kind, fitted if name == job.asset_name else data) for name, kind, data in bundle_entries]
    check_ceiling(bundle_count(entries, [*shapes, job.asset_name]), ceiling)
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
