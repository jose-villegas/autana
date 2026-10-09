"""Making a bake (bake.py bake): runs its stage's tool into the cache and records which bytes the
key got. Needs the tool's environment: the r3d requirements for a mesh, and for a fit also a CUDA
GPU with requirements-gpu.txt (r3d/gpu_python.sh). Before it runs, every requirement pin the key
counts must match what is installed, so a key never names bytes made with other packages.

A fit's references are made once per reference key into the cache's reference/ folder and never
leave the machine that made them: only the fit stage reads them, and it runs there.
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

from bake import bake as keys

KEY_INDEX = "keys"
REFERENCES = "reference"
DONE = "done"
PIN = re.compile(r"^([A-Za-z0-9_.-]+)==(\S+)$")


class EnvironmentMismatch(keys.BakeMissing):
    """An installed package differs from a pin the key counts."""


def index_path(key, cache):
    return pathlib.Path(cache) / KEY_INDEX / f"{key}.json"


def write_atomically(target, text):
    """Writes by rename, so a build reading the cache meanwhile never sees half a file."""
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=target.parent, delete=False, encoding="utf-8") as scratch:
        scratch.write(text)
    os.replace(scratch.name, target)


def made(key, cache):
    """The row a bake made for `key` in this cache, with its file there, or None."""
    path = index_path(key, cache)
    if not path.is_file():
        return None
    row = json.loads(path.read_text(encoding="utf-8"))
    file = keys.cached(row, row["suffix"], cache)
    return row if file.is_file() and keys.file_sha256(file) == row["sha256"] else None


def record(bake, path, cache, run=None):
    """Puts the file `path` made for `bake` in the cache and indexes it by key."""
    data = pathlib.Path(path).read_bytes()
    row = {"output": bake.output, "source": keys.relative(bake.source), "key": bake.key,
           "sha256": keys.hashlib.sha256(data).hexdigest(), "size": len(data), "suffix": bake.suffix,
           "host": f"{platform.system()} {platform.machine()}"}
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
    from r3d.mesh_import import write_baked

    job = bake.job
    if keys.is_blend(job.settings):
        job = with_source(job, export_file(job.settings, lock, cache))
    with tempfile.TemporaryDirectory() as work:
        name = bake.output.removesuffix(bake.suffix)
        write_baked(job, bake.scene, pathlib.Path(work), name)
        return record(bake, pathlib.Path(work) / bake.output, cache)


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


def references(bake, cache):
    """The fit's reference folder in this cache, prepared there when its key has none."""
    from r3d.fitted_variant import prepare

    folder = pathlib.Path(cache) / REFERENCES / bake.stages["reference"]
    if (folder / DONE).is_file():
        return folder
    partial = folder.with_name(folder.name + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    partial.mkdir(parents=True)
    prepare(bake.scene.path, bake.scene, bake.job, partial)
    (partial / DONE).write_text(bake.stages["reference"] + "\n", encoding="utf-8")
    shutil.rmtree(folder, ignore_errors=True)
    os.replace(partial, folder)
    return folder


def produce_fit(bake, cache):
    from r3d.fitted_variant import fit

    inputs = references(bake, cache)
    with tempfile.TemporaryDirectory() as work:
        target = pathlib.Path(work) / bake.output
        fit(bake.scene.path, bake.scene, bake.job, pathlib.Path(work), target=target, inputs=inputs)
        return record(bake, target, cache)


def produce(bake, cache, lock=None, blender=None):
    """Makes `bake` into the cache and returns its row; the environment must match its pins."""
    if bake.kind == "blend":
        return produce_blend(bake, cache, blender)
    check_environment(("mesh", "reference", "fit") if bake.kind == "fit" else ("mesh",))
    if bake.kind == "fit":
        return produce_fit(bake, cache)
    return produce_mesh(bake, cache, lock or {})


def export(rows, cache, out):
    """Copies each made file and its index row into `out`, the folder a CI run uploads."""
    for row in rows:
        (out / "files").mkdir(parents=True, exist_ok=True)
        (out / KEY_INDEX).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(keys.cached(row, row["suffix"], cache), out / "files" / f"{row['sha256']}{row['suffix']}")
        (out / KEY_INDEX / f"{row['key']}.json").write_text(json.dumps(row, sort_keys=True) + "\n", encoding="utf-8")


def import_run(folder, run, cache):
    """{key: row} of the files a CI run uploaded, each checked against its row and put in the cache."""
    rows = {}
    for index in sorted(pathlib.Path(folder).rglob(f"{KEY_INDEX}/*.json")):
        row = json.loads(index.read_text(encoding="utf-8"))
        file = index.parent.parent / "files" / f"{row['sha256']}{row['suffix']}"
        if not file.is_file() or keys.file_sha256(file) != row["sha256"]:
            raise keys.BakeMissing(f"run {run}: {row['output']} has no file with the SHA-256 its row gives")
        keys.store(file, row, row["suffix"], cache)
        rows[row["key"]] = {**row, "run": run}
    return rows
