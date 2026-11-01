"""Regenerate measured render documentation from GPU runs or board captures."""
import argparse
import ast
import copy
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import threading
from concurrent.futures import Future
from typing import NamedTuple
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
INPUTS_PYTHON_MINIMUM = tuple(map(int, (ROOT / "scripts/lib/python-min-version").read_text().strip().split(".")))
TOOLS = ROOT / "launcher/tools"
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture import capture_board, require_same_board
from frame_profile import poses
from frame_cost import US_PER_MS
from build import compile_db
from generated_blocks import apply_tables, tracked_files
from doc_tables import markdown
from layout_measure import parse_timings, MEAN_RE
from r3d.process_budget import WSL_MEMORY_REQUIRED_BYTES, WINDOWS_MEMORY_REQUIRED_BYTES
from r3d.import_settings import content_checksum, load_import_settings, source_files
from r3d.scene_name import scene_id

HOST_SCRIPT = ROOT / "launcher/tools/render/scene_viewer.sh"
RESULTS = ROOT / "launcher/tools/results/doc_images"
WEIGHTS = ROOT / "launcher/tools/r3d/board_cost_weights.txt"
VARIANTS = ("lite", "flat", "fitted", "fitted-full", "flat-fitted")
PATH_CULL_SAMPLING = SimpleNamespace(every_ms=100, size=(184, 224), samples=3, margin=8)


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


class LightSheet(NamedTuple):
    """One sheet of what a light option buys, on the scene's showcase fitted under each look of physical_scene.py:
    the look whose reference heads it, its (label, look) columns, the held-out frames it shows, how many crops it
    enlarges (0: none) and whether each column also carries its own look's reference."""
    name: str
    reference: str
    columns: tuple
    frames: str = "0,4"
    crops: int = 4
    own_references: bool = False


LIGHT_SHEETS = (
    LightSheet("bake-indirect-compare", "smooth", (("direct light only", "direct"), ("two bounces", "smooth"))),
    LightSheet("bake-indirect-look", "smooth", (("intensity 1", "smooth"), ("intensity 2", "intensity-2"),
               ("intensity 3", "intensity-3"), ("albedo boost 2", "boost-2")), frames="4", crops=0, own_references=True),
    LightSheet("bake-ao-compare", "ao-occluded", (("no occlusion", "ao-flat"), ("occlusion", "ao-occluded"))),
)
# The occlusion factor beside the reference, at the poses and from the reference look of the occlusion sheet.
AO_MAP = SimpleNamespace(name="bake-ao-map", sheet=next(sheet for sheet in LIGHT_SHEETS if sheet.name == "bake-ao-compare"))


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
             Path(name).suffix in (".py", ".sh", ".c", ".h", ".toml", ".mesh", ".txt", ".lock")}
    for name in names:
        if name.endswith(".import.toml"):
            paths.update(source_files(load_import_settings(ROOT / name)))
    return source_stamp(ROOT, paths)


def named_files(path, tracked):
    """The tracked files a module names by a string, resolved as its code would: from the repository, the
    tools folder or its own folder."""
    from bake import bake
    for node in ast.walk(bake.module_facts(path)[0]):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.strip():
            for base in (ROOT, TOOLS, path.parent):
                try:
                    candidate = (base / node.value).resolve()
                except (OSError, ValueError):
                    continue
                if candidate in tracked:
                    yield candidate
                    break


def host_inputs(scene_path):
    """Host CMake inputs, compile flags and the compiler's -MM list, repo-relative.
    System headers and compiler identity/version are not counted."""
    def relative(token):
        if not Path(token).is_absolute():
            return token
        path = Path(token).resolve()
        if not path.is_relative_to(ROOT):
            raise ValueError(f"the host build reads {token}, outside the repository")
        return path.relative_to(ROOT).as_posix()

    listing = subprocess.run(["sh", HOST_SCRIPT, scene_path, "--deps"], cwd=ROOT, capture_output=True,
                             text=True, check=True).stdout
    flags, _, deps = listing.partition("\n")
    if not flags.startswith("flags "):
        raise ValueError(f"{HOST_SCRIPT.name} --deps printed no flags line")
    files = {relative(token) for token in compile_db.parse_dependencies(deps, directory=ROOT)}
    files.update(("launcher/tools/render/CMakeLists.txt", "launcher/tools/render/scene/CMakeLists.txt"))
    return {"host flags": " ".join(relative(token) for token in flags.split()[1:]),
            **{f"host {name}": content_checksum(ROOT / name).decode() for name in sorted(files)}}


