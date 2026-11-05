"""packages: the first-party packages under launcher/packages/, for Python that
resolves, compiles or checks firmware code; packages.sh is the shell twin. Each
package's include root is <package>/include, found by globbing, so adding a
package changes no caller."""
import pathlib
import re

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


def board_include_dir(launcher=LAUNCHER):
    """main/board/<vendor>/<board> of the board a host build compiles: the BOARD_DIR of the Kconfig choice
    BOARD's default, as main/board/board_dir.cmake reads it. Raises when the choice names none
    that exists."""
    kconfig = (pathlib.Path(launcher) / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
    choice = re.search(r"^ *choice BOARD\n(.*?)^ *endchoice", kconfig, re.S | re.M)
    default = choice and re.search(r"^ *default ([A-Z0-9_]+)$", choice.group(1), re.M)
    folder = default and re.search(rf'^ *default "([a-z0-9_/]+)" if {default.group(1)}$', kconfig, re.M)
    path = folder and pathlib.Path(launcher) / "main" / "board" / folder.group(1)
    if not path or not path.is_dir():
        raise ValueError("no board folder: main/Kconfig.projbuild's choice BOARD names none that exists")
    return path


def include_roots(launcher=LAUNCHER):
    """launcher/main, the board's folder and every package's include root: the firmware's
    quoted-include search path."""
    return (pathlib.Path(launcher) / "main", board_include_dir(launcher), *include_dirs(launcher))
