"""Regenerate GPU comparisons or consume board captures with the doc block writer."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generated_blocks import apply_tables, tracked_files
from layout_measure import MEAN_RE, parse_report
from r3d.process_budget import WSL_MEMORY_REQUIRED_BYTES, WINDOWS_MEMORY_REQUIRED_BYTES
from r3d.import_settings import content_checksum, load_import_settings, source_files
from r3d.scene_asset import scene_id

HOST_SCRIPT = ROOT / "launcher/tools/render/scene_viewer.sh"
RESULTS = ROOT / "launcher/tools/results/doc_images"
WEIGHTS = ROOT / "launcher/tools/r3d/board_cost_weights.txt"
VARIANTS = ("lite", "flat", "fitted", "fitted-full", "flat-fitted")


class Fitted(NamedTuple):
    """One fitted object of the GPU stage: the prefix of its rows, the bake it is compared with, what else it
    carries (the normal and budget sweeps, the path cull, a point in the budget table) and its sheet's columns,
    (label, row) pairs."""
    name: str
    prefix: str
    baked: str
    sheet: tuple
    sweeps: bool = False
    path_cull: bool = False
    budget_point: bool = False


# The stage runs the rows in this order, so a sheet may name the rows of an earlier entry.
FITTED = (
    Fitted("_fitted", "lite", "_lite", (("GI bake", "lite-GI-bake"), ("GI fit", "lite-GI-fit")), sweeps=True),
    Fitted("_fitted_full", "full", "", (("GI bake", "full-GI-bake"), ("GI fit", "full-GI-fit")),
           path_cull=True, budget_point=True),
    Fitted("_flat_fitted", "flat", "_flat",
           (("flat bake", "flat-GI-bake"), ("lite fit", "lite-GI-fit"), ("flat fit", "flat-GI-fit"))),
)


def git_environment():
    gitfile = ROOT / ".git"
    if sys.platform == "linux" and gitfile.is_file():
        directory = gitfile.read_text().strip().removeprefix("gitdir: ")
        if len(directory) > 2 and directory[1] == ":":
            os.environ["GIT_DIR"] = "/mnt/" + directory[0].lower() + directory[2:].replace("\\", "/")
            os.environ["GIT_WORK_TREE"] = str(ROOT)


def source_stamp(root, paths):
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0" + content_checksum(path))
    return digest.hexdigest()


def current_stamp():
    names = tracked_files(ROOT, ("launcher", "scripts"))
    paths = {ROOT / name for name in names if name and
             Path(name).suffix in (".py", ".sh", ".c", ".h", ".toml", ".mesh", ".txt")}
    for name in names:
        if name.endswith(".import.toml"):
            paths.update(source_files(load_import_settings(ROOT / name)))
    return source_stamp(ROOT, paths)


def validate_mode(smoke, check):
    if smoke and check:
        raise ValueError("smoke outputs cannot check published docs")


def require_memory(available, required, side):
    if available < required:
        shortfall = required - available
        raise ValueError(f"GPU stage needs {required / 1024 ** 3:g} GiB available memory; "
                         f"{side} is short by {shortfall / 1024 ** 3:.2f} GiB")


def memory_guard():
    from r3d.process_budget import meminfo_bytes
    wsl_available = meminfo_bytes()["MemAvailable"]
    require_memory(wsl_available, WSL_MEMORY_REQUIRED_BYTES, "WSL")
    powershell = Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
    if powershell.exists():
        try:
            host_bytes = subprocess.check_output([str(powershell), "-NoProfile", "-Command",
                                                  "(Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory).AvailableBytes"],
                                                 stderr=subprocess.STDOUT)
        except (OSError, subprocess.CalledProcessError) as error:
            detail = error.output.decode(errors="replace").strip() if isinstance(error, subprocess.CalledProcessError) and error.output else str(error)
            raise ValueError(f"Windows host memory query failed (PowerShell interop): {detail}") from error
        try:
            host_available = int(host_bytes.strip())
        except ValueError as error:
            raise ValueError(f"Windows host memory query failed: not a byte count: {host_bytes.decode(errors='replace').strip()}") from error
        require_memory(host_available, WINDOWS_MEMORY_REQUIRED_BYTES, "Windows host")


def run(command, log):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as output:
        subprocess.run([str(value) for value in command], cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, check=True)


def markdown(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    return "\n".join(lines + ["| " + " | ".join(map(str, row)) + " |" for row in rows]) + "\n"


MACHINE_FIELDS = ("CPU", "Cores/threads", "RAM", "GPU", "VRAM", "Driver", "CUDA", "OS", "Python", "Mitsuba", "PyTorch")


def machine_probes():
    import platform
    import importlib.metadata
    from r3d.process_budget import meminfo_bytes
    import torch
    cpuinfo = Path("/proc/cpuinfo").read_text()
    cpu = next(line.split(":", 1)[1].strip() for line in cpuinfo.splitlines() if line.startswith("model name"))
    cores = set()
    for block in cpuinfo.split("\n\n"):
        fields = dict(line.split(":", 1) for line in block.splitlines() if ":" in line)
        fields = {key.strip(): value.strip() for key, value in fields.items()}
        if "core id" in fields:
            cores.add((fields.get("physical id", "0"), fields["core id"]))
    gpu = subprocess.check_output(["nvidia-smi", "--query-gpu=name,memory.total,driver_version",
                                   "--format=csv,noheader,nounits"], text=True, timeout=10).strip().splitlines()
    devices = [tuple(value.strip() for value in line.split(",")) for line in gpu]
    values = {"CPU": cpu, "Cores/threads": f"{len(cores) or 'unknown'}/{os.cpu_count()}",
              "RAM": f"{meminfo_bytes()['MemTotal'] / 1024 ** 3:.1f} GiB",
              "GPU": "; ".join(row[0] for row in devices),
              "VRAM": "; ".join(row[1] + " MiB" for row in devices),
              "Driver": "; ".join(row[2] for row in devices), "CUDA": torch.version.cuda,
              "OS": platform.platform(), "Python": platform.python_version(),
              "Mitsuba": importlib.metadata.version("mitsuba"), "PyTorch": torch.__version__}
    return {key: (lambda value=value: value) for key, value in values.items()}


def machine_table(probes=None):
    probes = machine_probes() if probes is None else probes
    return markdown(["Machine", "Value"], [(key, probes[key]()) for key in MACHINE_FIELDS])


def bake_steps_table(variants):
    def mib(value):
        return "not available" if value is None else f"{value / 1024 ** 2:.1f}"
    return markdown(["Variant", "Step", "Wall s", "Triangles in", "Triangles out", "Peak RAM MiB", "Peak VRAM MiB"],
                    [(variant, row["step"], f'{row["wall_s"]:.3f}', row["triangles_in"], row["triangles_out"],
                      mib(row["peak_ram_bytes"]), mib(row["peak_vram_bytes"]))
                     for variant, rows in variants for row in rows])


def capture_rows(path, commit, id, object_name):
    text = path.read_text(encoding="utf-8", errors="replace")
    import re
    builds = re.findall(r"\b([0-9a-f]{12,40})-diag\b", text)
    if not builds or any(not commit.startswith(build) for build in builds):
        raise ValueError(f"{path}: missing or mismatched diagnostics build identity")
    if re.search(r"\bFAIL\b", text):
        raise ValueError(f"{path}: failed suite")
    aliases = {object_name: id, **{object_name + "_" + name.replace("-", "_"): name for name in VARIANTS}}
    names = [aliases.get(match.group("name"), match.group("name")) for match in MEAN_RE.finditer(text)]
    if len(names) != len(set(names)):
        raise ValueError(f"{path}: duplicate variant means; give one --capture per run")
    return {aliases.get(name, name): value / 1000 for name, value in parse_report(path).items()
}

def board_means(captures, commit, id, object_name):
    runs = [capture_rows(path, commit, id, object_name) for path in captures]
    for path, rows in zip(captures, runs):
        missing = set((id, *VARIANTS)) - rows.keys()
        if missing:
            raise ValueError(f"{path}: missing variants {sorted(missing)}")
    return {
name: statistics.median(rows[name] for rows in runs) for name in (id, *VARIANTS)
}

def board_frames(captures, expected, id, object_name):
    import re
    pattern = rf"({re.escape(id)}|lite|{re.escape(object_name)}|{re.escape(object_name + '_lite')}) t=\s*(\d+)s .*?both cores: frame\s+(\d+)us"
    values = {}
    for capture in captures:
        found = re.findall(pattern, capture.read_text(encoding="utf-8", errors="replace"))
        parsed = [({object_name: id, object_name + "_lite": "lite"}.get(name, name), int(time), int(value) / 1000)
                  for name, time, value in found]
        keys = [(name, time) for name, time, _ in parsed]
        if len(keys) != len(expected) or set(keys) != set(expected):
            raise ValueError(f"{capture}: needs each full/lite pose exactly once")
        for name, time, value in parsed:
            values.setdefault((name, time), []).append(value)
    return {
key: statistics.median(row) for key, row in values.items()
}

def capture_viewport(path, expected):
    import re
    sizes = re.findall(r"FRAME COST .*?rendered (\d+)x(\d+)", path.read_text(encoding="utf-8", errors="replace"))
    if not sizes or any(tuple(map(int, size)) != tuple(expected) for size in sizes):
        raise ValueError(f"{path}: missing or mismatched render size")


def board(args, out, work):
    from r3d.cost_model import FEATURES, fit, mesh_rows
    from r3d.import_settings import load_scene
    from r3d.mesh_import import camera_path_poses
    from r3d.fitted_variant import placed_variant, poses_text
    import re
    import numpy as np

    commit = subprocess.check_output(["git", "rev-parse", args.build_commit or "HEAD"], cwd=ROOT).decode().strip()
    source_paths = [f":(glob)launcher/**/*.{suffix}" for suffix in ("c", "h", "mesh", "toml", "cmake")]
    source_paths += [":(glob)launcher/**/CMakeLists.txt", ":(glob)launcher/**/Kconfig*", ":(glob)launcher/sdkconfig*"]
    if subprocess.run(["git", "diff", "--quiet", commit, "--", *source_paths], cwd=ROOT).returncode:
        raise ValueError("board capture commit differs from current firmware sources")
    scene_path = args.scene
    id = scene_id(scene_path)
    means = board_means(args.capture, commit, id, args.object)
    (out / f"tables/{id}-board.md").write_text(markdown(["Mesh", "Median board ms"],
        [(name, f"{means[name]:.3f}") for name in (id, *VARIANTS)]), encoding="utf-8")
    scene = load_scene(scene_path)
    job = placed_variant(scene, args.object + "_fitted")
    every_ms = job.renderer.fit.held_out_every_ms
    poses = camera_path_poses(scene, job.renderer.visibility, every_ms, either_way_up=False)
    for capture in args.capture:
        capture_viewport(capture, poses[:2])
    frame_values = board_frames(args.capture, [(name, index * every_ms // 1000) for name in (id, "lite")
                                             for index in range(len(poses[-1]))], id, args.object)
    (work / "board-poses.txt").write_text(poses_text(*poses))
    rows, times, records = [], [], []
    for name, asset in ((id, args.object), ("lite", args.object + "_lite")):
        mesh = next(item.asset_path for item in scene.renderers if item.object.name == asset)
        features = mesh_rows(mesh, work / "board-poses.txt")
        for index, row in enumerate(features):
            ms = frame_values[(name, index * every_ms // 1000)]
            rows.append(row)
            times.append(ms)
            records.append((name, index, ms, row))
    weights = fit(np.array(rows), times)
    body = "feature " + " ".join(FEATURES) + "\nweight " + " ".join(f"{value:.9g}" for value in weights) + "\n"
    body += "".join(f"frame {name} {index} {ms:.3f} " + " ".join(f"{value:.9g}" for value in row) + "\n"
                    for name, index, ms, row in records)
    (out / "board_cost_weights.txt").write_text(body)
    (out / f"tables/{id}-board-model.md").write_text(
        markdown(["Feature", "Weight"], [(name, f"{value:.9g}") for name, value in zip(FEATURES, weights)]) + "\n" +
        markdown(["Mesh", "Pose", "Board ms", *FEATURES],
                 [(name, index, f"{ms:.3f}", *(f"{value:.9g}" for value in row)) for name, index, ms, row in records]), encoding="utf-8")
    metadata = {"commit": commit, "source": current_stamp(), "captures":
                {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in args.capture}}
    (work / "board.json").write_text(json.dumps(metadata, indent=2) + "\n")
    changed = apply_tables(ROOT, out / "tables", args.check)
    weights_changed = WEIGHTS.read_text() != body
    print(f"{'changed' if weights_changed else 'same'} {WEIGHTS.relative_to(ROOT)}")
    changed |= weights_changed
    if not args.check:
        WEIGHTS.write_bytes(body.encode())
    return int(args.check and changed)


def measure_worker(label, job, mesh, reference_inputs, work, host, weights, scene_path):
    from types import SimpleNamespace
    from r3d.bake_fidelity import score, write_assets
    from r3d.appearance_simplify import load_views, normal_error, start_mesh
    from r3d.lit_mesh import finest_triangles, read_lit_mesh
    from r3d.cost_model import predict, mesh_rows
    from r3d.fitted_variant import viewer_args
    directory = work / label
    directory.mkdir(exist_ok=True)
    from r3d.poses import read_poses
    count = len(read_poses(reference_inputs / "held_out.txt")[-1])
    settings = SimpleNamespace(render_args=viewer_args(job, count, job.renderer.fit.held_out_every_ms, scene_path),
                               reference=reference_inputs / "reference_held_out", reference_scale=2)
    metrics = score(settings, host, write_assets(job.asset_name, mesh, directory, scene_path), directory)
    views, size = load_views([(str(reference_inputs / "held_out.txt"), str(settings.reference))], 2)
    angle = normal_error(start_mesh(mesh), views, size, angle_dir=directory / "angles")
    triangles = len(finest_triangles(read_lit_mesh(mesh))[2])
    predicted = float(predict(weights, mesh_rows(mesh, reference_inputs / "held_out.txt")).mean())
    row = (label, triangles, *(f"{value:.3f}" for value in metrics[:3]), f"{angle:.3f}", f"{predicted:.3f}")
    return row, directory / "frames.avi", {"triangles": triangles, "mean_delta_e": metrics[0],
                                         "p95_delta_e": metrics[1], "predicted_ms": predicted}

def bake_worker(scene, job, path_cull, prefix, baked_name, work):
    from r3d.mesh_import import bake_geometry, camera_path_poses, write_baked
    from r3d.lit_mesh import write_lit_mesh
    baked = copy.deepcopy(next(item for item in scene.renderers if item.object.name == baked_name))
    directory = work / f"bake-{prefix}"
    directory.mkdir(exist_ok=True)
    geometry = bake_geometry(baked, scene)
    write_baked(baked, scene, directory, baked.renderer.variant.name, geometry)
    if path_cull:
        from r3d.light import visible_from_path
        from r3d.ray_query import RayQuery
        visibility = job.renderer.visibility
        width, height, lens, near, poses = camera_path_poses(scene, visibility)
        seen = visible_from_path(geometry.positions, geometry.tris, geometry.tri_double,
            RayQuery(geometry.positions, geometry.tris),
            poses, width, height, lens, near, visibility.samples, visibility.margin)
        culled_dir = work / "culled-full"
        culled_dir.mkdir(exist_ok=True)
        write_lit_mesh(culled_dir, "culled", geometry.positions, geometry.rgb, geometry.tris[seen],
                       geometry.tri_double[seen], **geometry.scale)
    return (directory / f"{baked.renderer.variant.name}.mesh",
            culled_dir / "culled.mesh" if path_cull else None, geometry.measurements)

def smoke_prepare(scene_path, scene, job, inputs):
    from r3d.mesh_import import camera_path_poses
    from r3d.fitted_variant import poses_text
    from r3d.reference_render import main as reference_main
    inputs.mkdir(exist_ok=True)
    shutil.copyfile(job.asset_path, inputs / f"{job.renderer.variant.name}.mesh")
    width, height, lens, near, poses = camera_path_poses(scene, job.renderer.visibility, 5000, either_way_up=False)
    for name in ("train", "train_landscape", "held_out", "coverage"):
        (inputs / f"{name}.txt").write_text(poses_text(32, 40, lens, near, poses[:2]))
    for name in ("reference", "reference_landscape", "reference_held_out"):
        reference_main([str(scene_path), "--object", job.object.name, "--poses", str(inputs / "train.txt"),
                        "--out", str(inputs / name), "--normals"])

def prepare_variants(executor, scene_path, scene, jobs, work, on_ready):
    from concurrent.futures import Future
    from r3d.fitted_variant import prepare
    from r3d.process_budget import PREPARE_BYTES
    ready = [Future() for job in jobs]
    prepares = [executor.submit(prepare, scene_path, scene, job, work / f"inputs-{fitted.prefix}", estimates=PREPARE_BYTES,
                                priority=True)
                for fitted, job in zip(FITTED, jobs)]
    for index, (job, future) in enumerate(zip(jobs, prepares)):
        def completed(future, index=index, job=job):
            try:
                result = future.result()
                on_ready(index, job)
                ready[index].set_result(result)
            except BaseException as error:
                ready[index].set_exception(error)
        future.add_done_callback(completed)
    return ready


# Run in a child so the stage's own process never initialises the JIT.
RAY_TRACING_CHECK = "import drjit as dr; raise SystemExit(0 if dr.has_backend(dr.JitBackend.LLVM) else 3)"


def require_ray_tracing():
    """The bakes trace their rays on Mitsuba's LLVM variant, which needs libatomic1 and libLLVM. A runner missing
    either fails here with the fix, not later inside a worker."""
    done = subprocess.run([sys.executable, "-c", RAY_TRACING_CHECK], capture_output=True, text=True)
    if done.returncode:
        lines = done.stderr.strip().splitlines()
        reason = lines[-1] if lines else "drjit found no LLVM backend"
        raise ValueError(f"the GPU stage traces rays on Mitsuba's LLVM variant ({reason}); "
                         "install libatomic1 and libLLVM, for example apt install libatomic1 llvm")


def gpu(args, out, work):
    # The parent never forks or initialises CUDA; disposable workers own heavy state.
    memory_guard()
    require_ray_tracing()
    from r3d.process_budget import TaskExecutor
    with TaskExecutor() as executor:
        return _gpu(args, out, work, executor)


def _gpu(args, out, work, executor):
    from r3d.fitted_variant import (placed_variant, plot_pareto, prepare,
                                   run_sweep_points, sweep_rows, write_sweep_csv, fit_point)
    from r3d.import_settings import load_scene
    from r3d.bake_fidelity import build_host
    from r3d.cost_model import load
    from functools import partial
    from r3d.process_budget import BAKE_BYTES, MEASURE_BYTES, SMOKE_PREPARE_BYTES, FIT_BYTES

    scene_path = args.scene
    scene = load_scene(scene_path)
    id = scene_id(scene_path)
    jobs = [placed_variant(scene, args.object + entry.name) for entry in FITTED]
    if len({job.renderer.fit.held_out_every_ms for job in jobs}) != 1:
        raise ValueError("GPU comparisons require a common held-out pose interval")
    stamp = current_stamp()
    work = work / ("smoke-" if args.smoke else "full-") / stamp
    work.mkdir(parents=True, exist_ok=True)
    inputs = work / "references"
    if args.smoke:
        job = jobs[0]
        executor.submit(smoke_prepare, scene_path, scene, job, inputs, estimates=SMOKE_PREPARE_BYTES).result()
        executor.submit(fit_point, {}, work / "fit", scene_path, scene, job, inputs, True,
                        work / "smoke.mesh", estimates=FIT_BYTES).result()
        print(f"GPU smoke: eight steps completed; scratch only: {work}")
        return 0
    host = build_host(HOST_SCRIPT, work / "host", scene_path)
    rows, comparisons, sweep = [], {}, []
    bake_steps = []
    weights, *_ = load(WEIGHTS)

    def measure(label, job, mesh, reference_inputs):
        row, frames, values = executor.submit(measure_worker, label, job, mesh, reference_inputs,
                                              work, host, weights, scene_path, estimates=MEASURE_BYTES, priority=True).result()
        rows.append(row)
        comparisons[label] = frames
        return values

    fitted_futures, normal_futures, sweep_futures = {}, {}, {}
    def prepared(index, job):
        prefix = FITTED[index].prefix
        reference_inputs = work / f"inputs-{prefix}"
        fitted_futures[prefix] = executor.submit(fit_point, {}, work / f"fit-{prefix}", scene_path, scene, job,
                                                  reference_inputs, False, work / f"{prefix}.mesh", True, estimates=FIT_BYTES)
        if FITTED[index].sweeps:
            normal_weights = list(dict.fromkeys((0.0, 0.1, 0.3, job.renderer.fit.normal_weight)))
            for normal in normal_weights:
                if normal != job.renderer.fit.normal_weight:
                    variant = copy.deepcopy(job)
                    variant.renderer.fit.normal_weight = normal
                    name = f"normal-{normal:g}"
                    normal_futures[normal] = executor.submit(fit_point, {}, work / name, scene_path, scene, variant, reference_inputs, estimates=FIT_BYTES)
            budgets = list(dict.fromkeys(budget for budget in (4000, 6000, job.renderer.fit.budget)
                                         if budget <= job.renderer.variant.triangles))
            points = [{"budget": budget, "cost_weight": cost} for budget in budgets for cost in (0.0, 0.1)]
            unfinished = [point for point in points if point["budget"] != job.renderer.fit.budget or point["cost_weight"]]
            run_sweep_points(work, unfinished, partial(fit_point, scene_path=scene_path, scene=scene, job=job,
                             inputs=reference_inputs), executor=executor, deferred=sweep_futures)

    ready = prepare_variants(executor, scene_path, scene, jobs, work, prepared)

    for index, job in enumerate(jobs):
        prepared_result = ready[index].result()
        entry = FITTED[index]
        prefix = entry.prefix
        reference_inputs = work / f"inputs-{prefix}"
        for label, baked_name in (("GI-bake", args.object + entry.baked),):
            baked_mesh, culled_mesh, measurements = executor.submit(bake_worker, scene, job, entry.path_cull, prefix, baked_name,
                                                        work, estimates=BAKE_BYTES, priority=True).result()
            bake_steps.append((f"{prefix}-{label}", measurements))
            measure(f"{prefix}-{label}", job, baked_mesh, reference_inputs)
            if entry.path_cull:
                measure("full-path-culled", job, culled_mesh, reference_inputs)
                run([sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out",
                     out / "render/gpu/appearance-path-culled.png", "--reference-bakes",
                     reference_inputs / "reference_held_out", "--reference-scale", "2", "--sheet-frames", "0,4",
                     "--bake", "full bake", comparisons["full-GI-bake"], "--bake", "path culled",
                     comparisons["full-path-culled"], "--crops", "3"], work / "path-sheet.log")
        fit_result = fitted_futures[prefix].result()
        start_steps = prepared_result["measurements"] if prepared_result else []
        bake_steps.append((f"{prefix}-fit-start", start_steps))
        bake_steps.append((f"{prefix}-GI-fit", fit_result["measurements"]))
        fitted = Path(fit_result["mesh"])
        fitted_values = measure(f"{prefix}-GI-fit", job, fitted, reference_inputs)
        if entry.budget_point:
            sweep.append({"budget": job.renderer.fit.budget, "cost_weight": 0.0, **fitted_values})
        image = out / "render/gpu" / f"appearance-indirect-{prefix}.png"
        columns = [part for label, row in entry.sheet for part in ("--bake", label, comparisons[row])]
        run([sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out", image,
             "--reference-bakes", reference_inputs / "reference_held_out", "--reference-scale", "2", "--sheet-frames", "0,4",
             *columns, "--crops", "3"], work / f"sheet-{prefix}.log")
        if entry.sweeps:
            normal_rows = []
            normal_weights = list(dict.fromkeys((0.0, 0.1, 0.3, job.renderer.fit.normal_weight)))
            for normal in normal_weights:
                variant = copy.deepcopy(job)
                variant.renderer.fit.normal_weight = normal
                name = f"normal-{normal:g}"
                mesh = fitted if normal == job.renderer.fit.normal_weight else Path(normal_futures[normal].result()["mesh"])
                measure(name, job, mesh, reference_inputs)
                normal_rows.append(rows[-1])
            (out / f"tables/{id}-normal.md").write_text(markdown(
                ["Normal weight", "Triangles", "Mean dE76", "p95 dE76", "SSIM", "Normal angle", "Predicted ms"], normal_rows) +
                "\n![Normal angle heatmaps](../images/render/gpu/appearance-normal-heat.png)\n")
            command = [sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out",
                       out / "render/gpu/appearance-normal-heat.png"]
            for normal in normal_weights:
                command += ["--angle-column", f"normal {normal:g}", work / f"normal-{normal:g}/angles"]
            run(command, work / "normal-sheet.log")
            budgets = list(dict.fromkeys(budget for budget in (4000, 6000, job.renderer.fit.budget)
                                         if budget <= job.renderer.variant.triangles))
            points = [{
    "budget" : budget, "cost_weight" : cost} for budget in budgets
                      for cost in (0.0, 0.1)]

            def run_point(point, point_dir):
                budget, cost = point["budget"], point["cost_weight"]
                name = point_dir.name
                mesh = fitted if budget == job.renderer.fit.budget and not cost else Path(sweep_futures[name].result()["mesh"])
                return measure(name, job, mesh, reference_inputs)

            run_sweep_points(work, points, run_point)
            sweep = sweep_rows(work, points)
    write_sweep_csv(out / "sweep.csv", sweep)
    plot_pareto(out / "render/gpu/appearance-pareto.png", sweep)
    (out / f"tables/{id}-budget.md").write_text(markdown(
        ["Budget", "Cost weight", "Triangles", "Held-out dE76", "Predicted ms"],
        [(row["budget"], row["cost_weight"], row["triangles"], f'{row["mean_delta_e"]:.3f}', f'{row["predicted_ms"]:.3f}')
         for row in sweep]) + "\n![Budget and cost sweep](../images/render/gpu/appearance-pareto.png)\n")
    (out / f"tables/{id}-gpu.md").write_text(markdown(
        ["Mesh", "Triangles", "Mean dE76", "p95 dE76", "SSIM", "Normal angle", "Predicted ms"], rows[:2] +
        [row for row in rows if row[0].startswith("full-")]) + "\n" + "\n".join(
        f"![{prefix} GI bake and fit{suffix}](../images/render/gpu/appearance-indirect-{prefix}{suffix}.png)"
        for prefix in ("lite", "full") for suffix in ("", ".crops")) + "\n")
    # The cost model has no shading term and cannot price a flat mesh; the board table times it.
    flat_sheet = next(entry.sheet for entry in FITTED if entry.prefix == "flat")
    measured = {row[0]: row for row in rows}
    (out / f"tables/{id}-flat-fit.md").write_text(markdown(
        ["Mesh", "Triangles", "Mean dE76", "p95 dE76", "SSIM", "Normal angle"],
        [measured[name][:-1] for _label, name in flat_sheet]) + "\n" + "\n".join(
        f"![Flat bake, lite fit and flat fit{suffix}](../images/render/gpu/appearance-indirect-flat{suffix}.png)"
        for suffix in ("", ".crops")) + "\n")
    with (out / f"tables/{id}-gpu.md").open("a") as output:
        output.write("\n![Full bake and path cull](../images/render/gpu/appearance-path-culled.png)\n")
        if (out / "render/gpu/appearance-path-culled.crops.png").exists():
            output.write("\n![Path cull differences](../images/render/gpu/appearance-path-culled.crops.png)\n")
    machine = machine_table()
    (out / "tables/bake-machine.md").write_text(machine)
    (out / "tables/bake-steps.md").write_text(bake_steps_table(bake_steps))
    (out / "measurements.json").write_text(json.dumps({"source": stamp, "rows": rows, "sweep": sweep,
                                                        "bake_steps": bake_steps, "machine": machine}, indent=2) + "\n")
    apply_tables(ROOT, out / "tables")
    for image in (out / "render/gpu").glob("*.png"):
        target = ROOT / "docs/images/render/gpu" / image.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(image, target)
    return 0


def check_gpu(out):
    manifest = out / "source.json"
    if not manifest.exists() or json.loads(manifest.read_text())["source"] != current_stamp():
        raise ValueError("GPU source changed or no full run exists; run --stage gpu without --check")
    for name, checksum in json.loads(manifest.read_text())["files"].items():
        path = out / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise ValueError(f"GPU output missing or altered: {name}; run --stage gpu")
    changed = apply_tables(ROOT, out / "tables", True)
    for image in sorted((out / "render/gpu").glob("*.png")):
        target = ROOT / "docs/images/render/gpu" / image.name
        if not target.exists():
            print(f"changed {target}: new image")
            changed = True
            continue
        result = subprocess.run([sys.executable, ROOT / "launcher/tools/render/compare_images.py", target, image],
                                capture_output=True, text=True)
        if result.returncode > 1:
            raise ValueError(result.stdout + result.stderr)
        print(f"{'changed' if result.returncode else 'same'} {target.relative_to(ROOT)}")
        changed |= bool(result.returncode)
    extras = set((ROOT / "docs/images/render/gpu").glob("*.png"))
    extras -= {ROOT / "docs/images/render/gpu" / image.name for image in (out / "render/gpu").glob("*.png")}
    for image in sorted(extras):
        print(f"orphan {image}")
        changed = True
    return int(changed)


def main(argv=None):
    git_environment()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", type=Path, required=True)
    parser.add_argument("--object", required=True)
    parser.add_argument("--stage", choices=("gpu", "board"), required=True)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--capture", type=Path, action="append", default=[])
    parser.add_argument("--build-commit", help="capture commit, default HEAD; firmware sources must still match")
    args = parser.parse_args(argv)
    try:
        validate_mode(args.smoke, args.check)
        if args.stage == "board" and (not args.capture or args.smoke):
            raise ValueError("board stage needs --capture and does not support --smoke")
        work = RESULTS / "work" / args.stage
        out = RESULTS / "out" / (args.stage + "-smoke" if args.smoke else args.stage)
        if args.stage == "gpu" and args.check:
            return check_gpu(out)
        if out.exists():
            shutil.rmtree(out)
        (out / "tables").mkdir(parents=True)
        (out / "render/gpu").mkdir(parents=True)
        work.mkdir(parents=True, exist_ok=True)
        result = gpu(args, out, work) if args.stage == "gpu" else board(args, out, work)
        if args.stage == "gpu" and not args.smoke:
            files = {path.relative_to(out).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in out.rglob("*") if path.is_file()}
            (out / "source.json").write_text(json.dumps({"source": current_stamp(), "files": files}) + "\n")
        return result
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