def gpu_inputs(scene_path, object_name):
    """{input: digest} of everything the GPU stage's output is made from: the Python it runs (the import
    closure of this file and of each script its closure names, until none is new, with the C and
    requirements bake.py counts for them), the other tracked files that code names, the host renderer's
    compile, the scene file, its non-bake pack entries, and the key and locked bytes of every bake the
    scene holds. Its digest (fingerprint) names the stage's run."""
    from bake import bake
    tracked = {(ROOT / name).resolve() for name in tracked_files(ROOT, ("launcher", "scripts"))}
    entries = {Path(__file__).resolve()}
    while True:
        code = bake.closure(sorted(entries), consumers=True)
        named = {file for path in code for file in named_files(path, tracked)}
        scripts = {file for file in named if file.suffix == ".py"} - entries
        if not scripts:
            break
        entries |= scripts

    def name(path):
        return path.resolve().relative_to(ROOT).as_posix()

    from r3d.build_pack import pack_jobs
    packs, jobs = pack_jobs([scene_path])
    lock = bake.read_lock()
    inputs = {"scene": f"{name(scene_path)} {content_checksum(scene_path).decode()}", "object": object_name,
              **{f"code {name(path)}": bake.digest(bake.code_tokens(path)) for path in code},
              **{f"imports {name(path)}": bake.digest([
                  (node.module if isinstance(node, ast.ImportFrom) else None,
                   node.level if isinstance(node, ast.ImportFrom) else 0, alias.name, alias.asname)
                  for node in ast.walk(bake.module_facts(path)[0])
                  if isinstance(node, (ast.Import, ast.ImportFrom)) for alias in node.names]) for path in code},
              **{f"pack {entry}": content_checksum(source).decode()
                 for pack in packs.values() for entry, source in pack.items() if source.resolve() not in jobs},
              **{f"native {path}": value for path, value in bake.native_inputs(code).items()},
              **{f"requirement {file} {line}": line for file, line in bake.pinned_requirements(code)},
              **{f"file {name(path)}": content_checksum(path).decode() for path in sorted(named - set(code))},
              **host_inputs(scene_path)}
    for found in bake.bakes([scene_path]):
        row = lock.get(found.key)
        inputs[f"bake {found.output}"] = f"{found.key} {row['sha256']}" if row else f"unlocked {found.key}"
    return inputs


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



BAKE_STEPS_CHART = "bake-steps.png"
MACHINE_FIELDS = ("CPU", "Cores/threads", "RAM", "GPU", "VRAM", "Driver", "CUDA", "OS", "Python", "Mitsuba", "PyTorch")


def machine_probes(cpuinfo, meminfo, nvidia_smi, versions):
    from r3d.process_budget import meminfo_bytes
    cpu = next(line.split(":", 1)[1].strip() for line in cpuinfo.splitlines() if line.startswith("model name"))
    cores = set()
    for block in cpuinfo.split("\n\n"):
        fields = dict(line.split(":", 1) for line in block.splitlines() if ":" in line)
        fields = {key.strip(): value.strip() for key, value in fields.items()}
        if "core id" in fields:
            cores.add((fields.get("physical id", "0"), fields["core id"]))
    gpu = nvidia_smi.strip().splitlines()
    devices = [tuple(value.strip() for value in line.split(",")) for line in gpu]
    values = {"CPU": cpu, "Cores/threads": f"{len(cores) or 'unknown'}/{versions['threads']}",
              "RAM": f"{meminfo_bytes(meminfo)['MemTotal'] / 1024 ** 3:.1f} GiB",
              "GPU": "; ".join(row[0] for row in devices),
              "VRAM": "; ".join(row[1] + " MiB" for row in devices),
              "Driver": "; ".join(row[2] for row in devices), "CUDA": versions["CUDA"],
              **{name: versions[name] for name in ("OS", "Python", "Mitsuba", "PyTorch")}}
    return values


def machine_table(probes=None):
    if probes is None:
        import platform
        import importlib.metadata
        import torch
        gpu = subprocess.check_output(["nvidia-smi", "--query-gpu=name,memory.total,driver_version",
                                       "--format=csv,noheader,nounits"], text=True, timeout=10)
        probes = machine_probes(Path("/proc/cpuinfo").read_text(), Path("/proc/meminfo").read_text(), gpu,
                                dict(threads=os.cpu_count(), CUDA=torch.version.cuda, OS=platform.platform(),
                                     Python=platform.python_version(), Mitsuba=importlib.metadata.version("mitsuba"),
                                     PyTorch=torch.__version__))
    return markdown(["Machine", "Value"], [(key, probes[key]) for key in MACHINE_FIELDS])


