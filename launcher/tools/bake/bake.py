#!/usr/bin/env python3
"""Cached bakes: the meshes a pack needs, named by a digest of what makes them.

    python launcher/tools/bake/bake.py list [PATH ...]
    python launcher/tools/bake/bake.py lock --seed [PATH ...] [--cache DIR]
    python launcher/tools/bake/bake.py check [PATH ...]
    python launcher/tools/bake/bake.py fetch [PATH ...] [--cache DIR] [--offline]
    python launcher/tools/bake/bake.py publish

PATH is what build_pack.py takes; with none, launcher/main is searched. A
bake's key is a SHA-256 of its kind, its recipe (the parsed import and
renderer settings and the scene inputs its look reads), its sources (an LFS
pointer's oid stands for the file, so no source is downloaded) and the code
of the tool that makes it. LOCK, written only by this tool, records for
each key the SHA-256 of the bytes made for it, because a bake is not
byte-reproducible across machines and a fit not even on one.

`list` prints every bake with its key. `lock --seed` locks the meshes
in the tree as they are and copies them into the cache. `check` fails when
LOCK lacks a needed key or holds one nothing needs. `fetch` puts every
locked file in the cache, from the release when it is not there; `--offline`
never downloads. `publish`, on main in CI only, uploads each locked file the
release lacks. The cache is %LOCALAPPDATA%/autana/bakes, else
$XDG_CACHE_HOME/autana/bakes, else ~/.cache/autana/bakes, shared by every
clone. Standard library only; Python 3.12 or later.
"""

import argparse
import ast
import dataclasses
import hashlib
import json
import os
import pathlib
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

from r3d.import_settings import SettingsError, source_digest  # noqa: E402

REPO = TOOLS.parents[1]
LOCK = REPO / "launcher" / "bakes.lock"
RELEASE_TAG = "bakes"
RELEASE_URL = f"https://github.com/jose-villegas/autana/releases/download/{RELEASE_TAG}/"
MESH_SUFFIX = ".mesh"
DOWNLOAD_TIMEOUT_S = 60
# Each stage's entry scripts. Its code is their import closure under TOOLS, which stops at another
# stage's entry script (that stage's key already chains in) and at this tool, plus the requirements
# it installs and the native library it builds.
STAGES = {
    "mesh": ("r3d/mesh_import.py",),
    "reference": ("r3d/mesh_import.py", "r3d/reference_render.py", "r3d/fitted_variant.py"),
    "fit": ("r3d/fitted_variant.py", "r3d/appearance_simplify.py"),
}
REQUIREMENTS = ("r3d/requirements.txt",)
SUBMODULES = ("third_party/upstream/meshoptimizer",)
# Token kinds that carry no code: a comment or blank-line edit does not rebake.
SKIPPED_TOKENS = {"COMMENT", "NL", "ENCODING"}
SHAPE_TOKENS = {"NEWLINE", "INDENT", "DEDENT"}


class BakeMissing(Exception):
    """One or more bakes are not available; the message names each."""


@dataclasses.dataclass(frozen=True)
class Bake:
    output: str            # the file it makes: "<id>.mesh"
    source: pathlib.Path   # the file that asks for it
    holder: str            # the scene object, or the import variant, it is made for
    kind: str              # "mesh" | "fit"
    key: str               # SHA-256 hex of everything that determines it
    suffix: str
    tree: pathlib.Path     # where the tree keeps it today


def default_cache():
    for name in ("LOCALAPPDATA", "XDG_CACHE_HOME"):
        if os.environ.get(name):
            return pathlib.Path(os.environ[name]) / "autana" / "bakes"
    return pathlib.Path.home() / ".cache" / "autana" / "bakes"


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def module_file(name, package=None, level=0):
    """The file under TOOLS that `import name` (relative to `package` at `level`) loads, or None outside TOOLS."""
    parts = name.split(".") if name else []
    if level:
        parts = package.split(".")[:len(package.split(".")) - level + 1] + parts
    base = TOOLS.joinpath(*parts)
    for path in (base.with_suffix(".py"), base / "__init__.py"):
        if parts and path.is_file():
            return path
    return None


def closure(entries, stop=()):
    """Every file under TOOLS the entry scripts import, at any depth, and the packages holding them;
    a file in `stop` is neither counted nor followed."""
    found, pending = set(), [TOOLS / entry for entry in entries]
    while pending:
        path = pending.pop()
        if path in found or path in stop:
            continue
        found.add(path)
        package = ".".join(path.relative_to(TOOLS).with_suffix("").parts[:-1])
        for init in (TOOLS.joinpath(*package.split(".")[:depth + 1], "__init__.py")
                     for depth in range(len(package.split("."))) if package):
            if init.is_file():
                pending.append(init)
        for node in ast.walk(ast.parse(path.read_bytes(), str(path))):
            names = []
            if isinstance(node, ast.Import):
                names = [(alias.name, 0) for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                names = [(node.module or "", node.level)]
                names += [((node.module + "." if node.module else "") + alias.name, node.level) for alias in node.names]
            for name, level in names:
                target = module_file(name, package, level)
                if target is not None:
                    pending.append(target)
    return sorted(found)


def code_tokens(path):
    """A file's tokens without comments, blank lines or layout: the same on every Python from 3.12."""
    with path.open("rb") as source:
        return [[tokenize.tok_name[token.type], "" if tokenize.tok_name[token.type] in SHAPE_TOKENS else token.string]
                for token in tokenize.tokenize(source.readline)
                if tokenize.tok_name[token.type] not in SKIPPED_TOKENS]


def submodule_commit(path):
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-s", "--", path], check=True,
                         capture_output=True, text=True).stdout.split()
    if not out:
        raise SettingsError(f"{path} is not a submodule of this checkout")
    return out[1]


