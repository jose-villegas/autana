"""packages: the first-party packages under launcher/packages/, for Python that
resolves, compiles or checks firmware code; packages.sh is the shell twin. Each
package's include root is <package>/include, found by globbing, so adding a
package changes no caller."""
import pathlib

LAUNCHER = pathlib.Path(__file__).resolve().parents[2]
# The firmware's own C, as repository paths: the main component and the packages.
FIRST_PARTY = ("launcher/main/", "launcher/packages/")


def package_part(rel):
    """(name, part) for a repository path inside launcher/packages/<name>/<part>/, part
    being include, src, tests or another top folder of the package; None elsewhere."""
    parts = pathlib.PurePosixPath(rel).parts
    return (parts[2], parts[3]) if parts[:2] == ("launcher", "packages") and len(parts) > 4 else None


def include_dirs(launcher=LAUNCHER):
    """Every package's include root, sorted."""
    return sorted(path for path in (pathlib.Path(launcher) / "packages").glob("*/include") if path.is_dir())


def include_roots(launcher=LAUNCHER):
    """launcher/main and every package's include root: the firmware's quoted-include search path."""
    return (pathlib.Path(launcher) / "main", *include_dirs(launcher))
