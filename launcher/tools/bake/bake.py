#!/usr/bin/env python3
"""Cached bakes: the content a pack needs, named by a digest of what makes them.

    python launcher/tools/bake/bake.py list [PATH ...] [--missing] [--kind mesh|fit|sound]
    python launcher/tools/bake/bake.py bake [PATH ...] [--kind KIND] [--only OUTPUT ...] [--again] [--out DIR]
                                            [--from-run N ...]
    python launcher/tools/bake/bake.py lock [PATH ...] [--from-run N ... | --seed] [--cache DIR]
    python launcher/tools/bake/bake.py check [PATH ...]
    python launcher/tools/bake/bake.py fetch [PATH ...] [--cache DIR] [--offline]
    python launcher/tools/bake/bake.py path OUTPUT [--cache DIR] [--offline]
    python launcher/tools/bake/bake.py publish

PATH is what build_pack.py takes; with none, launcher/main is searched. A
bake's key is a SHA-256 of its kind, its recipe (the parsed import and
renderer settings and the scene inputs its look reads), its sources (an LFS
pointer's oid stands for the file, so no source is downloaded) and the code
of the tool that makes it. LOCK, written only by this tool, records for
each key the SHA-256 of the bytes made for it. A fit is byte-reproducible on
the same GPU model in a pinned environment; a different GPU may differ.
A mesh bake is byte-reproducible only on hosts of one CPU vendor: r3d/isa.py
pins the ray tracer to AVX2, whose reciprocal estimates AMD and Intel each
define their own way. LOCK's `mesh_cpu_vendor` names the vendor its meshes
were made on; a run records its host's, and `lock --from-run` refuses a run
whose meshes were made on another, as `bake --out` does on such a host.

A fit's start is a mesh bake of its own (`<entry>.start.mesh`, locked and published, held by no pack).
The fit's key names the start's locked bytes, so it is unknown until the start is locked: `list` says the
fit waits for its start, and the Bakes GPU run fits only once the start's row is committed. A fit's
references are keyed on what they read (fitted_variant.ReferenceInputs), not on the start.

`list` prints every bake with its key, or with `--missing` those LOCK lacks. `bake` makes each bake LOCK has no
row for into the cache (produce.py), and with `--out` also copies each one there as it is made, the folder a CI
run uploads, so a run cut short still uploads what it finished; `--from-run N` first takes run N's uploads made on a
host like this one into the cache, so a rerun makes only what is left; `--only` limits it to the named outputs and
`--again` re-makes them even when locked, to compare a new make with the lock
(the lock keeps its row). `lock` drops the rows nothing needs, writes the rows it has and then fails naming the bakes still
without one;
`--from-run N` adds the rows CI run N made, from its uploads, and is the only
way a new bake row is written. Sound bakes use the CPU Bakes run. It repeats, so the meshes of a Bakes run
and the fits of a Bakes GPU run lock together, so every locked file can be published; `--seed`
locks the keys LOCK lacks, keeping every row it has: a new key takes the bytes
of the stale row for the same output when there is one, else the tree's file,
and each new row is marked `seeded`: its bytes
were carried over, not made by the code its key names. `check` fails when LOCK lacks a
needed key or holds one nothing needs, and samples every needed poses row again, failing on one
missing, unneeded or different, exactly.

LOCK's `[[poses]]` rows hold the digest and count of each camera-path sampling, keyed on its clip bytes,
sampling arguments and sampler code. `lock` samples and writes them. A key computed from a held row needs
no host C compiler.

`fetch` puts every
locked file in the cache, from the release when it is not there; `--offline`
never downloads; `path` fetches one bake by its output's name (`NAME.glb`)
and prints where it is, for a tool that reads it. `publish`, on main in CI only, uploads each locked file the
release lacks, taking a row's file from the run that made it. The cache is %LOCALAPPDATA%/autana/bakes, else
$XDG_CACHE_HOME/autana/bakes, else ~/.cache/autana/bakes, shared by every
clone. Standard library only; Python 3.12 or later.
"""

import argparse
import ast
import concurrent.futures
import dataclasses
import fnmatch
import functools
import hashlib
import io
import json
import multiprocessing
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import tokenize
import tomllib
import urllib.error
import urllib.request
import zipfile

TOOLS = pathlib.Path(__file__).resolve().parents[1]
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(REPO / "scripts/gates"))
sys.path.insert(0, str(REPO / "scripts/lib"))
from autana_config import user_cache_dir
from http_get import get

from build import packages  # noqa: E402
from build.c_header import read_define  # noqa: E402
from c_lexer import c_code  # noqa: E402
from r3d.import_settings import BLEND_SUFFIX, SettingsError, content_checksum, source_digest  # noqa: E402

