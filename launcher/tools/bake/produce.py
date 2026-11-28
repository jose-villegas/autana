"""Making a bake (bake.py bake): runs its stage's tool into the cache and records which bytes the
key got. Needs the tool's environment: the r3d requirements for a mesh or a reference set, and for a fit
also a CUDA GPU with requirements-gpu.txt (r3d/gpu_python.sh). A reference set traces on the CUDA GPU where
Mitsuba loads a scene there, else on the CPU (reference_render.reference_variant); a reference bake fails off
CUDA (REFERENCE_VARIANT). Before it runs, every
requirement pin the key counts must match what is installed, so a key never names bytes made with other packages.
A bake made on the GPU, a fit or a reference set traced there, names the GPU in its row.

A fit's start is a mesh bake of its own and its reference set a reference bake; the fit reads the bytes its key
names, from the lock or this cache, so any GPU host fits from the same inputs. A reference bake renders its set
once per reference key and machine (reference_folder) into the cache's reference/ folder, where the doc stages
read the held-out references too, and packs what the fit reads. Each pose of a set is traced once per pose key into reference-poses/ (PoseStore),
so a set whose key changed traces only the poses whose own inputs changed. A run made `again` renders each set once
more, in a folder of its own and tracing every pose; a set or pose nothing has read for REFERENCE_KEEP_DAYS is
deleted.
"""

import copy
import importlib.metadata
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import tempfile
import time

from bake import bake as keys

KEY_INDEX = "keys"
REFERENCES = "reference"
REFERENCE_POSES = "reference-poses"
DONE = "done"
PRUNED = ".pruned"
REFERENCE_KEEP_DAYS = 14
DAY_S = 24 * 60 * 60
PIN = re.compile(r"^([A-Za-z0-9_.-]+)==(\S+)$")


class EnvironmentMismatch(keys.BakeMissing):
    """An installed package differs from a pin the key counts."""


def index_path(key, cache):
    return pathlib.Path(cache) / KEY_INDEX / f"{key}.json"


def write_atomically(target, text):
    """Writes by rename, so a build reading the cache meanwhile never sees half a file."""
    keys.place(text.encode("utf-8"), target)


def made(key, cache):
    """The row a bake made for `key` in this cache, with its file there, or None."""
    path = index_path(key, cache)
    if not path.is_file():
        return None
    row = json.loads(path.read_text(encoding="utf-8"))
    file = keys.cached(row, row["suffix"], cache)
    return row if file.is_file() and keys.file_sha256(file) == row["sha256"] else None


def maker(on_gpu=False):
    """Where a bake is made, as its row names it: the host, and what its bytes differ by; `on_gpu` for one made on
    the GPU."""
    fields = {"host": f"{platform.system()} {platform.machine()}"}
    if keys.cpu_vendor() is not None:
        fields["cpu_vendor"] = keys.cpu_vendor()  # a rerun adopts a mesh only from a matching CPU vendor
    if on_gpu and (gpu := keys.gpu_name()) is not None:
        fields["gpu"] = gpu  # a GPU bake's bytes differ between GPU models: its lock row names the one
    return fields


def record(bake, path, cache, run=None, on_gpu=False):
    """Puts the file `path` made for `bake` in the cache and indexes it by key; `on_gpu` when it was made on the
    GPU."""
    data = pathlib.Path(path).read_bytes()
    row = {"output": bake.output, "source": keys.relative(bake.source), "key": bake.key,
           "sha256": keys.hashlib.sha256(data).hexdigest(), "size": len(data), "suffix": bake.suffix,
           **maker(on_gpu)}
    if run is not None:
        row["run"] = run
    keys.store(path, row, bake.suffix, cache)
    write_atomically(index_path(bake.key, cache), json.dumps(row, sort_keys=True) + "\n")
    return row


def check_environment(stages):
    """Fails naming every keyed pin the installed packages do not match."""
    wrong = []
    for stage in stages:
        for file, line in keys.requirement_pins(stage):
            match = PIN.match(line)
            if match is None:
                continue
            name, pinned = match.groups()
            try:
                installed = importlib.metadata.version(name)
            except importlib.metadata.PackageNotFoundError:
                installed = None
            if installed != pinned:
                wrong.append(f"  {name}: {file} pins {pinned}, this environment has {installed or 'none'}")
    if wrong:
        raise EnvironmentMismatch("the key would claim packages this environment does not have:\n"
                                  + "\n".join(sorted(set(wrong))))


