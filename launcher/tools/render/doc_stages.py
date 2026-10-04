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

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generated_blocks import apply_tables
from perf_compare import MEAN_RE, parse_report

SCENE = ROOT / "launcher/main/apps/render_lab/meshes/sponza.scene.toml"
HOST_SCRIPT = ROOT / "launcher/main/apps/render_lab/tools/render_lab_render_host.sh"
RESULTS = ROOT / "launcher/tools/results/doc_images"
WEIGHTS = ROOT / "launcher/tools/r3d/board_cost_weights.txt"
WSL_MEMORY_REQUIRED_BYTES = 6 * 1024 ** 3
WINDOWS_MEMORY_REQUIRED_BYTES = 2 * 1024 ** 3
VARIANTS = ("sponza", "lite", "flat", "fitted", "fitted-full")


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
        digest.update(path.read_bytes())
    return digest.hexdigest()


def current_stamp():
    names = subprocess.check_output(["git", "ls-files", "-z", "launcher", "scripts"], cwd=ROOT).decode().split("\0")
    return source_stamp(ROOT, [ROOT / name for name in names if name and
                              Path(name).suffix in (".py", ".sh", ".c", ".h", ".toml", ".mesh", ".txt")])


def validate_mode(smoke, check):
    if smoke and check:
        raise ValueError("smoke outputs cannot check published docs")


def require_memory(available, required, side):
    if available < required:
        shortfall = required - available
        raise ValueError(f"GPU stage needs {required / 1024 ** 3:g} GiB available memory; "
                         f"{side} is short by {shortfall / 1024 ** 3:.2f} GiB")


def memory_guard():
    wsl_available = next(int(line.split()[1]) * 1024 for line in Path("/proc/meminfo").read_text().splitlines()
                         if line.startswith("MemAvailable:"))
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


def capture_rows(path, commit):
    text = path.read_text(encoding="utf-8", errors="replace")
    import re
    builds = re.findall(r"\b([0-9a-f]{12,40})-diag\b", text)
    if not builds or any(not commit.startswith(build) for build in builds):
        raise ValueError(f"{path}: missing or mismatched diagnostics build identity")
    if re.search(r"\bFAIL\b", text):
        raise ValueError(f"{path}: failed suite")
    aliases = {"atrium": "sponza", "atrium_lite": "lite", "atrium_flat": "flat",
               "atrium_fitted": "fitted", "atrium_fitted_full": "fitted-full"}
    names = [aliases.get(match.group("name"), match.group("name")) for match in MEAN_RE.finditer(text)]
    if len(names) != len(set(names)):
        raise ValueError(f"{path}: duplicate variant means; give one --capture per run")
    return {aliases.get(name, name): value / 1000 for name, value in parse_report(path).items()
}

def board_means(captures, commit):
    runs = [capture_rows(path, commit) for path in captures]
    for path, rows in zip(captures, runs):
        missing = set(VARIANTS) - rows.keys()
        if missing:
            raise ValueError(f"{path}: missing variants {sorted(missing)}")
    return {
name: statistics.median(rows[name] for rows in runs) for name in VARIANTS
}