LOCK = REPO / "launcher" / "bakes.lock"
RELEASE_TAG = "bakes"
RELEASE_URL = f"https://github.com/jose-villegas/autana/releases/download/{RELEASE_TAG}/"
API_URL = "https://api.github.com/repos/jose-villegas/autana"
MESH_SUFFIX = ".mesh"
# A fit's start, the mesh its fit begins from.
START_MARKER = ".start"
START_SUFFIX = START_MARKER + MESH_SUFFIX
DOWNLOAD_TIMEOUT_S = 60
# A rename into the cache another process holds open is retried this often, waiting a growing step.
PLACE_ATTEMPTS = 8
PLACE_WAIT_S = 0.05
RUN_ARTIFACTS = "bakes-*"
CACHE_VARIABLE = "AUTANA_BAKE_CACHE"
# A lock row's fields, in file order: "host" is the system and machine a run made the bytes on, since a
# bake is reproducible only on one kind of host, and "gpu" the GPU a fit was made on, since a fit is
# reproducible only on that GPU model; a seeded row has neither.
ROW_FIELDS = ("output", "source", "key", "sha256", "size", "run", "host", "gpu", "seeded")
POSE_FIELDS = ("key", "poses", "count", "clip", "host")
CPUINFO = pathlib.Path("/proc/cpuinfo")
NVIDIA_SMI_NAME = ("nvidia-smi", "--query-gpu=name", "--format=csv,noheader")
GPU_QUERY_TIMEOUT_S = 10
# Each stage's entry scripts. Its code is their import closure (see stage_files), the C and the
# submodules its files compile, and the requirements beside them.
STAGES = {
    "poses": ("anim/track_host.py",),
    "mesh": ("r3d/mesh_import.py",),
    "reference": ("r3d/mesh_import.py", "r3d/reference_render.py", "r3d/fitted_variant.py"),
    "fit": ("r3d/fitted_variant.py", "r3d/appearance_simplify.py"),
    "blend": ("gltf/blend_skin_to_glb.py",),
    "sound": ("audio/sound_import.py",),
}
BLEND_EXPORT = "gltf/blend_skin_to_glb.py"
GLB_SUFFIX = ".glb"
# The order kinds are made in: a mesh reads the export of its .blend.
KINDS = ("blend", "mesh", "fit", "sound")
SOUND_SUFFIX = ".sond"
ENTRIES = {(TOOLS / entry).resolve() for entries in STAGES.values() for entry in entries}
REQUIREMENTS_NAME = "requirements.txt"
REQUIREMENTS_GLOB = "requirements*.txt"
C_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp")
# Token kinds that carry no code: a comment or blank-line edit does not rebake.
SKIPPED_TOKENS = {"COMMENT", "NL", "ENCODING"}
SHAPE_TOKENS = {"NEWLINE", "INDENT", "DEDENT"}
QUOTED_INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
INCLUDE_LINE = re.compile(rb'^[ \t]*#[ \t]*include\b[^\n]*\n?', re.MULTILINE)
# What a module-level constant may be built from when it is read without running the module.
CONSTANT_NAMES = {"pathlib": pathlib, "Path": pathlib.Path, "os": os, "str": str, "sorted": sorted, "tuple": tuple, "list": list}


class BakeMissing(Exception):
    """One or more bakes are not available; the message names each."""


class BakeUnlocked(BakeMissing):
    """Every bake that is not available waits for LOCK to get a row for its key."""


@dataclasses.dataclass(frozen=True)
class Bake:
    output: str            # the file it makes: "<id>.mesh"
    source: pathlib.Path   # the file that asks for it
    holder: str            # the scene object, or the import variant, it is made for
    kind: str              # one of KINDS
    key: str               # SHA-256 hex of everything that determines it
    suffix: str
    tree: pathlib.Path     # the path its import or scene names: how build_pack matches it to an entry
    job: object = dataclasses.field(default=None, compare=False, repr=False)
    scene: object = dataclasses.field(default=None, compare=False, repr=False)
    stages: dict = dataclasses.field(default=None, compare=False, repr=False)
    packed: bool = dataclasses.field(default=True, compare=False)  # a pack holds it: not an export or a fit's start
    start: object = dataclasses.field(default=None, compare=False, repr=False)  # a fit's start Bake


def default_cache():
    """AUTANA_BAKE_CACHE when a process sets it (a build with a cache of its own), else the user cache."""
    if os.environ.get(CACHE_VARIABLE):
        return pathlib.Path(os.environ[CACHE_VARIABLE])
    return user_cache_dir() / "bakes"


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


def closure(entries, stop=(), consumers=False):
    """The entries' import closure, including packages, under TOOLS or sys.path roots the code adds.
    A file in `stop`, any file under bake/, and, unless `consumers`, a file that imports this tool
    (it consumes bakes, so it makes none) are neither counted nor followed. Unplaced tracked imports fail."""
    roots = [TOOLS]
    while True:
        known = len(roots)
        found, pending, unplaced = set(), [(TOOLS / entry).resolve() for entry in entries], []
        while pending:
            path = pending.pop()
            if path in found or path in stop:
                continue
            tree, _, added = module_facts(path)
            if path.is_relative_to(TOOLS / "bake") or (not consumers and imports_bake(tree) and path not in ENTRIES):
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


def plumbing_lines(tree):
    """The lines of a module's import statements and sys.path edits: where its code lives, not what it
    computes. The files they reach are in the stage by content, so a move that rewrites them keys the same."""
    lines = set()
    for node in ast.walk(tree):
        plumbing = isinstance(node, (ast.Import, ast.ImportFrom))
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            plumbing = ast.unparse(node.value.func) in ("sys.path.insert", "sys.path.append")
        if plumbing:
            lines.update(range(node.lineno, node.end_lineno + 1))
    return lines


def string_statement_spans(tree):
    """Spans of statements that are only a string, which compute nothing."""
    return [((node.lineno, node.col_offset), (node.end_lineno, node.end_col_offset))
            for node in ast.walk(tree) if isinstance(node, ast.Expr)
            and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str)]


def code_tokens(path):
    """A file's tokens without comments, docstrings, blank lines, layout or import plumbing: the same
    on every Python from 3.12, and wherever the file lives."""
    tree = module_facts(path)[0]
    skipped = plumbing_lines(tree)
    strings = string_statement_spans(tree)
    kept = []
    with path.open("rb") as source:
        for token in tokenize.tokenize(source.readline):
            kind = tokenize.tok_name[token.type]
            if kind in SKIPPED_TOKENS or token.start[0] in skipped:
                continue
            if any(start <= token.start < end for start, end in strings):
                continue
            if kind == "NEWLINE" and (not kept or kept[-1][0] in SHAPE_TOKENS):
                continue
            kept.append([kind, "" if kind in SHAPE_TOKENS else token.string])
    return kept


def c_content(path):
    """C content without includes, comments or blank lines, with outside-literal spacing normalized.
    Literal bytes and directive newlines count; the included headers count separately."""
    data = INCLUDE_LINE.sub(b"", pathlib.Path(path).read_bytes())
    content = c_code(data.decode("utf-8", errors="surrogateescape"))
    return hashlib.sha256(content.encode("utf-8", errors="surrogateescape")).hexdigest()


