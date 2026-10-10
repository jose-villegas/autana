"""The files git tracks under a root; what CI sees, not whatever a build or a
checkout nested inside this one has left beside them; and the contents of named
files at a git revision."""
import io
import fnmatch
import pathlib
import subprocess

_LISTINGS = {}

# What a walk outside git skips: the output of a build or a package manager.
SKIP = {"build", "build.dev", "build.diag", "build.qemu", "build.qemu.perf", "build.qemu.shell",
        "managed_components", "node_modules", ".git"}


def git_listing(cwd, args):
    """The lines `git ls-files <args>` prints in `cwd`, or None when `cwd` is
    no git repository (a test fixture), for the caller to walk instead. Any
    other git failure raises with git's message: a refused checkout falling
    back to a walk would silently change what a gate reads."""
    result = subprocess.run(["git", "ls-files", *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode == 0:
        return [line for line in result.stdout.splitlines() if line]
    if "not a git repository" in result.stderr:
        return None
    raise RuntimeError(f"git ls-files in {cwd} failed: {result.stderr.strip()}")


def tracked_files(root, patterns=()):
    """A tuple of paths relative to `root`, as posix strings, matching the
    `git ls-files` pathspecs in `patterns` (every file when empty).

    A git listing is taken once per root and pattern set and then reused: a
    gate reads one snapshot of the tree, and re-listing per lookup made the
    citations gate four times slower. Outside git (a test fixture), every
    file under `root` instead, never cached, since a fixture changes between
    calls."""
    root = pathlib.Path(root)
    key = (str(root.resolve()), tuple(patterns))
    if key in _LISTINGS:
        return _LISTINGS[key]
    listing = git_listing(root, patterns)
    if listing is None:
        names = sorted(path.relative_to(root).as_posix()
                       for path in root.rglob("*") if path.is_file())
        return tuple(name for name in names
                     if not patterns or any(fnmatch.fnmatch(name, p) or
                                            name.startswith(p.rstrip("/") + "/") for p in patterns))
    _LISTINGS[key] = tuple(listing)
    return _LISTINGS[key]


def committable(base):
    """Every file under `base` git would commit (tracked, or new and not
    ignored), as sorted paths: a build's output or a local venv never
    counts, a file not yet added does. Outside git (a test fixture), every
    file not under a SKIP directory."""
    base = pathlib.Path(base)
    listing = git_listing(base, ["--cached", "--others", "--exclude-standard"])
    if listing is not None:
        return sorted(path for path in (base / line for line in listing) if path.is_file())
    return sorted(path for path in base.rglob("*")
                  if path.is_file() and not any(part in SKIP for part in path.relative_to(base).parts))


def revision_contents(root, revision, names, renames=None):
    renames = renames or {}
    requests = "".join(f"{revision}:{renames.get(name, name)}\n" for name in names).encode("utf-8")
    result = subprocess.run(["git", "cat-file", "--batch"], cwd=root, input=requests,
                            check=True, capture_output=True)
    stream = io.BytesIO(result.stdout)
    for name in names:
        header = stream.readline().rstrip(b"\n")
        if header.endswith(b" missing"):
            continue
        _, kind, size = header.split()
        if kind != b"blob":
            raise ValueError(f"Not a source blob: {revision}:{name}")
        content = stream.read(int(size))
        if len(content) != int(size) or stream.read(1) != b"\n":
            raise ValueError(f"Incomplete source blob: {revision}:{name}")
        yield name, content
