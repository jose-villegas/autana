#!/usr/bin/env python3
"""Cached bakes: the meshes a pack needs, named by a digest of what makes them.

    python launcher/tools/bake/bake.py list [PATH ...] [--missing] [--kind mesh|fit]
    python launcher/tools/bake/bake.py bake [PATH ...] [--kind KIND] [--only OUTPUT ...] [--again] [--out DIR]
    python launcher/tools/bake/bake.py lock [PATH ...] [--from-run N | --seed] [--cache DIR]
    python launcher/tools/bake/bake.py check [PATH ...]
    python launcher/tools/bake/bake.py fetch [PATH ...] [--cache DIR] [--offline]
    python launcher/tools/bake/bake.py publish

PATH is what build_pack.py takes; with none, launcher/main is searched. A
bake's key is a SHA-256 of its kind, its recipe (the parsed import and
renderer settings and the scene inputs its look reads), its sources (an LFS
pointer's oid stands for the file, so no source is downloaded) and the code
of the tool that makes it. LOCK, written only by this tool, records for
each key the SHA-256 of the bytes made for it, because a fit is not
byte-reproducible even on one machine, and a mesh bake only on hosts of one
CPU vendor: r3d/isa.py pins the ray tracer to AVX2, whose reciprocal estimates
AMD and Intel each define their own way. The locked meshes are AMD's.

`list` prints every bake with its key, or with `--missing` those LOCK lacks. `bake` makes each bake LOCK has no
row for into the cache (produce.py), and with `--out` also copies what it made
there, the folder a CI run uploads; `--only` limits it to the named outputs and
`--again` re-makes them even when locked, to compare a new make with the lock
(the lock keeps its row). `lock` drops the rows nothing needs;
`--from-run N` adds the rows CI run N made, from its uploads, and is the only
way a new row is written, so every locked file can be published; `--seed`
locks the meshes in the tree as they are, marking each row `seeded`: its bytes
were carried over, not made by the code its key names. `check` fails when LOCK lacks a
needed key or holds one nothing needs. `fetch` puts every
locked file in the cache, from the release when it is not there; `--offline`
never downloads. `publish`, on main in CI only, uploads each locked file the
release lacks, taking a row's file from the run that made it. The cache is %LOCALAPPDATA%/autana/bakes, else
$XDG_CACHE_HOME/autana/bakes, else ~/.cache/autana/bakes, shared by every
clone. Standard library only; Python 3.12 or later.
"""

import argparse
import ast
import dataclasses
import functools
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import tokenize
import tomllib
import urllib.error
import urllib.request

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from r3d.import_settings import BLEND_SUFFIX, SettingsError, content_checksum, source_digest  # noqa: E402

REPO = TOOLS.parents[1]
LOCK = REPO / "launcher" / "bakes.lock"
RELEASE_TAG = "bakes"
RELEASE_URL = f"https://github.com/jose-villegas/autana/releases/download/{RELEASE_TAG}/"
MESH_SUFFIX = ".mesh"
DOWNLOAD_TIMEOUT_S = 60
RUN_ARTIFACTS = "bakes-*"
# A lock row's fields, in file order: "host" is the system and machine a run made the bytes on, since a
# bake is reproducible only on one kind of host; a seeded row has none.
ROW_FIELDS = ("output", "source", "key", "sha256", "size", "run", "host", "seeded")
# Each stage's entry scripts. Its code is their import closure (see stage_files), the C and the
# submodules its files compile, and the requirements beside them.
STAGES = {
    "mesh": ("r3d/mesh_import.py",),
    "reference": ("r3d/mesh_import.py", "r3d/reference_render.py", "r3d/fitted_variant.py"),
    "fit": ("r3d/fitted_variant.py", "r3d/appearance_simplify.py"),
    "blend": ("gltf/blend_skin_to_glb.py",),
}
BLEND_EXPORT = "gltf/blend_skin_to_glb.py"
GLB_SUFFIX = ".glb"
# The order kinds are made in: a mesh reads the export of its .blend.
KINDS = ("blend", "mesh", "fit")
REQUIREMENTS_NAME = "requirements.txt"
REQUIREMENTS_GLOB = "requirements*.txt"
C_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp")
# Token kinds that carry no code: a comment or blank-line edit does not rebake.
SKIPPED_TOKENS = {"COMMENT", "NL", "ENCODING"}
SHAPE_TOKENS = {"NEWLINE", "INDENT", "DEDENT"}
QUOTED_INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
# What a module-level constant may be built from when it is read without running the module.
CONSTANT_NAMES = {"pathlib": pathlib, "os": os, "str": str, "sorted": sorted, "tuple": tuple, "list": list}