@functools.cache
def stage_files(stage):
    """The files whose code makes `stage`'s output: its entries' closure, stopping at another stage's
    entry script, whose key or output the stage already names."""
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

    defines = {}
    for path in files:
        tree, constants, _ = module_facts(path)
        headers = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "read_define":
                try:
                    header = pathlib.Path(eval(compile(ast.Expression(node.args[0]), str(path), "eval"),
                                               {"__builtins__": {}}, constants)).resolve()
                    header_text = header.read_text(encoding="utf-8")
                except (NameError, TypeError, ValueError, IndexError, OSError) as error:
                    raise SettingsError(f"{path}:{node.lineno}: cannot resolve read_define header: {error}") from error
                if header not in defines:
                    values = {}
                    for name in re.findall(r"^#define\s+(\w+)\s+[^\n]+", header_text, re.MULTILINE):
                        try:
                            values[name] = read_define(header, name)
                        except RuntimeError:
                            pass
                    defines[header] = values
                headers.add(header)
        for value in constants.values():
            for item in path_constants(value):
                if holder(item) is not None:
                    keyed[relative(holder(item))] = pinned[holder(item)]
                elif item not in headers and item.suffix in C_SUFFIXES and item.is_file():
                    sources.append(item)
    # Literal defines are data inputs; their include chains are not compiled by the stage.
    for header, values in defines.items():
        keyed[relative(header)] = digest(sorted(values.items()))
    for item in c_includes(sources, packages.include_roots()):
        if holder(item) is not None:
            keyed[relative(holder(item))] = pinned[holder(item)]
        else:
            keyed[relative(item)] = c_content(item)
    return keyed


def requirement_lines(path):
    lines = (line.split("#", 1)[0].strip() for line in path.read_text(encoding="utf-8").splitlines())
    return [line for line in lines if line]


def package_name(text):
    return re.split(r"[=<>!~\[@; ]", text, maxsplit=1)[0].lower().replace("-", "_").replace(".", "_")


@functools.cache
def requirement_pins(stage):
    """(file, line) of every requirement a stage's key counts (see pinned_requirements)."""
    return pinned_requirements(stage_files(stage))


@functools.cache
def tracked_requirements():
    """The requirements*.txt files git tracks under TOOLS: a scratch copy of the tree in an ignored folder
    holds others, which no key counts."""
    return [REPO / line for line in git_lines("ls-files", "--", relative(TOOLS))
            if fnmatch.fnmatch(pathlib.PurePosixPath(line).name, REQUIREMENTS_GLOB)]


def pinned_requirements(paths):
    """(file, line) of each line of the requirements.txt beside one of `paths`, and of each line of any
    tracked requirements*.txt under TOOLS naming a package one of them imports (the fit's GPU packages, for one)."""
    imported = {package_name(name.split(".")[0]) for path in paths
                for name, level in imported_names(module_facts(path)[0]) if level == 0 and name}
    pins = set()
    for near in {path.parent / REQUIREMENTS_NAME for path in paths}:
        if near.is_file():
            pins.update((relative(near), line) for line in requirement_lines(near))
    for listed in tracked_requirements():
        pins.update((relative(listed), line) for line in requirement_lines(listed) if package_name(line) in imported)
    return tuple(sorted(pins))


@functools.cache
def tool_digest(stage):
    """The code of a stage by content alone: its files' tokens and the C they compile as a sorted list of
    digests, no paths, so a move or rename keys the same; plus the submodules it builds and the
    requirements it counts."""
    paths = stage_files(stage)
    native = native_inputs(paths)
    pinned = {path: commit for path, commit in native.items() if (REPO / path).resolve() in submodules()}
    contents = [digest(code_tokens(path)) for path in paths]
    contents += [value for path, value in native.items() if path not in pinned]
    return digest([sorted(contents), pinned, [list(pin) for pin in requirement_pins(stage)]])


def canonical(value):
    from r3d.fitted_variant import canonical as parsed

    return parsed(value)


class Poses:
    """Sampled-poses digests read from held lock rows or sampled this run; `used` holds their rows."""
    def __init__(self, held, resample=False):
        self.held, self.resample = held, resample
        self.used, self.missing = {}, {}

    def digest(self, camera, every_ms, size, tools, unavailable=None):
        """The digest of parsed camera rows, or None when the sampler cannot supply a missing row."""
        from anim import tracks_asset

        path = camera.path
        if path is None or size is None:
            raise SettingsError("camera poses need camera_path visibility and a camera path")
        if not path.animation.is_file():
            raise SettingsError(f"camera path animation is missing: {path.animation}")
        clip = hashlib.sha256(tracks_asset.bake(path.animation)).hexdigest()
        key = digest(["poses", clip, path.node, every_ms, list(size), camera.half_fov_short_tan,
                      camera.near_z, tools["poses"]])
        row = self.used.get(key) or (None if self.resample else self.held.get(key))
        if row is None and key not in self.missing:
            from anim.track_host import TrackHostError
            from r3d.poses import sample_camera_rows

            try:
                width, height, lens, near, rows = sample_camera_rows(
                    path.animation, path.node, every_ms, *size, camera.half_fov_short_tan, camera.near_z)
            except TrackHostError:
                self.missing[key] = (f"poses clip {relative(path.animation)}, node {path.node}, "
                                     f"every_ms {every_ms}, size {tuple(size)} is not available")
            else:
                row = {"key": key, "poses": digest([width, height, lens, near, [list(map(float, r)) for r in rows]]),
                       "count": len(rows), "clip": relative(path.animation),
                       "host": f"{platform.system()} {platform.machine()}"}
        if row is None:
            if unavailable is not None:
                unavailable.add(key)
            return None
        self.used[key] = row
        return row["poses"]

    def reason(self, keys):
        return "; ".join(self.missing[key] for key in sorted(keys))


def camera_inputs(scene):
    """The camera lens recipe; sampled-poses digests are supplied by Poses."""
    camera = scene.camera.component
    return {"lens": {name: canonical(value) for name, value in vars(camera).items() if name != "path"}}


def is_blend(settings):
    return settings.source["path"].suffix.lower() == BLEND_SUFFIX


def blend_key(settings, tools):
    """The key of a .blend source's export: the file, the actions it exports and the exporter."""
    blend = settings.source["path"]
    return digest(["blend", content_checksum(blend).decode(), settings.source.get("clips"), tools["blend"]])


def import_recipe(settings, tools):
    """An import's settings without paths and its sources by content: a credit line or a moved file does not
    rebake, and a .blend counts by its export's key."""
    recipe = {name: canonical(value) for name, value in vars(settings).items()
              if name not in ("path", "source", "out_dir", "mesh_dir", "named", "variants")}
    return recipe, {"blend": blend_key(settings, tools)} if is_blend(settings) else source_digest(settings)