def capture_rows(path, commit, id, object_name):
    from capture import QUARTERS, select_quarter
    text = select_quarter(path.read_text(encoding="utf-8", errors="replace"))
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
    return {aliases.get(base := name.removesuffix(f" quarter={QUARTERS[0]}"), base): value / 1000
            for name, value in parse_timings(text).items()
}

def board_means(captures, commit, id, object_name):
    require_same_board({str(path): capture_board(path) for path in captures})
    runs = [capture_rows(path, commit, id, object_name) for path in captures]
    for path, rows in zip(captures, runs):
        missing = set((id, *VARIANTS)) - rows.keys()
        if missing:
            raise ValueError(f"{path}: missing variants {sorted(missing)}")
    return {
name: statistics.median(rows[name] for rows in runs) for name in (id, *VARIANTS)
}

def capture_pose_every_ms(paths) -> int:
    intervals = set()
    for path in paths:
        values = set(map(int, re.findall(r"pose_every_ms=(\d+)", path.read_text(encoding="utf-8"))))
        if len(values) != 1 or min(values) <= 0:
            raise ValueError(f"{path}: needs a common positive pose interval")
        intervals.update(values)
    if len(intervals) != 1:
        raise ValueError("board captures need a common positive pose interval")
    return intervals.pop()


def board_frames(captures, expected, id, object_name, every_ms):
    require_same_board({str(path): capture_board(path) for path in captures})
    values = {}
    for capture in captures:
        from capture import select_quarter
        text = select_quarter(capture.read_text(encoding="utf-8", errors="replace"))
        parsed = []
        for row in poses(text):
            name = {object_name: id, object_name + "_lite": "lite"}.get(row["row"], row["row"])
            if name not in (id, "lite"):
                continue
            time_ms = row["time_ms"]
            if time_ms % every_ms:
                raise ValueError(f"{capture}: needs each full/lite pose exactly once")
            parsed.append((name, time_ms, row["frame_us"] / US_PER_MS))
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
    every_ms = capture_pose_every_ms(args.capture)
    poses = camera_path_poses(scene, job.renderer.fit, every_ms, either_way_up=False)
    for capture in args.capture:
        capture_viewport(capture, poses[:2])
    frame_values = board_frames(args.capture, [(name, index * every_ms) for name in (id, "lite")
                                             for index in range(len(poses[-1]))], id, args.object, every_ms)
    (work / "board-poses.txt").write_text(poses_text(*poses))
    rows, times, records = [], [], []
    for name, asset in ((id, args.object), ("lite", args.object + "_lite")):
        mesh = next(item.asset_path for item in scene.renderers if item.object.name == asset)
        features = mesh_rows(mesh, work / "board-poses.txt")
        for index, row in enumerate(features):
            ms = frame_values[(name, index * every_ms)]
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

# A kept result's files in its folder: the steps the run that made it recorded, a fit's mesh, a path-culled bake.
STEPS = "steps.json"
FITTED_MESH = "fitted.mesh"
CULLED = "culled"
# The bake cache's folder of the stage's own results; reference sets live in produce.py's.
DOC_CACHE = "doc"


class Kept(NamedTuple):
    """A result kept by its key in the bake cache: its folder, and the steps the run that made it recorded."""
    folder: Path
    steps: list


def read_kept(folder):
    steps = folder / STEPS
    return Kept(folder, json.loads(steps.read_text()) if steps.is_file() else [])


def make_kept(folder, function, *args):
    """In a worker: `folder` made by function(partial, *args, recorder=...) through produce.keyed_folder, with the
    steps it records in STEPS."""
    from bake.produce import keyed_folder
    from r3d.process_budget import StepRecorder

    def make(partial):
        recorder = StepRecorder()
        function(partial, *args, recorder=recorder)
        (partial / STEPS).write_text(json.dumps(recorder.rows, indent=2) + "\n")
    return read_kept(keyed_folder(folder, make))


def kept_references(key, job, scene, cache):
    """In a worker: the reference set of `job`'s fit, shared with bake.py's fits through produce.reference_set."""
    from bake.produce import reference_set
    return read_kept(reference_set(key, job, scene, cache))


def write_start(directory, job, scene, recorder=None):
    from r3d.mesh_import import write_baked
    write_baked(job, scene, directory, job.renderer.variant.name, recorder=recorder)


def fit_into(directory, point, point_dir, scene_path, scene, job, inputs, recorder=None):
    from r3d.fitted_variant import fit_point
    fit_point(point, point_dir, scene_path, scene, job, inputs, False, directory / FITTED_MESH, recorder=recorder)