class BakeMissing(Exception):
    """One or more bakes are not available; the message names each."""


@dataclasses.dataclass(frozen=True)
class Bake:
    output: str            # the file it makes: "<id>.mesh"
    source: pathlib.Path   # the file that asks for it
    holder: str            # the scene object, or the import variant, it is made for
    kind: str              # "blend" | "mesh" | "fit"
    key: str               # SHA-256 hex of everything that determines it
    suffix: str
    tree: pathlib.Path     # where the tree keeps it today
    job: object = dataclasses.field(default=None, compare=False, repr=False)
    scene: object = dataclasses.field(default=None, compare=False, repr=False)
    stages: dict = dataclasses.field(default=None, compare=False, repr=False)


def default_cache():
    for name in ("LOCALAPPDATA", "XDG_CACHE_HOME"):
        if os.environ.get(name):
            return pathlib.Path(os.environ[name]) / "autana" / "bakes"
    return pathlib.Path.home() / ".cache" / "autana" / "bakes"


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def git_lines(*args):
    return subprocess.run(["git", "-C", str(REPO), *args], check=True, capture_output=True,
                          text=True).stdout.splitlines()


@functools.cache
def submodules():
    """{path: pinned commit} of every submodule .gitmodules names, from the index: no checkout
    needed. A submodule the index lacks fails, since its code would drop out of every key."""
    found = {}
    for line in git_lines("ls-files", "-s"):
        meta, _, path = line.partition("\t")
        mode, commit = meta.split()[:2]
        if mode == "160000":
            found[(REPO / path).resolve()] = commit
    named = [line.split("=", 1)[1].strip() for line in (REPO / ".gitmodules").read_text(encoding="utf-8").splitlines()
             if line.strip().startswith("path")]
    lost = [path for path in named if (REPO / path).resolve() not in found]
    if lost:
        raise SettingsError(f"the checkout is incomplete: .gitmodules names {', '.join(lost)}, the index has "
                            "no gitlink; keys made from it would leave that code out, so none are made")
    return found


@functools.cache
def tracked_modules():
    """The names a tracked .py file or package can be imported as: an import of one of them that
    no root holds is code this tool could not place, not a third-party package."""
    names = set()
    for line in git_lines("ls-files", "*.py"):
        path = pathlib.PurePosixPath(line)
        names.add(path.parent.name if path.name == "__init__.py" else path.stem)
    return names


@functools.cache
def module_facts(path):
    """A module's tree, the module-level constants that can be read without running it, and the
    directories it puts on sys.path."""
    tree = ast.parse(path.read_bytes(), str(path))
    constants = {**CONSTANT_NAMES, "__file__": str(path)}
    added = []
    for node in tree.body:
        targets = node.targets if isinstance(node, ast.Assign) else []
        if len(targets) == 1 and isinstance(targets[0], ast.Name):
            try:
                constants[targets[0].id] = eval(compile(ast.Expression(node.value), str(path), "eval"),
                                                {"__builtins__": {}}, constants)
            except Exception:  # a constant that needs the module to run names no file
                pass
        call = node.value if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call) else None
        if call is not None and ast.unparse(call.func) in ("sys.path.insert", "sys.path.append"):
            try:
                added.append(pathlib.Path(eval(compile(ast.Expression(call.args[-1]), str(path), "eval"),
                                               {"__builtins__": {}}, constants)).resolve())
            except Exception as error:
                raise SettingsError(f"{path}: cannot read the directory {ast.unparse(call)} adds, so what "
                                    f"it imports through it cannot be keyed ({error})") from None
    return tree, constants, tuple(added)