def export_file(settings, lock, cache):
    """The export a .blend source's import reads: made here, or locked and fetched."""
    key = keys.blend_key(settings, {stage: keys.tool_digest(stage) for stage in keys.STAGES})
    row = made(key, cache) or lock.get(key)
    if row is None:
        raise keys.BakeMissing(f"{keys.relative(settings.source['path'])}: its export {key} is neither made here "
                               "nor locked; bake.py bake --kind blend first")
    blend = keys.Bake(output=settings.source["path"].with_suffix(keys.GLB_SUFFIX).name, source=settings.path,
                      holder=settings.source["path"].name, kind="blend", key=key, suffix=keys.GLB_SUFFIX,
                      tree=settings.source["path"].with_suffix(keys.GLB_SUFFIX))
    return keys.fetch_all([blend], {key: row}, cache)[blend]


def with_source(job, path):
    """`job` reading `path` in place of its source."""
    settings = copy.copy(job.settings)
    settings.source = {**job.settings.source, "path": path}
    moved = copy.copy(job)
    moved.settings = settings
    moved.renderer = copy.copy(job.renderer)
    moved.renderer.settings = settings
    return moved


def produce_mesh(bake, cache, lock):
    from r3d.mesh_bundle import start_file
    from r3d.mesh_import import write_baked

    job = bake.job
    if keys.is_blend(job.settings):
        job = with_source(job, export_file(job.settings, lock, cache))
    with tempfile.TemporaryDirectory() as work:
        start = start_file(job, work)
        write_baked(job, bake.scene, start.parent, job.asset_name)
        return record(bake, start, cache)


def blender_program(given=None):
    program = given or shutil.which("blender")
    if program is None:
        raise keys.BakeMissing("exporting a .blend needs Blender: put `blender` on PATH or pass --blender")
    return program