def bake_into(directory, scene, baked_name, path_cull, recorder=None):
    from r3d.mesh_import import bake_geometry, camera_path_poses, write_baked
    from r3d.lit_mesh import write_lit_mesh
    baked = copy.deepcopy(next(item for item in scene.renderers if item.object.name == baked_name))
    geometry = bake_geometry(baked, scene, recorder)
    write_baked(baked, scene, directory, baked.renderer.variant.name, geometry, recorder)
    if path_cull:
        from r3d.light import visible_from_path
        from r3d.ray_query import RayQuery
        visibility = PATH_CULL_SAMPLING
        width, height, lens, near, poses = camera_path_poses(scene, visibility)
        seen = visible_from_path(geometry.positions, geometry.tris, geometry.tri_double,
            RayQuery(geometry.positions, geometry.tris),
            poses, width, height, lens, near, visibility.samples, visibility.margin)
        write_lit_mesh(directory, CULLED, geometry.positions, geometry.rgb, geometry.tris[seen],
                       geometry.tri_double[seen], **geometry.scale)


def code_digest(function):
    """`function`'s code without comments or layout: a kept result's key counts the code here that makes it, as a
    bake key counts its tool's (bake.code_tokens)."""
    import inspect
    import io
    import tokenize
    skipped = {tokenize.COMMENT, tokenize.NL, tokenize.INDENT, tokenize.DEDENT}
    tokens = tokenize.generate_tokens(io.StringIO(inspect.getsource(function)).readline)
    return hashlib.sha256(repr([token.string for token in tokens if token.type not in skipped]).encode()).hexdigest()


def link_inputs(inputs, start, references):
    """`inputs` as fit_point and measure_worker read it: the reference set's files and the start mesh, linked."""
    inputs.mkdir(parents=True, exist_ok=True)
    for source in [*references.iterdir(), *start.glob("*.mesh")]:
        link = inputs / source.name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(source.resolve(), target_is_directory=source.is_dir())


class Keeper:
    """The stage's starts, reference sets, fits and bakes by their key in the bake cache, which outlives the run: a
    key whose folder is done is not made again, and a key the run asks for twice is made once. Keys are bake.py's
    (bake.stage_keys), so reference sets are shared with its fits; the rest also count the code here that makes
    them. Each method returns a future of the result's Kept."""

    def __init__(self, executor, cache=None):
        from bake import bake
        self.executor = executor
        self.cache = Path(bake.default_cache() if cache is None else cache)
        self.tools = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        self.poses = bake.Poses({})
        self.made = {}
        self.lock = threading.Lock()

    def keys(self, job, scene, start=None):
        """bake.stage_keys of `job`; with `start`, its made start mesh, its fit's key too."""
        from bake import bake
        keys = bake.stage_keys(job, scene, self.tools, poses=self.poses)
        if start is not None:
            keys = bake.stage_keys(job, scene, self.tools, {keys["start"]: bake.file_sha256(start)}, self.poses)
        if keys.get("reason"):
            raise ValueError(keys["reason"])
        return keys

    def doc(self, *parts):
        from bake import bake
        return self.cache / DOC_CACHE / bake.digest(list(parts))

    def keep(self, folder, function, *args, **options):
        """The future of `folder`'s Kept: done when the folder is, else function(*args) submitted to a worker."""
        from bake.produce import DONE
        with self.lock:
            if folder not in self.made:
                if (folder / DONE).is_file():
                    self.made[folder] = Future()
                    self.made[folder].set_result(read_kept(folder))
                else:
                    self.made[folder] = self.executor.submit(function, *args, **options)
            return self.made[folder]

    def start(self, job, scene):
        from r3d.process_budget import PREPARE_BYTES
        folder = self.doc("start", self.keys(job, scene)["start"], code_digest(write_start))
        return self.keep(folder, make_kept, folder, write_start, job, scene, estimates=PREPARE_BYTES, priority=True)

    def references(self, job, scene):
        from bake.produce import REFERENCES
        from r3d.process_budget import PREPARE_BYTES
        key = self.keys(job, scene)["reference"]
        return self.keep(self.cache / REFERENCES / key, kept_references, key, job, scene, self.cache,
                         estimates=PREPARE_BYTES, priority=True)

    def fit(self, point, point_dir, scene_path, scene, job, inputs):
        """`job` fitted at `point` ({budget, cost_weight}, as fit_point takes it) from the start and references
        link_inputs put in `inputs`; its log in `point_dir`."""
        from r3d.process_budget import FIT_BYTES
        variant = copy.deepcopy(job)
        if point.get("budget") is not None:
            variant.renderer.fit.budget = point["budget"]
        cost = point.get("cost_weight", 0.0)
        folder = self.doc("fit", self.keys(variant, scene, inputs / f"{job.renderer.variant.name}.mesh")["fit"],
                          cost, content_checksum(WEIGHTS).decode() if cost else None, code_digest(fit_into))
        return self.keep(folder, make_kept, folder, fit_into, point, point_dir, scene_path, scene, job, inputs,
                         estimates=FIT_BYTES)

    def bake(self, scene, baked_name, path_cull):
        """The bake of the renderer `baked_name`, with its PATH_CULL_SAMPLING cull as CULLED when `path_cull`."""
        from r3d.process_budget import BAKE_BYTES
        baked = next(item for item in scene.renderers if item.object.name == baked_name)
        cull = None
        if path_cull:
            poses = self.poses.digest(scene.camera.component, PATH_CULL_SAMPLING.every_ms, PATH_CULL_SAMPLING.size,
                                      self.tools, set())
            if poses is None:
                raise ValueError(self.poses.reason(self.poses.missing))
            cull = [vars(PATH_CULL_SAMPLING), poses]
        folder = self.doc("bake", self.keys(baked, scene)["mesh"], cull, code_digest(bake_into))
        return self.keep(folder, make_kept, folder, bake_into, scene, baked_name, path_cull, estimates=BAKE_BYTES,
                         priority=True)