def module_file(name, package, level, roots):
    """The file `import name` loads from `package` at relative `level`, searched in `roots` for an
    absolute import, or None when no root holds it."""
    parts = name.split(".") if name else []
    if level:
        bases = [TOOLS.joinpath(*package.split(".")[:len(package.split(".")) - level + 1])]
    else:
        bases = roots
    for base in bases:
        for path in (base.joinpath(*parts).with_suffix(".py") if parts else None, base.joinpath(*parts) / "__init__.py"):
            if path is not None and path.is_file():
                return path.resolve()
    return None


def imports_bake(tree):
    return any(isinstance(node, ast.ImportFrom) and node.level == 0 and (node.module or "").split(".")[0] == "bake"
               for node in ast.walk(tree))


def imported_names(tree):
    """(name, level) of every import in a module, the module itself and each name taken from it."""
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            yield from ((alias.name, 0) for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            yield node.module or "", node.level
            yield from (((node.module + "." if node.module else "") + alias.name, node.level) for alias in node.names)


def closure(entries, stop=()):
    """Every file the entry scripts import, at any depth, found under TOOLS or in a directory a file
    of the closure puts on sys.path, and the packages holding them. A file in `stop`, or one that
    imports this tool (it consumes bakes, so it makes none), is neither counted nor followed. An
    import of a tracked module that no root holds fails: its code would be left out of the key."""
    roots = [TOOLS]
    while True:
        known = len(roots)
        found, pending, unplaced = set(), [(TOOLS / entry).resolve() for entry in entries], []
        while pending:
            path = pending.pop()
            if path in found or path in stop:
                continue
            tree, _, added = module_facts(path)
            if imports_bake(tree):
                continue
            found.add(path)
            roots += [directory for directory in added if directory not in roots]
            package = ".".join(path.relative_to(TOOLS).parts[:-1]) if path.is_relative_to(TOOLS) else ""
            for depth in range(len(package.split(".")) if package else 0):
                init = TOOLS.joinpath(*package.split(".")[:depth + 1], "__init__.py")
                if init.is_file():
                    pending.append(init.resolve())
            for name, level in imported_names(tree):
                target = module_file(name, package, level, roots)
                if target is not None:
                    pending.append(target)
                elif level == 0 and name and name.split(".")[0] not in sys.stdlib_module_names:
                    unplaced.append((path, name.split(".")[0]))
        if len(roots) == known:
            break
    lost = sorted({(relative(path), name) for path, name in unplaced
                   if name in tracked_modules() and module_file(name, "", 0, roots) is None})
    if lost:
        raise SettingsError("imports of tracked modules bake.py cannot place, so their code would be left "
                            "out of the key: " + ", ".join(f"{name} in {path}" for path, name in lost))
    return sorted(found)


def code_tokens(path):
    """A file's tokens without comments, blank lines or layout: the same on every Python from 3.12."""
    with path.open("rb") as source:
        return [[tokenize.tok_name[token.type], "" if tokenize.tok_name[token.type] in SHAPE_TOKENS else token.string]
                for token in tokenize.tokenize(source.readline)
                if tokenize.tok_name[token.type] not in SKIPPED_TOKENS]


@functools.cache
def stage_files(stage):
    """The files whose code makes `stage`'s output: its entries' closure, stopping at another stage's
    entry script, whose key the stage already chains in."""
    own = {(TOOLS / entry).resolve() for entry in STAGES[stage]}
    stop = {(TOOLS / entry).resolve() for entries in STAGES.values() for entry in entries} - own
    return closure(STAGES[stage], stop)


def c_includes(sources, include_dirs):
    """`sources` and every header they reach by #include "...", found beside the includer or in
    `include_dirs`, as the compiler searches; <...> system headers are not followed."""
    found, pending = set(), [pathlib.Path(path).resolve() for path in sources]
    while pending:
        path = pending.pop()
        if path in found:
            continue
        found.add(path)
        for name in QUOTED_INCLUDE.findall(path.read_bytes()):
            for base in (path.parent, *include_dirs):
                header = (base / name.decode()).resolve()
                if header.is_file():
                    pending.append(header)
                    break
    return sorted(found)


def path_constants(value):
    if isinstance(value, pathlib.Path):
        return [value.resolve()]
    if isinstance(value, (tuple, list)):
        return [path for item in value for path in path_constants(item)]
    return []


def native_inputs(files):
    """What a stage's files compile: each module-level path constant naming a C file counts with the
    quoted headers it reaches, and one inside a submodule counts as that submodule's pinned commit, so
    a clone that never checked the submodule out keys it the same."""
    pinned, sources, keyed = submodules(), [], {}

    def holder(item):
        return next((root for root in pinned if item == root or item.is_relative_to(root)), None)

    for path in files:
        for value in module_facts(path)[1].values():
            for item in path_constants(value):
                if holder(item) is not None:
                    keyed[relative(holder(item))] = pinned[holder(item)]
                elif item.suffix in C_SUFFIXES and item.is_file():
                    sources.append(item)
    for item in c_includes(sources, (REPO / "launcher" / "main",)):
        if holder(item) is not None:
            keyed[relative(holder(item))] = pinned[holder(item)]
        else:
            keyed[relative(item)] = file_sha256(item)
    return keyed


def requirement_lines(path):
    lines = (line.split("#", 1)[0].strip() for line in path.read_text(encoding="utf-8").splitlines())
    return [line for line in lines if line]


def package_name(text):
    return re.split(r"[=<>!~\[@; ]", text, maxsplit=1)[0].lower().replace("-", "_").replace(".", "_")


@functools.cache
def requirement_pins(stage):
    """(file, line) of every requirement a stage's key counts: each line of the requirements.txt
    beside a stage file, and each line of any requirements*.txt under TOOLS naming a package a stage
    file imports (the fit's GPU packages, for one)."""
    paths = stage_files(stage)
    imported = {package_name(name.split(".")[0]) for path in paths
                for name, level in imported_names(module_facts(path)[0]) if level == 0 and name}
    pins = set()
    for near in {path.parent / REQUIREMENTS_NAME for path in paths}:
        if near.is_file():
            pins.update((relative(near), line) for line in requirement_lines(near))
    for listed in TOOLS.rglob(REQUIREMENTS_GLOB):
        pins.update((relative(listed), line) for line in requirement_lines(listed) if package_name(line) in imported)
    return tuple(sorted(pins))


@functools.cache
def tool_digest(stage):
    """The code of a stage: its files' tokens, what they compile and the requirements it counts."""
    paths = stage_files(stage)
    files = {relative(path): code_tokens(path) for path in paths}
    files.update(native_inputs(paths))
    return digest([files, [list(pin) for pin in requirement_pins(stage)]])


def canonical(value):
    from r3d.fitted_variant import canonical as parsed

    return parsed(value)


def camera_inputs(scene):
    """The camera a look reads: its lens and region, and its clip's baked bytes, not its file path."""
    from r3d.fitted_variant import camera_clip

    camera = scene.camera.component
    lens = {name: canonical(value) for name, value in vars(camera).items() if name != "path"}
    clip = hashlib.sha256(camera_clip(scene)).hexdigest() if camera.path else None
    return {"lens": lens, "clip": clip}


def is_blend(settings):
    return settings.source["path"].suffix.lower() == BLEND_SUFFIX


def blend_key(settings, tools):
    """The key of a .blend source's export: the file, the actions it exports and the exporter."""
    blend = settings.source["path"]
    return digest(["blend", content_checksum(blend).decode(), settings.source.get("clips"), tools["blend"]])


def mesh_recipe(job, scene, tools):
    """What a lit or albedo mesh is made from, without its fit and without paths; its sources count by
    content, so a credit line or a moved file does not rebake, and a .blend by its export's key."""
    settings = {name: canonical(value) for name, value in vars(job.settings).items()
                if name not in ("path", "source", "out_dir", "mesh_dir", "named", "variants")}
    renderer = {name: canonical(value) for name, value in vars(job.renderer).items() if name not in ("settings", "fit")}
    sources = {"blend": blend_key(job.settings, tools)} if is_blend(job.settings) else source_digest(job.settings)
    recipe = {"settings": settings, "renderer": renderer, "sources": sources}
    if job.bake is not None:
        recipe["scene"] = {"lights": canonical(scene.lights), "tonemap_white": scene.tonemap_white,
                           "bake": canonical(job.bake), "indirect": canonical(scene.indirect) if job.bake.indirect else None}
        if job.renderer.visibility is not None:
            recipe["camera"] = camera_inputs(scene)
    return recipe


def fit_recipe(fit):
    return {name: value for name, value in vars(fit).items() if name not in ("sha256", "recipe_sha256")}


def stage_keys(job, scene, tools):
    """{stage: key} of one job: a mesh alone, or a fit's start, references and fit, each keyed on the one before."""
    start = digest(["mesh", mesh_recipe(job, scene, tools), tools["mesh"]])
    if job.renderer.fit is None:
        return {"mesh": start}
    fit = fit_recipe(job.renderer.fit)
    poses = {name: fit[name] for name in ("train_every_ms", "held_out_every_ms", "coverage_every_ms")}
    reference = digest(["reference", start, poses, camera_inputs(scene), tools["reference"]])
    return {"start": start, "reference": reference, "fit": digest(["fit", reference, fit, tools["fit"]])}


def bakes(paths):
    """Every bake the packs under `paths` hold, in pack and entry order."""
    from r3d.build_pack import pack_jobs

    return bakes_in(*pack_jobs(paths))


def bakes_in(packs, jobs):
    """The bakes of build_pack.pack_jobs()'s packs."""
    tools = {stage: tool_digest(stage) for stage in STAGES}
    found, exports = [], {}
    for name, entries in sorted(packs.items()):
        for entry, source in sorted(entries.items()):
            if source.resolve() not in jobs:
                continue
            job, scene, asker = jobs[source.resolve()]
            if is_blend(job.settings):
                blend = job.settings.source["path"]
                key = blend_key(job.settings, tools)
                exports.setdefault(key, Bake(output=blend.with_suffix(GLB_SUFFIX).name, source=job.settings.path,
                                             holder=blend.name, kind="blend", key=key, suffix=GLB_SUFFIX,
                                             tree=blend.with_suffix(GLB_SUFFIX), job=job.settings, stages={"blend": key}))
            keys = stage_keys(job, scene, tools)
            kind = "fit" if "fit" in keys else "mesh"
            holder = job.object.name if job.object is not None else job.renderer.variant.name
            found.append(Bake(output=f"{entry}{MESH_SUFFIX}", source=asker, holder=holder, kind=kind, key=keys[kind],
                              suffix=MESH_SUFFIX, tree=source, job=job, scene=scene, stages=keys))
    return list(exports.values()) + found


def relative(path):
    path = pathlib.Path(path).resolve()
    return path.relative_to(REPO).as_posix() if path.is_relative_to(REPO) else str(path)


def read_lock(path=LOCK):
    """{key: row} of LOCK; empty when there is none."""
    if not path.is_file():
        return {}
    with path.open("rb") as source:
        rows = tomllib.load(source).get("bake", [])
    return {row["key"]: row for row in rows}


def toml_value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return json.dumps(value) if isinstance(value, str) else str(value)


def write_lock(rows, path=LOCK):
    """LOCK from `rows`, one table per key, sorted by output then key."""
    lines = ["# Written by launcher/tools/bake/bake.py: the bytes made for each bake's key. Do not edit.", ""]
    for row in sorted(rows, key=lambda row: (row["output"], row["key"])):
        lines.append("[[bake]]")
        lines += [f"{name} = {toml_value(row[name])}" for name in ROW_FIELDS
                  if name in row]
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def file_sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def cached(row, suffix, cache):
    return pathlib.Path(cache) / f"{row['sha256']}{suffix}"


def store(path, row, suffix, cache):
    """Copies `path` into the cache under its locked SHA-256, by rename, so a cut copy leaves nothing."""
    target = cached(row, suffix, cache)
    if target.is_file() and file_sha256(target) == row["sha256"]:
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as scratch:
        scratch.write(pathlib.Path(path).read_bytes())
    os.replace(scratch.name, target)
    return target


def describe(bake, why, again):
    return (f"  {bake.output}  key {bake.key}  asked for by {relative(bake.source)} ({bake.holder})\n"
            f"    {why}\n    {again}")


def bake_again(bake):
    if bake.kind == "blend":
        return ("an export needs Blender: push, the Bakes check exports it on the pull request; then "
                "bake.py lock --from-run N with that run")
    if bake.kind == "fit":
        return ("a fit needs the CUDA GPU: run the Bakes GPU workflow on this branch, then "
                "bake.py lock --from-run N with that run")
    return "push: the Bakes check bakes it on the pull request; then bake.py lock --from-run N with that run"


def download(row, suffix, cache):
    """The locked file from the release into the cache, or None when the release has no such file."""
    name = f"{row['sha256']}{suffix}"
    try:
        with urllib.request.urlopen(RELEASE_URL + name, timeout=DOWNLOAD_TIMEOUT_S) as response:
            data = response.read()
    except urllib.error.HTTPError as error:
        error.close()
        if error.code == 404:
            return None
        raise
    target = cached(row, suffix, cache)
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as scratch:
        scratch.write(data)
    if hashlib.sha256(data).hexdigest() != row["sha256"]:
        os.unlink(scratch.name)
        raise BakeMissing(f"{RELEASE_URL}{name} does not have the SHA-256 its name and the lock give")
    os.replace(scratch.name, target)
    return target


def fetch_all(found, lock, cache, offline=False):
    """{bake: its file in the cache}; raises BakeMissing naming every bake that is not available."""
    paths, missing = {}, []
    for bake in found:
        row = lock.get(bake.key)
        if row is None:
            missing.append(describe(bake, f"{relative(LOCK)} has no row for this key: its recipe, a source or "
                                    "its tool changed", bake_again(bake)))
            continue
        path = cached(row, bake.suffix, cache)
        if path.is_file() and file_sha256(path) == row["sha256"]:
            paths[bake] = path
            continue
        path, where = None, "offline" if offline else "not on the release"
        if not offline:
            try:
                path = download(row, bake.suffix, cache)
            except (urllib.error.URLError, TimeoutError) as error:
                path, where = None, f"the release could not be reached ({error}); a cold cache needs the network"
        if path is None:
            missing.append(describe(bake, f"locked as {row['sha256']}, not in {cache} and {where}",
                                    "run bake.py fetch online, or wait for main to publish it"))
            continue
        paths[bake] = path
    if missing:
        count = "1 bake is" if len(missing) == 1 else f"{len(missing)} bakes are"
        raise BakeMissing(f"{count} not available:\n" + "\n".join(missing))
    return paths


def check(found, lock):
    """The problems of LOCK against the bakes `found` need: a key missing, or a key nothing needs."""
    needed = {bake.key for bake in found}
    problems = [describe(bake, f"{relative(LOCK)} has no row for this key", bake_again(bake))
                for bake in found if bake.key not in lock]
    problems += [f"  {row['output']}  key {key}: locked, but nothing needs it; run bake.py lock"
                 for key, row in sorted(lock.items()) if key not in needed]
    return problems


def seed(found, cache, lock=None):
    """Rows locking the tree's own files, each copied into the cache and marked seeded: the bytes are
    carried over from the tree, not made by the code their key names. A run that makes the key again
    writes a row without the mark. A key `lock` holds a made row for keeps it, as with a run's make."""
    rows, missing = [], []
    for bake in found:
        made = (lock or {}).get(bake.key)
        if made is not None and not made.get("seeded"):
            rows.append({name: made[name] for name in ROW_FIELDS if name in made})
            continue
        if not bake.tree.is_file():
            missing.append(describe(bake, f"{relative(bake.tree)} is not in the tree to seed from", bake_again(bake)))
            continue
        data = bake.tree.read_bytes()
        row = {"output": bake.output, "source": relative(bake.source), "key": bake.key,
               "sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "seeded": True}
        store(bake.tree, row, bake.suffix, cache)
        rows.append(row)
    if missing:
        raise BakeMissing("cannot seed:\n" + "\n".join(missing))
    return rows


def run_files(run, folder):
    """Downloads what CI run `run` uploaded into `folder`."""
    subprocess.run(["gh", "run", "download", str(run), "--pattern", RUN_ARTIFACTS, "--dir", str(folder)], check=True)
    return folder


def lock_rows(found, lock, made=None):
    """Rows for the bakes `found` need: the lock's own, else those in `made`; raises naming the rest."""
    rows, missing = [], []
    for bake in found:
        row = lock.get(bake.key)
        if row is None or (row.get("seeded") and bake.key in (made or {})):
            row = (made or {}).get(bake.key)  # a run's make replaces a seeded row, never a made one
        if row is None:
            missing.append(describe(bake, "no CI run has made this key", bake_again(bake)))
            continue
        rows.append({name: row[name] for name in ROW_FIELDS if name in row})
    if missing:
        raise BakeMissing("cannot lock:\n" + "\n".join(missing))
    return rows


def release_assets():
    out = subprocess.run(["gh", "release", "view", RELEASE_TAG, "--json", "assets"], capture_output=True, text=True)
    if out.returncode:
        return None
    return {asset["name"] for asset in json.loads(out.stdout)["assets"]}


def publish(found, lock, cache):
    """Uploads each locked file the release lacks, from the cache or the tree, checked against the lock.
    A bake LOCK has no row for is reported and skipped: there are no bytes to publish for it."""
    if os.environ.get("GITHUB_REF") != "refs/heads/main" or not os.environ.get("GITHUB_ACTIONS"):
        raise BakeMissing("publish runs only in the Bakes workflow on main")
    for problem in check(found, lock):
        print(f"not published:\n{problem}")
    found = [bake for bake in found if bake.key in lock]
    assets = release_assets()
    if assets is None:
        subprocess.run(["gh", "release", "create", RELEASE_TAG, "--prerelease", "--title", "Cached bakes",
                        "--notes", "Files named by SHA-256, locked by launcher/bakes.lock. Written by CI only."],
                       check=True)
        assets = set()
    for bake in found:
        row = lock[bake.key]
        name = f"{row['sha256']}{bake.suffix}"
        if name in assets:
            continue
        path = cached(row, bake.suffix, cache)
        if not (path.is_file() and file_sha256(path) == row["sha256"]) and "run" in row:
            from bake import produce

            with tempfile.TemporaryDirectory() as folder:
                produce.import_run(run_files(row["run"], pathlib.Path(folder)), row["run"], cache)
        if not (path.is_file() and file_sha256(path) == row["sha256"]):
            if not (bake.tree.is_file() and file_sha256(bake.tree) == row["sha256"]):
                raise BakeMissing(describe(bake, f"locked as {row['sha256']}, but neither run {row.get('run')} nor "
                                           "the tree has those bytes", "lock it again from a run that made it"))
            path = store(bake.tree, row, bake.suffix, cache)
        subprocess.run(["gh", "release", "upload", RELEASE_TAG, str(path)], check=True)
        assets.add(name)
        print(f"published {name} ({bake.output})")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("list", "bake", "lock", "check", "fetch", "publish"))
    parser.add_argument("paths", nargs="*", help="what build_pack.py takes; launcher/main when omitted")
    parser.add_argument("--cache", help="the cache directory; the user cache when omitted")
    parser.add_argument("--seed", action="store_true", help="lock: lock the tree's own files")
    parser.add_argument("--from-run", type=int, metavar="N", help="lock: add the rows CI run N made")
    parser.add_argument("--kind", choices=KINDS, help="list, bake: only this kind")
    parser.add_argument("--blender", help="bake: the Blender to export with; `blender` on PATH when omitted")
    parser.add_argument("--missing", action="store_true", help="list: only the bakes LOCK has no row for")
    parser.add_argument("--out", help="bake: also copy what was made here, for a CI run to upload")
    parser.add_argument("--only", action="append", metavar="OUTPUT", help="bake: only this output; repeatable")
    parser.add_argument("--again", action="store_true", help="bake: make them even when locked, and compare")
    parser.add_argument("--offline", action="store_true", help="fetch: never download")
    args = parser.parse_args(argv)
    from r3d.build_pack import DEFAULT_SEARCH

    cache = pathlib.Path(args.cache) if args.cache else default_cache()
    try:
        found = bakes(args.paths or [DEFAULT_SEARCH])
        lock = read_lock()
        if args.command == "list":
            for bake in found:
                if (args.missing and bake.key in lock) or args.kind not in (None, bake.kind):
                    continue
                print(f"{bake.output}\t{bake.kind}\t{bake.key}\t{relative(bake.source)}")
        elif args.command == "bake":
            from bake import produce

            wanted = sorted((bake for bake in found if (args.again or bake.key not in lock)
                             and args.kind in (None, bake.kind) and (not args.only or bake.output in args.only)),
                            key=lambda bake: KINDS.index(bake.kind))
            unknown = sorted(set(args.only or ()) - {bake.output for bake in found})
            if unknown:
                parser.error(f"--only: no bake makes {', '.join(unknown)}")
            rows = [(None if args.again else produce.made(bake.key, cache)) or produce.produce(bake, cache, lock, args.blender)
                    for bake in wanted]
            for row in rows:
                locked = lock.get(row["key"])
                versus = "" if locked is None else (
                    "; the same bytes as the lock" if locked["sha256"] == row["sha256"]
                    else f"; differs from the lock's {locked['sha256']}, which keeps its row")
                print(f"made {row['output']}  key {row['key']}  sha256 {row['sha256']}{versus}")
            if args.out:
                produce.export(rows, cache, pathlib.Path(args.out))
        elif args.command == "lock":
            if args.seed:
                rows = seed(found, cache, lock)
            elif args.from_run is not None:
                from bake import produce

                with tempfile.TemporaryDirectory() as folder:
                    made = produce.import_run(run_files(args.from_run, pathlib.Path(folder)), args.from_run, cache)
                rows = lock_rows(found, lock, made)
            else:
                rows = lock_rows(found, lock)
            write_lock(rows)
            print(f"locked {len(rows)} bakes in {relative(LOCK)}")
        elif args.command == "check":
            problems = check(found, lock)
            if problems:
                run = os.environ.get("GITHUB_RUN_ID")
                after = (f"\nThis run made the mesh bakes: bake.py lock --from-run {run}, then commit {relative(LOCK)}."
                         if run else "")
                raise BakeMissing(f"{relative(LOCK)} is out of date:\n" + "\n".join(problems) + after)
            seeded = sorted(row["output"] for row in lock.values() if row.get("seeded"))
            if seeded:
                print(f"{len(seeded)} of {len(lock)} rows are seeded: bytes carried over from the tree, not made by "
                      "the code their keys name; a CI run that makes a key again clears it: " + ", ".join(seeded))
        elif args.command == "fetch":
            for bake, path in fetch_all(found, lock, cache, args.offline).items():
                print(f"{bake.output}\t{path}")
        else:
            publish(found, lock, cache)
    except (BakeMissing, SettingsError) as error:
        print(f"bake.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