def produce_blend(bake, cache, blender=None):
    """Exports the .blend inside Blender with the import's clips; the exporter checks the version."""
    settings = bake.job
    clips = settings.source.get("clips")
    with tempfile.TemporaryDirectory() as work:
        out = pathlib.Path(work) / bake.output
        command = [blender_program(blender), "--background", "--factory-startup", "--python",
                   str(keys.TOOLS / keys.BLEND_EXPORT), "--", str(settings.source["path"]), str(out)]
        if clips:
            command += ["--clips", ",".join(clips)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not out.is_file():
            raise keys.BakeMissing(f"exporting {keys.relative(settings.source['path'])} failed:\n"
                                   + (result.stdout + result.stderr)[-2000:])
        return record(bake, out, cache)


def keyed_folder(folder, make):
    """`folder` once its DONE marker is there; else make(partial) fills a sibling, which is marked done and renamed
    into place, so a cut run leaves nothing a later one takes for done."""
    folder = pathlib.Path(folder)
    if (folder / DONE).is_file():
        return folder
    partial = folder.with_name(folder.name + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    partial.mkdir(parents=True)
    make(partial)
    (partial / DONE).write_text(folder.name + "\n", encoding="utf-8")
    shutil.rmtree(folder, ignore_errors=True)
    os.replace(partial, folder)
    return folder


def read_references(folder):
    """Marks a reference set read, by its folder's time, the one prune_references ages it by."""
    if folder.is_dir():
        os.utime(folder)


def prune_references(cache, now=None):
    """Deletes each reference set or traced pose, done or cut, that nothing has read for REFERENCE_KEEP_DAYS; a bake
    runs it before it reads any. Each is renamed aside first, so a delete cut short never leaves one that looks done;
    one that cannot be moved (a file held open) is left for a later bake."""
    now = time.time() if now is None else now
    for folder in sorted(path for kind in (REFERENCES, REFERENCE_POSES) for path in (pathlib.Path(cache) / kind).glob("*")):
        pruned = folder if folder.name.endswith(PRUNED) else folder.with_name(folder.name + PRUNED)
        try:
            if now - folder.stat().st_mtime <= REFERENCE_KEEP_DAYS * DAY_S:
                continue
            if pruned != folder:
                shutil.rmtree(pruned, ignore_errors=True)  # an earlier delete cut short
                os.replace(folder, pruned)
            shutil.rmtree(pruned)
        except OSError:
            continue  # gone already, or held open


# The one variant a reference set is baked on (r3d/README.md, "Reference on the GPU"); its row names the GPU.
REFERENCE_VARIANT = "cuda_ad_rgb"


def trace_machine():
    """What a reference trace's bytes differ by besides its inputs: the Mitsuba variant it traces on, the GPU and its
    driver. A set's folder and each PoseStore key name it, so nothing traced on another machine, variant or driver
    is ever taken for this one's."""
    from r3d.reference_render import reference_variant

    return [reference_variant(), keys.gpu_name(), keys.gpu_driver()]


def reference_folder(cache, key):
    """Where this cache keeps the reference set of `key` traced on this machine (trace_machine)."""
    return pathlib.Path(cache) / REFERENCES / keys.digest([key, *trace_machine()])


class PoseStore:
    """Traced poses in this cache's reference-poses/, one keyed_folder per pose: a digest of the pose as
    reference_set.poses_text writes it (its size, lens and pose line), `key` (bake.py's "reference_pose" stage key:
    everything else its render reads) and trace_machine(). reference_render.render_sets traces only the poses it
    lacks."""

    def __init__(self, cache, key):
        self.root = pathlib.Path(cache) / REFERENCE_POSES
        self.key = keys.digest([key, *trace_machine()])

    def folder(self, width, height, lens, near, pose):
        from r3d.reference_set import poses_text

        return self.root / keys.digest(["reference pose", self.key, poses_text(width, height, lens, near, [pose])])


def reference_set(key, job, scene, cache, fresh=None, pose_key=None):
    """The reference set of `job`'s fit under its reference `key` in this cache, prepared there when it has none; or,
    given `fresh`, a run's own folder, in that one, so a run made again never replaces a set another may be
    reading. Given `pose_key` and no `fresh`, its poses come from this cache's PoseStore; without one (a doc stage's
    or objects report's set, keyed apart from a bake's) every pose is traced. The log says whether it was rendered
    or reused. Each pose traced is progress for the stage's guard: a set traces for hours, and one still tracing is
    never taken for hung."""
    from process_tree import progressed
    from r3d.reference_set import prepare_references, reference_inputs

    folder = pathlib.Path(fresh) / key if fresh else reference_folder(cache, key)
    reused = (folder / DONE).is_file()
    store = PoseStore(cache, pose_key) if pose_key and not fresh else None

    keyed_folder(folder, lambda partial: prepare_references(reference_inputs(job, scene), partial, store, progressed))
    read_references(folder)
    print(f"reference {key}: {'reused' if reused else 'rendered'}", flush=True)
    return folder


def input_file(bake, cache, lock, dependency=None):
    """The input whose bytes `bake`'s key names (its start, or its reference set when `dependency` is "reference"),
    from the lock or, when this run made it, this cache."""
    dependency = dependency or ("fit" if bake.kind == "levels" else "start")
    given = bake.reference if dependency == "reference" else bake.start
    sha256 = bake.stages[dependency + "_sha256"]
    row = next((row for row in (lock.get(given.key), made(given.key, cache)) if row and row["sha256"] == sha256), None)
    if row is None:
        raise keys.BakeMissing(f"{bake.output}: its {dependency} {given.key} with sha256 {sha256} is neither locked nor "
                               f"made here; bake.py bake --kind {given.kind} first")
    return keys.fetch_all([given], {given.key: row}, cache)[given]


def produce_reference(bake, cache, fresh_references):
    from r3d.reference_render import reference_variant
    from r3d.reference_set import pack_references, reference_inputs

    if reference_variant() != REFERENCE_VARIANT:
        raise keys.SettingsError(f"{bake.output}: a reference set is baked on Mitsuba's {REFERENCE_VARIANT} only, and "
                                 f"this machine traces on {reference_variant()} (r3d/README.md, \"Reference on the GPU\")")
    folder = reference_set(bake.key, bake.job, bake.scene, cache, fresh_references, bake.stages.get("reference_pose"))
    with tempfile.TemporaryDirectory() as work:
        target = pathlib.Path(work) / bake.output
        target.write_bytes(pack_references(folder, reference_inputs(bake.job, bake.scene).sets()))
        return record(bake, target, cache, on_gpu=True)


def produce_fit(bake, cache, lock):
    from r3d.fit.stage import fit
    from r3d.reference_set import unpack_references

    start = input_file(bake, cache, lock)
    references = input_file(bake, cache, lock, "reference")
    with tempfile.TemporaryDirectory() as work:
        inputs = pathlib.Path(work) / "references"
        unpack_references(references.read_bytes(), inputs)
        target = pathlib.Path(work) / bake.output
        fit(bake.scene.path, bake.scene, bake.job, pathlib.Path(work), target=target, inputs=inputs, start=start)
        return record(bake, target, cache, on_gpu=True)


def produce_sound(bake, cache):
    from audio import sound_asset, sound_import

    with tempfile.TemporaryDirectory() as work:
        target = pathlib.Path(work) / bake.output
        try:
            target.write_bytes(sound_import.bake(bake.source))
        except sound_asset.SoundError as error:
            raise keys.SettingsError(str(error)) from error
        return record(bake, target, cache)


def produce(bake, cache, lock=None, blender=None, fresh_references=None):
    """Makes `bake` into the cache and returns its row; the environment must match its pins. A reference bake
    given `fresh_references`, a run's own folder, renders its set there (reference_set)."""
    if bake.kind == "blend":
        return produce_blend(bake, cache, blender)
    if bake.kind == "sh_probes":
        check_environment(("sh_probes",))
        from r3d.sh_probe_bake import bake as bake_probes
        job = bake.job
        if keys.is_blend(job.settings):
            job = with_source(job, export_file(job.settings, lock or {}, cache))
        with tempfile.TemporaryDirectory() as work:
            target = pathlib.Path(work) / bake.output
            target.write_bytes(bake_probes(job, bake.scene))
            return record(bake, target, cache)
    if bake.kind == "sound":
        check_environment(("sound",))
        return produce_sound(bake, cache)
    if bake.kind == "levels":
        check_environment(("levels",))
        from r3d.lod.levels_stage import levels
        with tempfile.TemporaryDirectory() as work:
            target = pathlib.Path(work) / bake.output
            fit = input_file(bake, cache, lock or {})
            start = input_file(bake.start, cache, lock or {})
            target.write_bytes(levels(fit.read_bytes(), bake.job.asset_name, bake.job.settings.lod,
                                      bake.job.settings.meshlet_triangles, start=start.read_bytes()))
            return record(bake, target, cache)
    if bake.kind == "texture":
        check_environment(("texture",))
        from r3d.texture.texture_stage import texture
        with tempfile.TemporaryDirectory() as work:
            target = pathlib.Path(work) / bake.output
            target.write_bytes(texture(input_file(bake, cache, lock or {}).read_bytes(), bake.job, bake.stages["textures"]))
            return record(bake, target, cache)
    if bake.kind == "reference":
        check_environment(("mesh", "reference"))
        return produce_reference(bake, cache, fresh_references)
    check_environment(("mesh", "fit") if bake.kind == "fit" else ("mesh",))
    if bake.kind == "fit":
        return produce_fit(bake, cache, lock or {})
    return produce_mesh(bake, cache, lock or {})


def export(row, cache, out):
    """Copies a made file and its index row into `out`, the folder a CI run uploads, each by rename and the row
    last, so a run cut short uploads every bake it finished and no half of one."""
    keys.place(keys.cached(row, row["suffix"], cache).read_bytes(), out / "files" / f"{row['sha256']}{row['suffix']}",
               row["sha256"])
    keys.place((json.dumps(row, sort_keys=True) + "\n").encode("utf-8"), out / KEY_INDEX / f"{row['key']}.json")


def adopt(rows, cache):
    """Indexes in this cache each row a run made on a host like this one (maker), its file already there, so a
    rerun takes it as made here; returns those rows. A row with a `gpu` was made on the GPU and is taken only where
    nvidia-smi names the same one; a row without one was made on the CPU."""
    adopted = []
    for row in rows:
        made_on = {name: row[name] for name in ("host", "cpu_vendor", "gpu") if name in row}
        if made_on == maker(on_gpu="gpu" in row):
            write_atomically(index_path(row["key"], cache), json.dumps(row, sort_keys=True) + "\n")
            adopted.append(row)
    return adopted


def attempt(artifact):
    """The run attempt an artifact's folder names last (bakes-gpu-2 is 2), 0 when it names none."""
    tail = pathlib.Path(artifact).name.rsplit("-", 1)[-1]
    return int(tail) if tail.isdigit() else 0


def import_run(folder, run, cache):
    """{key: row} of the files a CI run uploaded, each checked against its row and put in the cache. Each attempt
    uploads its own artifact (bakes-gpu-2); a key two attempts made takes the later attempt's row."""
    rows = {}
    for index in sorted(pathlib.Path(folder).rglob(f"{KEY_INDEX}/*.json"), key=lambda index: (attempt(index.parent.parent), index)):
        row = json.loads(index.read_text(encoding="utf-8"))
        file = index.parent.parent / "files" / f"{row['sha256']}{row['suffix']}"
        if not file.is_file() or keys.file_sha256(file) != row["sha256"]:
            raise keys.BakeMissing(f"run {run}: {row['output']} has no file with the SHA-256 its row gives")
        keys.store(file, row, row["suffix"], cache)
        rows[row["key"]] = {**row, "run": run}
    return rows