def smoke_prepare(scene_path, scene, job, inputs):
    from r3d.mesh_import import camera_path_poses
    from r3d.fitted_variant import poses_text
    from r3d.reference_render import main as reference_main
    inputs.mkdir(exist_ok=True)
    shutil.copyfile(job.asset_path, inputs / f"{job.renderer.variant.name}.mesh")
    width, height, lens, near, poses = camera_path_poses(scene, job.renderer.fit, 5000, either_way_up=False)
    for name in ("train", "train_landscape", "held_out", "coverage"):
        (inputs / f"{name}.txt").write_text(poses_text(32, 40, lens, near, poses[:2]))
    for name in ("reference", "reference_landscape", "reference_held_out"):
        reference_main([str(scene_path), "--object", job.object.name, "--poses", str(inputs / "train.txt"),
                        "--out", str(inputs / name), "--normals"])

def prepare_variants(keeper, preparations, on_ready):
    """The start and reference set from `keeper` per (scene, job, inputs folder) of `preparations`, linked into the
    inputs folder once both are there; on_ready(index, job) then runs, so its fit can start. Returns a future per
    preparation of its start's steps."""
    ready = [Future() for _ in preparations]
    lock = threading.Lock()
    for index, (scene, job, inputs) in enumerate(preparations):
        parts = (keeper.start(job, scene), keeper.references(job, scene))

        def completed(_future, index=index, job=job, inputs=inputs, parts=parts):
            with lock:
                if ready[index].done() or not all(part.done() for part in parts) and not any(
                        part.done() and part.exception() for part in parts):
                    return
                try:
                    start, references = (part.result() for part in parts)
                    link_inputs(inputs, start.folder, references.folder)
                    on_ready(index, job)
                    ready[index].set_result(start.steps)
                except BaseException as error:
                    ready[index].set_exception(error)
        for part in parts:
            part.add_done_callback(completed)
    return ready


def write_look_scene(scene_path, look, directory):
    """The scene in `look` (physical_scene.LOOKS) in `directory`, beside copies of the scene folder's imports and a
    link to its source folder, so the copy bakes like the scene."""
    from physical_scene import LOOKS, physical_scene
    directory.mkdir(parents=True, exist_ok=True)
    for settings in scene_path.parent.glob("*.import.toml"):
        shutil.copyfile(settings, directory / settings.name)
    source = directory / "source"
    if (scene_path.parent / "source").is_dir() and not source.exists():
        source.symlink_to((scene_path.parent / "source").resolve(), target_is_directory=True)
    path = directory / scene_path.name
    path.write_text(physical_scene(scene_path.read_text(encoding="utf-8"), scene_path.parent, LOOKS[look]),
                    encoding="utf-8")
    return path