def mesh_recipe(job, scene, tools, poses=None, unavailable=None):
    """What a lit or albedo mesh is made from, without its fit and without paths."""
    settings, sources = import_recipe(job.settings, tools)
    renderer = {name: canonical(value) for name, value in vars(job.renderer).items() if name not in ("settings", "fit")}
    recipe = {"settings": settings, "renderer": renderer, "sources": sources}
    if job.settings.meshes:
        recipe["renderer_id"] = job.asset_name
    if job.bake is not None:
        recipe["scene"] = {"lights": canonical(scene.lights), "tonemap_white": scene.tonemap_white,
                           "bake": canonical(job.bake), "indirect": canonical(scene.indirect) if job.bake.indirect else None}
        if job.renderer.visibility is not None:
            visibility = job.renderer.visibility
            camera = recipe["camera"] = camera_inputs(scene)
            if visibility.source == "camera_path":
                camera["poses"] = poses.digest(scene.camera.component, visibility.every_ms, visibility.size, tools, unavailable)
    return recipe


def fit_recipe(fit):
    return dict(vars(fit))


def reference_recipe(inputs, tools, poses=None, unavailable=None):
    """A fit's reference set by every field of its fitted_variant.ReferenceInputs: the import as a mesh counts
    it, the camera by its lens and sampled poses, the rest as parsed."""
    recipe = {}
    for field in dataclasses.fields(inputs):
        value = getattr(inputs, field.name)
        if field.name == "settings":
            recipe["settings"], recipe["sources"] = import_recipe(value, tools)
        elif field.name == "camera":
            scene = inputs.scene()
            camera = camera_inputs(scene)
            camera["poses"] = {name: poses.digest(scene.camera.component, inputs.poses[field], inputs.size, tools, unavailable)
                               for name, field in (("train", "train_every_ms"),
                                                   ("coverage", "coverage_every_ms"))}
            recipe["camera"] = camera
        else:
            recipe[field.name] = canonical(value)
    return recipe


def stage_keys(job, scene, tools, starts=None, poses=None):
    """{stage: key} of one job: a mesh alone, or a fit's start, references and fit. The references are keyed
    on what they read, not on the start, so fits that differ only in their start share them; the fit is
    keyed on its references and on its start's bytes, which `starts` ({start key: sha256}) gives, and is
    None while they are unknown. A key is None while a poses digest it needs cannot be had."""
    poses = Poses({}) if poses is None else poses
    unavailable = set()
    recipe = mesh_recipe(job, scene, tools, poses, unavailable)
    start = None if unavailable else digest(["mesh", recipe, tools["mesh"]])
    keys = {"mesh": start}
    if job.renderer.fit is not None:
        from r3d.fitted_variant import reference_inputs

        recipe = reference_recipe(reference_inputs(job, scene), tools, poses, unavailable)
        reference = None if unavailable else digest(["reference", recipe, tools["reference"]])
        start_sha256 = None if starts is None else starts.get(start)
        fit = None if start_sha256 is None or reference is None else digest(
            ["fit", reference, start_sha256, fit_recipe(job.renderer.fit), tools["fit"]])
        keys = {"start": start, "start_sha256": start_sha256, "reference": reference, "fit": fit}
    if unavailable:
        keys["reason"] = poses.reason(unavailable)
    return keys


def bakes(paths, starts=None, poses=None):
    """Every bake the packs under `paths` hold, in pack and entry order."""
    from r3d.build_pack import pack_jobs

    return bakes_in(*pack_jobs(paths), starts, poses)


def blend_export(settings, tools):
    """The blend stage record shared by cache producers and pack consumers."""
    blend = settings.source["path"]
    key = blend_key(settings, tools)
    return Bake(output=blend.with_suffix(GLB_SUFFIX).name, source=settings.path,
                holder=blend.name, kind="blend", key=key, suffix=GLB_SUFFIX,
                tree=blend.with_suffix(GLB_SUFFIX), job=settings, stages={"blend": key},
                packed=False)


def locked_starts(lock, *made):
    """{key: sha256} of the start bytes a fit's key names: the lock's row, except that a run's make replaces a
    seeded one, as lock_rows writes it."""
    starts = {key: row["sha256"] for key, row in lock.items()}
    for rows in made:
        for key, row in rows.items():
            if key not in lock or lock[key].get("seeded"):
                starts[key] = row["sha256"]
    return starts


def bakes_in(packs, jobs, starts=None, poses=None):
    """The bakes of build_pack.pack_jobs()'s packs. A fit's start is a mesh bake of its own, which no pack
    holds; the fit's key reads the start's bytes from `starts` ({key: sha256}, the lock's when None)."""
    from audio import sound_asset

    if starts is None or poses is None:
        tables = read_lock_tables()
        held, rows = tables["bake"], tables["poses"]
        starts = locked_starts(held) if starts is None else starts
        poses = Poses(rows) if poses is None else poses
    tools = {stage: tool_digest(stage) for stage in STAGES}
    found, exports, fit_starts = [], {}, {}
    for name, entries in sorted(packs.items()):
        for entry, source in sorted(entries.items()):
            if source.name.endswith(sound_asset.SUFFIX):
                try:
                    audio, fmt, loop = sound_asset.load_source(source)
                except sound_asset.SoundError as error:
                    raise SettingsError(str(error)) from error
                key = digest(["sound", {"source": audio.name, "format": fmt, "loop": loop},
                              content_checksum(audio).decode(), tools["sound"]])
                found.append(Bake(output=f"{entry}{SOUND_SUFFIX}", source=source, holder=entry,
                                  kind="sound", key=key, suffix=SOUND_SUFFIX, tree=source))
                continue
            if source.resolve() not in jobs:
                continue
            job, scene, asker = jobs[source.resolve()]
            if is_blend(job.settings):
                export = blend_export(job.settings, tools)
                exports.setdefault(export.key, export)
            keys = stage_keys(job, scene, tools, starts, poses)
            kind = "fit" if "fit" in keys else "mesh"
            holder = job.object.name if job.object is not None else job.renderer.variant.name
            start = None
            suffix = source.suffix
            start_suffix = START_MARKER + suffix
            if kind == "fit":
                start = fit_starts.setdefault(keys["start"], Bake(
                    output=f"{entry}{start_suffix}", source=asker, holder=holder, kind="mesh", key=keys["start"],
                    suffix=suffix, tree=source.with_name(f"{entry}{start_suffix}"), job=job, scene=scene,
                    stages={"mesh": keys["start"], **({"reason": keys["reason"]} if "reason" in keys else {})}, packed=False))
            found.append(Bake(output=f"{entry}{suffix}", source=asker, holder=holder, kind=kind, key=keys[kind],
                              suffix=suffix, tree=source, job=job, scene=scene, stages=keys, start=start))
    return list(exports.values()) + list(fit_starts.values()) + found


