"""packages: the first-party packages under launcher/packages/ (math/ first), for
Python that resolves or compiles firmware includes; packages.sh is the shell twin.
Each package's include root is <package>/include, found by globbing, so adding a
package changes no caller."""
import pathlib

LAUNCHER = pathlib.Path(__file__).resolve().parents[2]


def include_dirs(launcher=LAUNCHER):
    """Every package's include root, sorted."""
    return sorted(path for path in (pathlib.Path(launcher) / "packages").glob("*/include") if path.is_dir())


def include_roots(launcher=LAUNCHER):
    """launcher/main and every package's include root: the firmware's quoted-include search path."""
    return (pathlib.Path(launcher) / "main", *include_dirs(launcher))