def submit_light_fits(keeper, scene_path, work):
    """[(look, scene file, scene, job, inputs folder)] for every look LIGHT_SHEETS names, and the futures that finish once
    each is prepared and {look: future of its fit's Kept}, filled as they do: the scene's showcase, its first renderer,
    prepared and fitted in that look."""
    from r3d.fitted_variant import placed_variant
    from r3d.import_settings import load_scene
    from showcase import showcase
    looks = dict.fromkeys([sheet.reference for sheet in LIGHT_SHEETS] +
                          [look for sheet in LIGHT_SHEETS for _label, look in sheet.columns])
    studies = []
    for look in looks:
        path = write_look_scene(scene_path, look, work / f"look-{look}")
        scene = load_scene(path)
        studies.append((look, path, scene, placed_variant(scene, showcase(scene)), path.parent / "inputs"))
    fits = {}

    def prepared(index, job):
        look, path, scene, _job, inputs = studies[index]
        fits[look] = keeper.fit({}, inputs.parent / "fit", path, scene, job, inputs)
    ready = prepare_variants(keeper, [study[2:] for study in studies], prepared)
    return studies, ready, fits


def light_sheets(light, out, work, measure, comparisons):
    """LIGHT_SHEETS and the AO_MAP sheet into out/render/gpu from submit_light_fits; `measure` renders a mesh at the
    held-out poses and files its frames in `comparisons` under its label."""
    from PIL import Image
    from r3d.fitted_variant import poses_text
    from r3d.poses import read_poses
    studies, ready, fits = light
    frames, inputs = {}, {}
    for (look, _path, _scene, job, look_inputs), prepared in zip(studies, ready):
        prepared.result()
        measure(f"light-{look}", job, fits[look].result().folder / FITTED_MESH, look_inputs)
        frames[look], inputs[look] = comparisons[f"light-{look}"], look_inputs
    for sheet in LIGHT_SHEETS:
        image = out / "render/gpu" / f"{sheet.name}.png"
        command = [sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out", image,
                   "--reference-bakes", inputs[sheet.reference] / "reference_held_out", "--reference-scale", "2",
                   "--sheet-frames", sheet.frames]
        for label, look in sheet.columns:
            command += ["--bake", label, frames[look]]
        if sheet.own_references:
            for label, look in sheet.columns:
                command += ["--bake-reference", label, inputs[look] / "reference_held_out"]
        if sheet.crops:
            command += ["--crops", sheet.crops]
        run(command, work / f"{sheet.name}.log")
        # render_compare.py writes no crops where the fits do not differ; fail rather than link a missing file.
        if sheet.crops and not image.with_suffix(".crops.png").exists():
            raise ValueError(f"{sheet.name}: no crops, the fits do not differ")
    look = AO_MAP.sheet.reference
    path, job = next((path, job) for name, path, _scene, job, _inputs in studies if name == look)
    width, height, lens, near, poses = read_poses(inputs[look] / "held_out.txt")
    shown = [int(frame) for frame in AO_MAP.sheet.frames.split(",")]
    occlusion = work / AO_MAP.name
    occlusion.mkdir(parents=True, exist_ok=True)
    (occlusion / "poses.txt").write_text(poses_text(width, height, lens, near, [poses[frame] for frame in shown]))
    run([sys.executable, ROOT / "launcher/tools/r3d/reference_render.py", path, "--object", job.object.name,
         "--poses", occlusion / "poses.txt", "--occlusion", "--out", occlusion], work / f"{AO_MAP.name}.log")
    rows = []
    for index in range(len(shown)):
        pair = [Image.open(occlusion / f"{index:04d}{suffix}.png").convert("RGB") for suffix in (".occlusion", "")]
        rows.append([picture.resize((picture.width * 2, picture.height * 2), Image.Resampling.NEAREST)
                     for picture in pair])
    width, height = rows[0][0].size
    sheet = Image.new("RGB", (width * 2, height * len(rows)))
    for y, row in enumerate(rows):
        for x, picture in enumerate(row):
            sheet.paste(picture, (x * width, y * height))
    sheet.save(out / "render/gpu" / f"{AO_MAP.name}.png")


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
    from r3d.task_executor import TaskExecutor
    with TaskExecutor() as executor:
        return _gpu(args, out, work, executor)


def normal_weights(job):
    """The normal sweep's weights: the recipe's among them."""
    return list(dict.fromkeys((0.0, 0.1, 0.3, job.renderer.fit.normal_weight)))


def sweep_points(job):
    """The budget sweep's points, at the budgets the variant has the triangles for."""
    budgets = dict.fromkeys(budget for budget in (4000, 6000, job.renderer.fit.budget)
                            if budget <= job.renderer.variant.triangles)
    return [{"budget": budget, "cost_weight": cost} for budget in budgets for cost in (0.0, 0.1)]