def relative(path):
    path = pathlib.Path(path).resolve()
    return path.relative_to(REPO).as_posix() if path.is_relative_to(REPO) else str(path)


def read_lock_tables(path=LOCK):
    """The bake and poses tables, each as {key: row}, empty when absent, and "mesh_cpu_vendor", None when
    absent."""
    rows = {}
    if path.is_file():
        with path.open("rb") as source:
            rows = tomllib.load(source)
    return {**{name: {row["key"]: row for row in rows.get(name, [])} for name in ("bake", "poses")},
            "mesh_cpu_vendor": rows.get("mesh_cpu_vendor")}


def read_lock(path=LOCK, table="bake"):
    """{key: row} of a lock table; empty when absent."""
    return read_lock_tables(path)[table]


def toml_value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return json.dumps(value) if isinstance(value, str) else str(value)


def write_lock(rows, path=LOCK, poses=(), mesh_cpu_vendor=None):
    """LOCK from `rows`, one table per key sorted by output then key, then the `poses` rows sorted by key."""
    lines = ["# Written by launcher/tools/bake/bake.py: the bytes made for each bake's key. Do not edit.", ""]
    if mesh_cpu_vendor is not None:
        lines += ["# mesh_cpu_vendor: the CPU vendor the meshes were made on; a mesh bake's bytes differ between "
                  "vendors.",
                  f"mesh_cpu_vendor = {toml_value(mesh_cpu_vendor)}", ""]
    for row in sorted(rows, key=lambda row: (row["output"], row["key"])):
        lines.append("[[bake]]")
        lines += [f"{name} = {toml_value(row[name])}" for name in ROW_FIELDS
                  if name in row]
        lines.append("")
    lines += ["# [[poses]]: the digest of each camera-path sampling, keyed on its clip bytes, sampling arguments",
              "# and sampler code. bake.py lock writes them; bake.py check samples them again.", ""]
    for row in sorted(poses, key=lambda row: row["key"]):
        lines.append("[[poses]]")
        lines += [f"{name} = {toml_value(row[name])}" for name in POSE_FIELDS]
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def file_sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def cached(row, suffix, cache):
    return pathlib.Path(cache) / f"{row['sha256']}{suffix}"


def place(data, target, sha256=None):
    """Writes `data` to `target` by rename, so no reader ever sees half a file. Builds run side by side
    share the cache: on Windows a rename onto a file another process has open is refused, so when the
    target already holds these bytes the write is done, and otherwise it is retried briefly. The
    scratch file never stays behind."""
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as scratch:
        scratch.write(data)
    try:
        for attempt in range(PLACE_ATTEMPTS):
            try:
                os.replace(scratch.name, target)
                return target
            except PermissionError:
                if target.is_file() and (sha256 or hashlib.sha256(data).hexdigest()) == file_sha256(target):
                    return target
                time.sleep(PLACE_WAIT_S * (attempt + 1))
        raise BakeMissing(f"{target}: another process holds it; the write was refused {PLACE_ATTEMPTS} times")
    finally:
        if os.path.exists(scratch.name):
            os.unlink(scratch.name)


def store(path, row, suffix, cache):
    """Copies `path` into the cache under its locked SHA-256, by rename, so a cut copy leaves nothing."""
    target = cached(row, suffix, cache)
    if target.is_file() and file_sha256(target) == row["sha256"]:
        return target
    return place(pathlib.Path(path).read_bytes(), target, row["sha256"])


def describe(bake, why, again):
    return (f"  {bake.output}  key {bake.key}  asked for by {relative(bake.source)} ({bake.holder})\n"
            f"    {why}\n    {again}")


def unlocked(bake):
    """Why LOCK has no row for `bake`."""
    if bake.stages and bake.stages.get("reason"):
        return bake.stages["reason"]
    if bake.key is None:
        return (f"its start {bake.stages['start']} is not locked, so its key, which names the start's bytes, "
                "is not known yet")
    return f"{relative(LOCK)} has no row for this key: its recipe, a source or its tool changed"


def bake_again(bake):
    if bake.stages and bake.stages.get("reason"):
        return ("run bake.py lock on a host with a C compiler (track_host.py builds the device sampler), "
                "or the CI Bakes check")
    if bake.key is None:
        return ("lock its start first: bake.py lock --from-run N with the Bakes run that made it, then run the "
                "Bakes GPU workflow on this branch")
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
        data = get(RELEASE_URL + name, timeout=DOWNLOAD_TIMEOUT_S)
    except urllib.error.HTTPError as error:
        error.close()
        if error.code == 404:
            return None
        raise
    if hashlib.sha256(data).hexdigest() != row["sha256"]:
        raise BakeMissing(f"{RELEASE_URL}{name} does not have the SHA-256 its name and the lock give")
    return place(data, cached(row, suffix, cache), row["sha256"])


def fetch_all(found, lock, cache, offline=False):
    """{bake: its file in the cache}; raises BakeMissing naming every bake that is not available,
    BakeUnlocked when each of them only lacks its LOCK row."""
    paths, missing, without_row = {}, [], 0
    for bake in found:
        row = lock.get(bake.key)
        if row is None:
            missing.append(describe(bake, unlocked(bake), bake_again(bake)))
            without_row += 1
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
            if path is None and "run" in row:
                path, why = from_run(row, bake.suffix, cache)
                where = f"not on the release, and run {row['run']}'s uploads could not supply it ({why})"
        if path is None:
            missing.append(describe(bake, f"locked as {row['sha256']}, not in {cache} and {where}",
                                    "run bake.py fetch online, or wait for main to publish it"))
            continue
        paths[bake] = path
    if missing:
        count = "1 bake is" if len(missing) == 1 else f"{len(missing)} bakes are"
        error = BakeUnlocked if without_row == len(missing) else BakeMissing
        raise error(f"{count} not available:\n" + "\n".join(missing))
    return paths


