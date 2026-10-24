"""package_requires: which packages a launcher/packages/<name>/ package uses,
for the gates that check its includes; packages.sh's package_requires and
package_reachable are the shell twins.

A package names them in its own CMakeLists.txt, on one line both builds read:

    set(physics_requires math)

idf_component_register(... REQUIRES ${physics_requires}) and
target_link_libraries(physics PUBLIC ${physics_requires}) take it from there.
Kept apart from packages.py, which the bake's tools import: a bake key hashes
its tools' code, and these rules are not part of any bake."""
import pathlib
import re


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