def recipe_point(job, point):
    """Whether the sweep `point` is the recipe's own fit."""
    return point["budget"] == job.renderer.fit.budget and not point["cost_weight"]


def _gpu(args, out, work, executor):
    from r3d.fitted_variant import (placed_variant, plot_pareto, point_name,
                                   run_sweep_points, sweep_rows, write_sweep_csv, fit_point)
    from r3d.import_settings import load_scene
    from r3d.bake_fidelity import build_host
    from r3d.cost_model import load
    from r3d.process_budget import MEASURE_BYTES, SMOKE_PREPARE_BYTES, FIT_BYTES

    scene_path = args.scene
    scene = load_scene(scene_path)
    id = scene_id(scene_path)
    jobs = [placed_variant(scene, args.object + entry.name) for entry in FITTED]
    if len({job.renderer.fit.held_out_every_ms for job in jobs}) != 1:
        raise ValueError("GPU comparisons require a common held-out pose interval")
    made_from = gpu_inputs(scene_path, args.object)
    from bake import bake
    stamp = bake.digest(made_from)
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
    keeper = Keeper(executor)
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
        fitted_futures[prefix] = keeper.fit({}, work / f"fit-{prefix}", scene_path, scene, job, reference_inputs)
        if FITTED[index].sweeps:
            for normal in normal_weights(job):
                if normal != job.renderer.fit.normal_weight:
                    variant = copy.deepcopy(job)
                    variant.renderer.fit.normal_weight = normal
                    normal_futures[normal] = keeper.fit({}, work / f"normal-{normal:g}", scene_path, scene, variant,
                                                        reference_inputs)
            for point in sweep_points(job):
                point_dir = work / point_name(point)
                if not recipe_point(job, point) and not (point_dir / "result.json").is_file():
                    sweep_futures[point_dir.name] = keeper.fit(point, point_dir, scene_path, scene, job, reference_inputs)

    ready = prepare_variants(keeper, [(scene, job, work / f"inputs-{entry.prefix}") for entry, job in zip(FITTED, jobs)],
                             prepared)
    light = submit_light_fits(keeper, scene_path, work)
    bakes = [keeper.bake(scene, args.object + entry.baked, entry.path_cull) for entry in FITTED]

    for index, job in enumerate(jobs):
        start_steps = ready[index].result()
        entry = FITTED[index]
        prefix = entry.prefix
        reference_inputs = work / f"inputs-{prefix}"
        baked = bakes[index].result()
        bake_steps.append((f"{prefix}-GI-bake", baked.steps))
        baked_job = next(item for item in scene.renderers if item.object.name == args.object + entry.baked)
        measure(f"{prefix}-GI-bake", job, baked.folder / f"{baked_job.renderer.variant.name}.mesh", reference_inputs)
        if entry.path_cull:
            measure("full-path-culled", job, baked.folder / f"{CULLED}.mesh", reference_inputs)
            run([sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out",
                 out / "render/gpu/appearance-path-culled.png", "--reference-bakes",
                 reference_inputs / "reference_held_out", "--reference-scale", "2", "--sheet-frames", "0,4",
                 "--bake", "full bake", comparisons["full-GI-bake"], "--bake", "path culled",
                 comparisons["full-path-culled"], "--crops", "3"], work / "path-sheet.log")
        fit = fitted_futures[prefix].result()
        bake_steps.append((f"{prefix}-fit-start", start_steps))
        bake_steps.append((f"{prefix}-GI-fit", fit.steps))
        fitted = fit.folder / FITTED_MESH
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
            for normal in normal_weights(job):
                mesh = fitted if normal == job.renderer.fit.normal_weight else normal_futures[normal].result().folder / FITTED_MESH
                measure(f"normal-{normal:g}", job, mesh, reference_inputs)
                normal_rows.append(rows[-1])
            (out / f"tables/{id}-normal.md").write_text(markdown(
                ["Normal weight", "Triangles", "Mean dE76", "p95 dE76", "SSIM", "Normal angle", "Predicted ms"], normal_rows) +
                "\n![Normal angle heatmaps](../images/render/gpu/appearance-normal-heat.png)\n")
            command = [sys.executable, ROOT / "launcher/tools/render/render_compare.py", "--out",
                       out / "render/gpu/appearance-normal-heat.png"]
            for normal in normal_weights(job):
                command += ["--angle-column", f"normal {normal:g}", work / f"normal-{normal:g}/angles"]
            run(command, work / "normal-sheet.log")
            points = sweep_points(job)

            def run_point(point, point_dir):
                mesh = fitted if recipe_point(job, point) else sweep_futures[point_dir.name].result().folder / FITTED_MESH
                return measure(point_dir.name, job, mesh, reference_inputs)

            run_sweep_points(work, points, run_point)
            sweep = sweep_rows(work, points)
    light_sheets(light, out, work, measure, comparisons)
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
    from bake_steps import block as steps_block, chart as steps_chart
    steps_chart(bake_steps, out / "render/gpu" / BAKE_STEPS_CHART)
    (out / "tables/bake-steps.md").write_text(steps_block(bake_steps, f"../images/render/gpu/{BAKE_STEPS_CHART}"))
    (out / "measurements.json").write_text(json.dumps({"source": stamp, "rows": rows, "sweep": sweep,
                                                        "bake_steps": bake_steps, "machine": machine}, indent=2) + "\n")
    apply_tables(ROOT, out / "tables")
    sync_images(out / "render/gpu", ROOT / "docs/images/render/gpu", check=False)
    files = {path.relative_to(out).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in out.rglob("*") if path.is_file()}
    (out / "source.json").write_text(json.dumps({"source": stamp, "inputs": made_from, "files": files}, indent=2) + "\n")
    return 0