def check(found, lock):
    """The problems of LOCK against the bakes `found` need: a key missing, or a key nothing needs."""
    needed = {bake.key for bake in found}
    problems = [describe(bake, unlocked(bake), bake_again(bake)) for bake in found if bake.key not in lock]
    problems += [f"  {row['output']}  key {key}: locked, but nothing needs it; run bake.py lock"
                 for key, row in sorted(lock.items()) if key not in needed]
    return problems


def check_poses(found, held):
    """Problems of re-sampled digests and counts against held rows: missing, unneeded or different, exactly."""
    poses = Poses(held, resample=True)
    tools = {stage: tool_digest(stage) for stage in STAGES}
    for item in found:
        if item.job is not None and item.scene is not None:
            stage_keys(item.job, item.scene, tools, poses=poses)
    problems = [f"{reason}; run bake.py lock on a host with a C compiler or the CI Bakes check"
                for reason in poses.missing.values()]
    for key, row in sorted(poses.used.items()):
        previous = held.get(key)
        if previous is None:
            problems.append(f"  poses {key}: missing; run bake.py lock")
        elif (previous["poses"], previous["count"]) != (row["poses"], row["count"]):
            problems.append(f"  poses {key}: sampled digest or count differs; run bake.py lock")
    problems += [f"  poses {key}: nothing needs it; run bake.py lock" for key in sorted(held.keys() - poses.used.keys())]
    return problems


def seed(found, cache, lock=None, made=None):
    """Rows marked seeded, whose bytes are carried over, not made by the code their key names; a run
    that makes the key again writes a row without the mark. A key `lock` holds a made row for keeps
    it, as with a run's make. A re-keyed output carries the bytes of the row `lock` holds for it
    under its old key, so what a run made outlives the re-key (its file fetched as any locked file).
    Otherwise the bytes are the tree's own file, or once the tree has none the seeded row's."""
    lock = lock or {}
    needed = {bake.key for bake in found}
    previous = {row["output"]: row for key, row in lock.items() if key not in needed}
    rows, missing = [], []
    for bake in found:
        if bake.key is None:
            missing.append(describe(bake, unlocked(bake), bake_again(bake)))
            continue
        held = lock.get(bake.key) or (made or {}).get(bake.key)
        if held is not None and not held.get("seeded"):
            rows.append({name: held[name] for name in ROW_FIELDS if name in held})
            continue
        carried = previous.get(bake.output) if held is None else (None if bake.tree.is_file() else held)
        if carried is not None:
            fetch_all([bake], {bake.key: carried}, cache)
            rows.append({**{name: carried[name] for name in ("output", "source", "sha256", "size")},
                         "key": bake.key, "seeded": True})
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


def from_run(row, suffix, cache):
    """(the locked file from the CI run that made it, before main publishes it, or None; why not)."""
    from bake import produce

    try:
        with tempfile.TemporaryDirectory() as folder:
            produce.import_run(run_files(row["run"], pathlib.Path(folder)), row["run"], cache)
    except (OSError, urllib.error.URLError, subprocess.CalledProcessError, BakeMissing, zipfile.BadZipFile) as error:
        reason = "; ".join([f"{type(error).__name__}: {error}", *getattr(error, "__notes__", ())])
        return None, reason
    path = cached(row, suffix, cache)
    if not path.is_file():
        return None, "its uploads hold no file for this key"
    if file_sha256(path) != row["sha256"]:
        return None, "its file for this key has other bytes than the lock's"
    return path, None


def github_token():
    """GH_TOKEN or GITHUB_TOKEN as CI sets them, else the signed-in gh's; None without either."""
    for name in ("GH_TOKEN", "GITHUB_TOKEN"):
        if os.environ.get(name):
            return os.environ[name]
    if shutil.which("gh"):
        out = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    return None


class KeepRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def github_get(url, token):
    """GET with the token; a redirect (an artifact's storage link, signed already) is followed
    without it, since the storage refuses a request that carries one."""
    request = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}",
                                                   "Accept": "application/vnd.github+json"})
    try:
        return get(request, timeout=DOWNLOAD_TIMEOUT_S, opener=urllib.request.build_opener(KeepRedirect))
    except urllib.error.HTTPError as error:
        location = error.headers.get("Location")
        error.close()
        if error.code not in (301, 302, 303, 307, 308) or not location:
            raise
    return get(location, timeout=DOWNLOAD_TIMEOUT_S)


def run_files(run, folder):
    """Downloads what CI run `run` uploaded (its bakes-* artifacts) into `folder`, with the standard
    library and GitHub's API, so a runner without gh can too; artifacts need a token even when public."""
    token = github_token()
    if token is None:
        raise BakeMissing(f"reading run {run}'s uploads needs a GitHub token: GH_TOKEN, or gh signed in")
    listing = json.loads(github_get(f"{API_URL}/actions/runs/{run}/artifacts?per_page=100", token))
    for artifact in listing["artifacts"]:
        if fnmatch.fnmatch(artifact["name"], RUN_ARTIFACTS) and not artifact["expired"]:
            data = github_get(artifact["archive_download_url"], token)
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                archive.extractall(pathlib.Path(folder) / artifact["name"])
    return folder


def cpu_vendor():
    """The CPU vendor /proc/cpuinfo names (AuthenticAMD, GenuineIntel), None where there is none."""
    try:
        lines = CPUINFO.read_text().splitlines()
    except OSError:
        return None
    for line in lines:
        if line.startswith("vendor_id"):
            return line.split(":", 1)[1].strip()
    return None


def gpu_name():
    """The first GPU nvidia-smi names (NVIDIA GeForce RTX 2060), None where there is none."""
    try:
        names = subprocess.run(NVIDIA_SMI_NAME, capture_output=True, text=True, timeout=GPU_QUERY_TIMEOUT_S,
                               stdin=subprocess.DEVNULL).stdout.splitlines()
    except (OSError, subprocess.TimeoutExpired):
        return None
    return names[0].strip() if names and names[0].strip() else None