def board_frames(captures, expected):
    import re
    pattern = r"(sponza|lite|atrium|atrium_lite) t=\s*(\d+)s .*?both cores: frame\s+(\d+)us"
    values = {}
    for capture in captures:
        found = re.findall(pattern, capture.read_text(encoding="utf-8", errors="replace"))
        parsed = [(dict(atrium="sponza", atrium_lite="lite").get(name, name), int(time), int(value) / 1000)
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
    means = board_means(args.capture, commit)
    (out / "tables/sponza-board.md").write_text(markdown(["Mesh", "Median board ms"],
        [(name, f"{means[name]:.3f}") for name in VARIANTS]), encoding="utf-8")
    scene = load_scene(SCENE)
    job = placed_variant(scene, "sponza_fitted")
    header = ROOT / "launcher/main/apps/render_lab/sponza_flythrough.h"
    interval = re.search(r"#define\s+SPONZA_POSE_EVERY_MS\s+(\d+)", header.read_text())
    if interval is None:
        raise ValueError("cannot read the board suite pose interval")
    every_ms = int(interval.group(1))
    poses = camera_path_poses(scene, job.renderer.visibility, every_ms, either_way_up=False)
    for capture in args.capture:
        capture_viewport(capture, poses[:2])
    frame_values = board_frames(args.capture, [(name, index * every_ms // 1000) for name in ("sponza", "lite")
                                             for index in range(len(poses[-1]))])
    (work / "board-poses.txt").write_text(poses_text(*poses))
    rows, times, records = [], [], []
    for name, asset in (("sponza", "atrium"), ("lite", "atrium_lite")):
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
    (out / "tables/sponza-board-model.md").write_text(
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


def gpu(args, out, work):
    from r3d.fitted_variant import (prepare, fit, placed_variant, poses_text, plot_pareto,
                                   run_sweep_points, sweep_rows, write_sweep_csv)
    from r3d.import_settings import load_scene
    from r3d.mesh_import import bake_geometry, camera_path_poses
    from r3d.lit_mesh import write_lit_mesh, read_lit_mesh, finest_triangles
    from r3d.bake_fidelity import build_host, write_pack, score
    from r3d.appearance_simplify import load_views, normal_error, start_mesh
    from r3d.cost_model import load, mesh_rows, predict
    from r3d.reference_render import main as reference_main
    from types import SimpleNamespace

    memory_guard()
    scene = load_scene(SCENE)
    jobs = [placed_variant(scene, name) for name in ("sponza_fitted", "sponza_fitted_full")]
    if len({job.renderer.fit.held_out_every_ms for job in jobs}) != 1:
        raise ValueError("GPU comparisons require a common held-out pose interval")
    stamp = current_stamp()
    work = work / ("smoke-" if args.smoke else "full-") / stamp
    work.mkdir(parents=True, exist_ok=True)
    inputs = work / "references"
    if args.smoke:
        inputs.mkdir(exist_ok=True)
        job = jobs[0]
        shutil.copyfile(job.asset_path, inputs / f"{job.renderer.variant.name}.mesh")
        width, height, lens, near, poses = camera_path_poses(scene, job.renderer.visibility, 5000, either_way_up=False)
        for name in ("train", "train_landscape", "held_out", "coverage"):
            (inputs / f"{name}.txt").write_text(poses_text(32, 40, lens, near, poses[:2]))
        for name in ("reference", "reference_landscape", "reference_held_out"):
            reference_main([str(SCENE), "--object", job.object.name, "--poses", str(inputs / "train.txt"),
                            "--out", str(inputs / name), "--normals"])
        memory_guard()
        fit(SCENE, scene, job, work / "fit", smoke=True, target=work / "smoke.mesh", inputs=inputs)
        print(f"GPU smoke: eight steps completed; scratch only: {work}")
        return 0
    host = build_host(HOST_SCRIPT, work / "host")
    rows, comparisons, sweep = [], {}, []
    weights, *_ = load(WEIGHTS)

    def measure(label, job, mesh, reference_inputs):
        directory = work / label
        directory.mkdir(exist_ok=True)
        from r3d.poses import read_poses
        count = len(read_poses(reference_inputs / "held_out.txt")[-1])
        settings = SimpleNamespace(render_args=f"--quarter 0 --no-hud --scene {job.renderer.variant.name.replace('_', '-')} "
                                              f"--frames {count} --dt {job.renderer.fit.held_out_every_ms}",
                                   reference=reference_inputs / "reference_held_out", reference_scale=2)
        metrics = score(settings, host, write_pack(job.asset_name, mesh, directory), directory)
        memory_guard()
        views, size = load_views([(str(reference_inputs / "held_out.txt"), str(settings.reference))], 2)
        angle = normal_error(start_mesh(mesh), views, size, angle_dir=directory / "angles")
        triangles = len(finest_triangles(read_lit_mesh(mesh))[2])
        predicted = float(predict(weights, mesh_rows(mesh, reference_inputs / "held_out.txt")).mean())
        row = (label, triangles, *(f"{value:.3f}" for value in metrics[:3]), f"{angle:.3f}", f"{predicted:.3f}")
        rows.append(row)
        comparisons[label] = directory / "frames.avi"
        return {
    "triangles" : triangles, "mean_delta_e" : metrics[0], "p95_delta_e" : metrics[1], "predicted_ms" : predicted}

    for index, job in enumerate(jobs):
        prefix = "lite" if index == 0 else "full"
        reference_inputs = work / f"inputs-{prefix}"
        memory_guard()
        prepare(SCENE, scene, job, reference_inputs)
        for label, baked_name in (("GI-bake", "atrium_lite" if index == 0 else "atrium"),):
            baked = copy.deepcopy(next(item for item in scene.renderers if item.object.name == baked_name))
            directory = work / f"bake-{prefix}"
            directory.mkdir(exist_ok=True)
            geometry = bake_geometry(baked, scene)
            write_lit_mesh(directory, baked.renderer.variant.name, geometry.positions, geometry.rgb, geometry.tris,
                           geometry.tri_double, **geometry.scale)
            if index == 1:
                from r3d.light import visible_from_path
                from trimesh import Trimesh
                from trimesh.ray.ray_pyembree import RayMeshIntersector
                visibility = job.renderer.visibility
                width, height, lens, near, poses = camera_path_poses(scene, visibility)
                memory_guard()
                seen = visible_from_path(geometry.positions, geometry.tris, geometry.tri_double,
                    RayMeshIntersector(Trimesh(geometry.positions, geometry.tris, process=False)),
                    poses, width, height, lens, near, visibility.samples, visibility.margin)
                culled_dir = work / "culled-full"
                culled_dir.mkdir(exist_ok=True)
                write_lit_mesh(culled_dir, "culled", geometry.positions, geometry.rgb, geometry.tris[seen],
                               geometry.tri_double[seen], **geometry.scale)
            del geometry
            measure(f"{prefix}-{label}", job, directory / f"{baked.renderer.variant.name}.mesh", reference_inputs)
            if index == 1:
                measure("full-path-culled", job, culled_dir / "culled.mesh", reference_inputs)
                run([sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out",
                     out / "render/gpu/appearance-path-culled.png", "--reference-bakes",
                     reference_inputs / "reference_held_out", "--reference-scale", "2", "--sheet-frames", "0,4",
                     "--bake", "full bake", comparisons["full-GI-bake"], "--bake", "path culled",
                     comparisons["full-path-culled"], "--crops", "3"], work / "path-sheet.log")
        memory_guard()
        fitted = fit(SCENE, scene, job, work / f"fit-{prefix}", target=work / f"{prefix}.mesh", inputs=reference_inputs)
        fitted_values = measure(f"{prefix}-GI-fit", job, fitted, reference_inputs)
        if index == 1:
            sweep.append({"budget": job.renderer.fit.budget, "cost_weight": 0.0, **fitted_values})
        image = out / "render/gpu" / f"appearance-indirect-{prefix}.png"
        run([sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out", image,
             "--reference-bakes", reference_inputs / "reference_held_out", "--reference-scale", "2", "--sheet-frames", "0,4",
             "--bake", "GI bake", comparisons[f"{prefix}-GI-bake"], "--bake", "GI fit", comparisons[f"{prefix}-GI-fit"],
             "--crops", "3"], work / f"sheet-{prefix}.log")
        if index == 0:
            normal_rows = []
            normal_weights = list(dict.fromkeys((0.0, 0.1, 0.3, job.renderer.fit.normal_weight)))
            for normal in normal_weights:
                variant = copy.deepcopy(job)
                variant.renderer.fit.normal_weight = normal
                name = f"normal-{normal:g}"
                memory_guard()
                mesh = fitted if normal == job.renderer.fit.normal_weight else fit(
                    SCENE, scene, variant, work / name, target=work / f"{name}.mesh", inputs=reference_inputs)
                measure(name, job, mesh, reference_inputs)
                normal_rows.append(rows[-1])
            (out / "tables/sponza-normal.md").write_text(markdown(
                ["Normal weight", "Triangles", "Mean dE76", "p95 dE76", "SSIM", "Normal angle", "Predicted ms"], normal_rows) +
                "\n![Normal angle heatmaps](../../../../../docs/images/render/gpu/appearance-normal-heat.png)\n")
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
                memory_guard()
                mesh = fitted if budget == job.renderer.fit.budget and not cost else fit(
                    SCENE, scene, job, point_dir, budget=budget, cost_weight=cost,
                    target=work / f"{name}.mesh", inputs=reference_inputs)
                return measure(name, job, mesh, reference_inputs)

            run_sweep_points(work, points, run_point)
            sweep = sweep_rows(work, points)
    write_sweep_csv(out / "sweep.csv", sweep)
    plot_pareto(out / "render/gpu/appearance-pareto.png", sweep)
    (out / "tables/sponza-budget.md").write_text(markdown(
        ["Budget", "Cost weight", "Triangles", "Held-out dE76", "Predicted ms"],
        [(row["budget"], row["cost_weight"], row["triangles"], f'{row["mean_delta_e"]:.3f}', f'{row["predicted_ms"]:.3f}')
         for row in sweep]) + "\n![Budget and cost sweep](../../../../../docs/images/render/gpu/appearance-pareto.png)\n")
    (out / "tables/sponza-gpu.md").write_text(markdown(
        ["Mesh", "Triangles", "Mean dE76", "p95 dE76", "SSIM", "Normal angle", "Predicted ms"], rows[:2] +
        [row for row in rows if row[0].startswith("full-")]) + "\n" + "\n".join(
        f"![{prefix} GI bake and fit{suffix}](../../../../../docs/images/render/gpu/appearance-indirect-{prefix}{suffix}.png)"
        for prefix in ("lite", "full") for suffix in ("", ".crops")) + "\n")
    with (out / "tables/sponza-gpu.md").open("a") as output:
        output.write("\n![Full bake and path cull](../../../../../docs/images/render/gpu/appearance-path-culled.png)\n")
        if (out / "render/gpu/appearance-path-culled.crops.png").exists():
            output.write("\n![Path cull differences](../../../../../docs/images/render/gpu/appearance-path-culled.crops.png)\n")
    (out / "measurements.json").write_text(json.dumps({"source": stamp, "rows": rows, "sweep": sweep}, indent=2) + "\n")
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