def check_gpu(args, out):
    from bake import bake
    manifest = out / "source.json"
    if not manifest.exists() or json.loads(manifest.read_text())["source"] != bake.digest(gpu_inputs(args.scene, args.object)):
        raise ValueError("GPU source changed or no full run exists; run --stage gpu without --check")
    for name, checksum in json.loads(manifest.read_text())["files"].items():
        path = out / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise ValueError(f"GPU output missing or altered: {name}; run --stage gpu")
    changed = apply_tables(ROOT, out / "tables", True)
    return int(sync_images(out / "render/gpu", ROOT / "docs/images/render/gpu", check=True) or changed)


def sync_images(made, kept, *, check):
    """Copies a stage's PNGs from `made` into the docs' `kept`, or with `check` reports each one that differs
    and each kept one the stage no longer makes; returns whether any did."""
    changed = False
    for image in sorted(made.glob("*.png")):
        target = kept / image.name
        if not check:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(image, target)
            continue
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
    if check:
        for image in sorted(set(kept.glob("*.png")) - {kept / image.name for image in made.glob("*.png")}):
            print(f"orphan {image}")
            changed = True
    return changed


def meshlets(args, out, work):
    from meshlet_sizes import table
    made = out / "render/meshlets"
    made.mkdir(parents=True)
    body = table(args.scene, args.captures, args.bakes, made / "meshlet-sizes.png")
    (out / "tables/meshlet-sizes.md").write_text(body)
    changed = apply_tables(ROOT, out / "tables", args.check)
    changed |= sync_images(made, ROOT / "docs/images/render/meshlets", check=args.check)
    return int(args.check and changed)


def main(argv=None):
    git_environment()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", type=Path, required=True)
    parser.add_argument("--object", required=True)
    parser.add_argument("--stage", choices=("gpu", "board", "meshlets"), required=True)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--inputs", action="store_true", help="gpu: print the stage's input fingerprint, run nothing")
    parser.add_argument("--capture", type=Path, action="append", default=[])
    parser.add_argument("--build-commit", help="capture commit, default HEAD; firmware sources must still match")
    parser.add_argument("--captures", type=Path, default=ROOT / "docs/render/data")
    parser.add_argument("--bakes", type=Path, default=ROOT / "launcher/tools/results/meshlet-sizes/bakes")
    args = parser.parse_args(argv)
    try:
        validate_mode(args.smoke, args.check)
        if args.stage == "board" and (not args.capture or args.smoke):
            raise ValueError("board stage needs --capture and does not support --smoke")
        work = RESULTS / "work" / args.stage
        out = RESULTS / "out" / (args.stage + "-smoke" if args.smoke else args.stage)
        if args.stage == "gpu" and args.inputs:
            if sys.version_info < INPUTS_PYTHON_MINIMUM:
                minimum = ".".join(map(str, INPUTS_PYTHON_MINIMUM))
                raise ValueError(f"--inputs requires Python {minimum} or later for stable code tokens")
            from bake import bake
            print(bake.digest(gpu_inputs(args.scene, args.object)))
            return 0
        if args.stage == "gpu" and args.check:
            return check_gpu(args, out)
        if out.exists():
            shutil.rmtree(out)
        (out / "tables").mkdir(parents=True)
        (out / "render/gpu").mkdir(parents=True)
        work.mkdir(parents=True, exist_ok=True)
        return {"gpu": gpu, "board": board, "meshlets": meshlets}[args.stage](args, out, work)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
