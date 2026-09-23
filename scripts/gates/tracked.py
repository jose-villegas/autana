"""The files git tracks under a root - what CI sees, not whatever a build or a
checkout nested inside this one has left beside them."""
import fnmatch
import pathlib
import subprocess

_LISTINGS = {}


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
    result = subprocess.run(["git", "ls-files", *patterns], cwd=root,
                            capture_output=True, text=True)
    if result.returncode:
        names = sorted(path.relative_to(root).as_posix()
                       for path in root.rglob("*") if path.is_file())
        return tuple(name for name in names
                     if not patterns or any(fnmatch.fnmatch(name, p) for p in patterns))
    _LISTINGS[key] = tuple(line for line in result.stdout.splitlines() if line)
    return _LISTINGS[key]