def rerun_on(mesh_cpu_vendor):
    return (f"the lock's meshes are {mesh_cpu_vendor}'s: re-run the failed Bakes CPU job until it lands on a "
            f"{mesh_cpu_vendor} runner")


def from_made(bake, lock, made):
    """Whether `bake`'s row comes from a run's make: the lock has none for it, or only a seeded one."""
    held = lock.get(bake.key)
    return bake.key in (made or {}) and (held is None or bool(held.get("seeded")))


def runs_mesh_cpu_vendor(found, lock, made, mesh_cpu_vendor):
    """The vendor the lock's meshes are from once the meshes in `made` are taken; a lock that names none takes
    theirs. Fails naming each mesh a run made on another vendor, or without saying which, since its bytes
    would not be the lock's."""
    wrong = []
    for bake in found:
        if bake.kind != "mesh" or not from_made(bake, lock, made):
            continue
        row = made[bake.key]
        if mesh_cpu_vendor is None:
            mesh_cpu_vendor = row.get("cpu_vendor")
        elif row.get("cpu_vendor") != mesh_cpu_vendor:
            wrong.append(f"  run {row['run']} made {bake.output} on {row.get('cpu_vendor') or 'a CPU it does not name'}")
    if wrong:
        raise BakeMissing("cannot lock meshes made on another CPU vendor:\n" + "\n".join(wrong) + "\n"
                          + rerun_on(mesh_cpu_vendor) + ", then lock from that run")
    return mesh_cpu_vendor


def runs_made(runs, cache):
    """{key: row} of what each CI run in `runs` uploaded, its files put in the cache. A key two runs made with the
    same bytes keeps the first run's row; with different bytes it fails, naming both, so the author lists one run."""
    from bake import produce

    made = {}
    for run in runs:
        with tempfile.TemporaryDirectory() as folder:
            for key, row in produce.import_run(run_files(run, pathlib.Path(folder)), run, cache).items():
                first = made.setdefault(key, row)
                if first["sha256"] != row["sha256"]:
                    raise BakeMissing(f"{row['output']} key {key}: run {first['run']} made sha256 {first['sha256']}, "
                                      f"run {run} made {row['sha256']}; lock from only the run whose bytes you want")
    return made


def lock_rows(found, lock, made=None, missing=None):
    """Rows for the bakes `found` need: the lock's own, else those in `made`; raises naming the rest, or
    with a `missing` list collects them there and returns the rows it has."""
    rows, collect = [], missing is not None
    missing = [] if missing is None else missing
    for bake in found:
        row = lock.get(bake.key)
        if row is None or from_made(bake, lock, made):
            row = (made or {}).get(bake.key)  # a run's make replaces a seeded row, never a made one
        if row is None:
            why = unlocked(bake) if bake.key is None else "no CI run has made this key"
            missing.append(describe(bake, why, bake_again(bake)))
            continue
        rows.append({name: row[name] for name in ROW_FIELDS if name in row})
    if missing and not collect:
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