def stage_files(stage):
    """The files whose code makes `stage`'s output."""
    own = {TOOLS / entry for entry in STAGES[stage]}
    stop = {TOOLS / entry for entries in STAGES.values() for entry in entries} - own
    stop |= set((TOOLS / "bake").glob("*.py"))
    return closure(STAGES[stage], stop)


def native_sources(files):
    """The C a stage's files compile and run: the pose sampler track_host builds, by its own SOURCES."""
    if TOOLS / "anim" / "track_host.py" not in files:
        return {}
    from anim.track_host import SOURCES

    return {relative(path): file_sha256(path) for path in SOURCES}


def tool_digest(stage):
    """The code of a stage: its files' tokens, the C they compile, its requirements and the submodule
    it builds."""
    paths = stage_files(stage)
    files = {path.relative_to(TOOLS).as_posix(): code_tokens(path) for path in paths}
    files.update(native_sources(paths))
    requirements = {name: (TOOLS / name).read_text(encoding="utf-8").split() for name in REQUIREMENTS}
    return digest([files, requirements, {name: submodule_commit(name) for name in SUBMODULES}])


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


def mesh_recipe(job, scene):
    """What a lit or albedo mesh is made from, without its fit and without paths; its sources count by
    content, so a credit line or a moved file does not rebake."""
    settings = {name: canonical(value) for name, value in vars(job.settings).items()
                if name not in ("path", "source", "out_dir", "mesh_dir", "named", "variants")}
    renderer = {name: canonical(value) for name, value in vars(job.renderer).items() if name not in ("settings", "fit")}
    recipe = {"settings": settings, "renderer": renderer, "sources": source_digest(job.settings)}
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
    start = digest(["mesh", mesh_recipe(job, scene), tools["mesh"]])
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
    found = []
    for name, entries in sorted(packs.items()):
        for entry, source in sorted(entries.items()):
            if source.resolve() not in jobs:
                continue
            job, scene, asker = jobs[source.resolve()]
            keys = stage_keys(job, scene, tools)
            kind = "fit" if "fit" in keys else "mesh"
            holder = job.object.name if job.object is not None else job.renderer.variant.name
            found.append(Bake(output=f"{entry}{MESH_SUFFIX}", source=asker, holder=holder, kind=kind, key=keys[kind],
                              suffix=MESH_SUFFIX, tree=source))
    return found


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
    return json.dumps(value) if isinstance(value, str) else str(value)


def write_lock(rows, path=LOCK):
    """LOCK from `rows`, one table per key, sorted by output then key."""
    lines = ["# Written by launcher/tools/bake/bake.py: the bytes made for each bake's key. Do not edit.", ""]
    for row in sorted(rows, key=lambda row: (row["output"], row["key"])):
        lines.append("[[bake]]")
        lines += [f"{name} = {toml_value(row[name])}" for name in ("output", "source", "key", "sha256", "size", "run")
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
    if bake.kind == "fit":
        return "bake it: fitted_variant.py prepare and fit on a CUDA GPU, then bake.py lock"
    return f"bake it: python launcher/tools/r3d/mesh_import.py {relative(bake.source)}, then bake.py lock"


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


def seed(found, cache):
    """Rows locking the tree's own files, each copied into the cache."""
    rows, missing = [], []
    for bake in found:
        if not bake.tree.is_file():
            missing.append(describe(bake, f"{relative(bake.tree)} is not in the tree to seed from", bake_again(bake)))
            continue
        data = bake.tree.read_bytes()
        row = {"output": bake.output, "source": relative(bake.source), "key": bake.key,
               "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
        store(bake.tree, row, bake.suffix, cache)
        rows.append(row)
    if missing:
        raise BakeMissing("cannot seed:\n" + "\n".join(missing))
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
        if not (path.is_file() and file_sha256(path) == row["sha256"]):
            if not (bake.tree.is_file() and file_sha256(bake.tree) == row["sha256"]):
                raise BakeMissing(describe(bake, f"locked as {row['sha256']}, but no file with those bytes is here",
                                           "run bake.py lock where it was made"))
            path = store(bake.tree, row, bake.suffix, cache)
        subprocess.run(["gh", "release", "upload", RELEASE_TAG, str(path)], check=True)
        assets.add(name)
        print(f"published {name} ({bake.output})")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("list", "lock", "check", "fetch", "publish"))
    parser.add_argument("paths", nargs="*", help="what build_pack.py takes; launcher/main when omitted")
    parser.add_argument("--cache", help="the cache directory; the user cache when omitted")
    parser.add_argument("--seed", action="store_true", help="lock: lock the tree's own files")
    parser.add_argument("--offline", action="store_true", help="fetch: never download")
    args = parser.parse_args(argv)
    from r3d.build_pack import DEFAULT_SEARCH

    cache = pathlib.Path(args.cache) if args.cache else default_cache()
    try:
        found = bakes(args.paths or [DEFAULT_SEARCH])
        lock = read_lock()
        if args.command == "list":
            for bake in found:
                print(f"{bake.output}\t{bake.kind}\t{bake.key}\t{relative(bake.source)}")
        elif args.command == "lock":
            if not args.seed:
                parser.error("lock needs --seed: the files that produce bakes come later")
            write_lock(seed(found, cache))
            print(f"locked {len(found)} bakes in {relative(LOCK)}")
        elif args.command == "check":
            problems = check(found, lock)
            if problems:
                raise BakeMissing(f"{relative(LOCK)} is out of date:\n" + "\n".join(problems))
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
