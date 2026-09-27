"""The files git tracks under a root - what CI sees, not whatever a build or a
checkout nested inside this one has left beside them."""
import fnmatch
import pathlib
import subprocess

_LISTINGS = {}


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
    citations gate four times slower. Outside git - a test fixture - every
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
                     if not patterns or any(fnmatch.fnmatch(name, p) for p in patterns))
    _LISTINGS[key] = tuple(listing)
    return _LISTINGS[key]