def _mesh_rows(found, cache, lock, blender, again, keep):
    """Make each mesh key once, preserving the callers' order for rows and fit starts. keep(row) takes each row the
    moment it is had, and on a worker's failure every row the workers still running finish."""
    from bake import produce
    from r3d import process_budget as budget

    unique = {}
    for item in found:
        if item.key is None:
            raise BakeMissing(describe(item, unlocked(item), "bake its start first: bake.py bake --kind mesh"))
        unique.setdefault(item.key, item)
    rows = {key: row for key in unique if not again and (row := produce.made(key, cache))}
    for row in rows.values():
        keep(row)
    pending = [item for key, item in unique.items() if key not in rows]
    if pending:
        cores = budget.cores_available()
        try:
            capacity = budget.worker_capacity(budget.available_bytes(gpu=False), budget.BAKE_BYTES, budget.FLOORS, cores)
            reason = "memory" if capacity < cores else "cores"
        except (OSError, KeyError, ValueError) as error:
            capacity, reason = 1, f"memory unavailable: {error}"
        workers = max(1, min(capacity, len(pending)))
        print(f"mesh workers: {workers} ({reason}; {len(pending)} unique meshes)")
        if workers == 1:
            for item in pending:
                rows[item.key] = keep(produce.produce(item, cache, lock, blender))
        else:
            executor = concurrent.futures.ProcessPoolExecutor(max_workers=workers,
                                                              mp_context=multiprocessing.get_context("spawn"))
            active = {}
            remaining = iter(pending)
            try:
                while True:
                    while len(active) < workers and (item := next(remaining, None)) is not None:
                        active[executor.submit(produce.produce, item, cache, lock, blender)] = item.key
                    if not active:
                        break
                    done, _ = concurrent.futures.wait(active, return_when=concurrent.futures.FIRST_COMPLETED)
                    for future in done:
                        rows[active.pop(future)] = keep(future.result())
            finally:
                for future in active:
                    future.cancel()
                executor.shutdown(wait=True, cancel_futures=True)
                for future in active:
                    if not future.cancelled() and future.exception() is None:
                        keep(future.result())
    return [rows[item.key] for item in found]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("list", "bake", "lock", "check", "fetch", "path", "publish", "tool"))
    parser.add_argument("paths", nargs="*", help="what build_pack.py takes; launcher/main when omitted")
    parser.add_argument("--cache", help="the cache directory; the user cache when omitted")
    parser.add_argument("--seed", action="store_true", help="lock: lock the tree's own files")
    parser.add_argument("--from-run", type=int, action="append", metavar="N",
                        help="lock: add the rows CI run N made; bake: reuse those made on a host like this; repeatable")
    parser.add_argument("--kind", choices=KINDS, help="list, bake: only this kind")
    parser.add_argument("--blender", help="bake: the Blender to export with; `blender` on PATH when omitted")
    parser.add_argument("--missing", action="store_true", help="list: only the bakes LOCK has no row for")
    parser.add_argument("--out", help="bake: also copy what was made here, for a CI run to upload")
    parser.add_argument("--only", action="append", metavar="OUTPUT", help="bake: only this output; repeatable")
    parser.add_argument("--again", action="store_true", help="bake: make them even when locked, and compare")
    parser.add_argument("--offline", action="store_true", help="fetch: never download")
    args = parser.parse_intermixed_args(argv)  # paths may follow options, as a shell user types them
    from r3d.build_pack import DEFAULT_SEARCH

    cache = pathlib.Path(args.cache) if args.cache else default_cache()
    try:
        if args.command == "tool":
            for stage in args.paths:
                if stage not in STAGES:
                    parser.error(f"tool: no stage {stage!r}; stages: {', '.join(STAGES)}")
                print(tool_digest(stage))
            return 0
        paths = args.paths or [DEFAULT_SEARCH]
        tables = read_lock_tables()
        lock, held = tables["bake"], tables["poses"]
        made = runs_made(args.from_run, cache) if args.command == "lock" and args.from_run else {}
        starts = locked_starts(lock, made)
        poses = Poses(held, resample=args.command == "lock")
        found = [] if args.command == "path" else bakes(paths, starts, poses)
        if args.command == "list":
            for bake in found:
                if (args.missing and bake.key in lock) or args.kind not in (None, bake.kind):
                    continue
                print(f"{bake.output}\t{bake.kind}\t{bake.key or 'waits for its start'}\t{relative(bake.source)}")
        elif args.command == "bake":
            from bake import produce

            unknown = sorted(set(args.only or ()) - {bake.output for bake in found})
            if unknown:
                parser.error(f"--only: no bake makes {', '.join(unknown)}")
            # A named output of another kind would make a run that bakes nothing and still passes.
            other = sorted(bake.output for bake in found
                           if bake.output in (args.only or ()) and args.kind not in (None, bake.kind))
            if other:
                parser.error(f"--only: {', '.join(other)} not of --kind {args.kind}")

            def to_make(bake, kind):
                return (bake.kind == kind and args.kind in (None, kind) and (args.again or bake.key not in lock)
                        and not (args.only and bake.output not in args.only))

            # What is made for a run to upload must be lockable: a mesh only on the lock's CPU vendor.
            mesh_cpu_vendor, cpu = tables.get("mesh_cpu_vendor"), cpu_vendor()
            if args.out:
                print(f"this host's CPU vendor: {cpu or 'not named'}; "
                      f"the lock's meshes: {mesh_cpu_vendor or 'not named'}")
                if mesh_cpu_vendor and cpu != mesh_cpu_vendor and any(to_make(bake, "mesh") for bake in found):
                    raise BakeMissing(f"this host's CPU is {cpu or 'not named'}, but {rerun_on(mesh_cpu_vendor)}")
            for run in args.from_run or ():
                # Each upload apart: two attempts of one run may have made one key on different hosts.
                with tempfile.TemporaryDirectory() as folder:
                    reused = [row for upload in sorted(run_files(run, pathlib.Path(folder)).iterdir(),
                                                         key=lambda upload: (produce.attempt(upload), upload))
                              for row in produce.adopt(produce.import_run(upload, run, cache).values(), cache)]
                print(f"run {run}: {len(reused)} bakes made on a host like this one are taken as made here")

            def keep(row):
                if args.out:
                    produce.export(row, cache, pathlib.Path(args.out))
                return row

            rows, made_starts = [], {}
            for kind in KINDS:
                if args.kind not in (None, kind):
                    continue
                if kind == "fit" and made_starts:
                    # A fit's key names its start's bytes: the lock's, else those this run just made.
                    found = bakes(paths, {**made_starts, **starts}, poses)
                selected = [bake for bake in found if to_make(bake, kind)]
                if kind == "mesh":
                    made_rows = _mesh_rows(selected, cache, lock, args.blender, args.again, keep)
                    made_starts.update({bake.key: row["sha256"] for bake, row in zip(selected, made_rows) if not bake.packed})
                    rows.extend(made_rows)
                else:
                    for bake in selected:
                        if bake.key is None:
                            raise BakeMissing(describe(bake, unlocked(bake), "bake its start first: bake.py bake --kind mesh"))
                        rows.append(keep((None if args.again else produce.made(bake.key, cache)) or produce.produce(
                            bake, cache, lock, args.blender)))
            for row in rows:
                locked = lock.get(row["key"])
                versus = "" if locked is None else (
                    "; the same bytes as the lock" if locked["sha256"] == row["sha256"]
                    else f"; differs from the lock's {locked['sha256']}, which keeps its row")
                print(f"made {row['output']}  key {row['key']}  sha256 {row['sha256']}{versus}")
        elif args.command == "lock":
            mesh_cpu_vendor = runs_mesh_cpu_vendor(found, lock, made, tables.get("mesh_cpu_vendor"))
            if args.seed:
                rows = seed(found, cache, lock, made)
            else:
                # A fit waits for its start's row: what is made locks now, and the rest is named.
                missing = []
                rows = lock_rows(found, lock, made, missing)
            write_lock(rows, path=LOCK, poses=poses.used.values(), mesh_cpu_vendor=mesh_cpu_vendor)
            print(f"locked {len(rows)} bakes in {relative(LOCK)}")
            if not args.seed and missing:
                raise BakeMissing("still to lock:\n" + "\n".join(missing))
        elif args.command == "check":
            problems = check(found, lock) + check_poses(found, held)
            if problems:
                run = os.environ.get("GITHUB_RUN_ID")
                after = (f"\nThis run made the mesh bakes: bake.py lock --from-run {run}, then commit {relative(LOCK)}."
                         if run else "")
                raise BakeMissing(f"{relative(LOCK)} is out of date:\n" + "\n".join(problems) + after)
            seeded = sorted(row["output"] for row in lock.values() if row.get("seeded"))
            if seeded:
                print(f"{len(seeded)} of {len(lock)} rows are seeded: bytes carried over (from an older key or the tree), not made by "
                      "the code their keys name; a CI run that makes a key again clears it: " + ", ".join(seeded))
        elif args.command == "path":
            if len(args.paths) != 1:
                parser.error("path takes one output name")
            named = [bake for bake in bakes([DEFAULT_SEARCH], starts, poses) if bake.output == args.paths[0]]
            if not named:
                parser.error(f"path: no bake makes {args.paths[0]}")
            print(fetch_all(named, lock, cache, args.offline)[named[0]])
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
