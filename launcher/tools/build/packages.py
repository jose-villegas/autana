"""packages: the first-party packages under launcher/packages/, for Python that
resolves, compiles or checks firmware code; packages.sh is the shell twin. Each
package's include root is <package>/include, found by globbing, so adding a
package changes no caller.

A package that uses another names it in its own CMakeLists.txt, on one line
that both builds read:

    set(physics_requires math)

idf_component_register(... REQUIRES ${physics_requires}) and
target_link_libraries(physics PUBLIC ${physics_requires}) take it from there."""
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


def include_roots(launcher=LAUNCHER):
    """launcher/main and every package's include root: the firmware's quoted-include search path."""
    return (pathlib.Path(launcher) / "main", *include_dirs(launcher))


def requires_line(name):
    """The declaration's one grammar: unindented, on one line, bare package names."""
    return re.compile(r"^set\(" + re.escape(name) + r"_requires((?:[ \t]+\w+)*)[ \t]*\)[ \t]*$", re.M)


def requires(package_dir):
    """The packages `package_dir` declares it uses, in its CMakeLists.txt order."""
    package_dir = pathlib.Path(package_dir)
    lists = package_dir / "CMakeLists.txt"
    if not lists.is_file():
        return ()
    m = requires_line(package_dir.name).search(lists.read_text(encoding="utf-8"))
    return tuple(m.group(1).split()) if m else ()


def reachable(package_dir):
    """The package folders `package_dir` may include: itself and every package it declares,
    and theirs in turn. A name with no package folder is skipped; PACKAGE-REQUIRES reports it."""
    package_dir = pathlib.Path(package_dir)
    seen = [package_dir]
    for current in seen:
        for name in requires(current):
            dep = package_dir.parent / name
            if dep.is_dir() and dep not in seen:
                seen.append(dep)
    return tuple(seen)
