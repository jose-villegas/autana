"""package_requires: which packages a launcher/packages/<name>/ package uses,
for the gates that check its includes; packages.sh's package_requires and
package_reachable are the shell twins.

A package names them in its own CMakeLists.txt, on one line both builds read:

    set(physics_requires math)

idf_component_register(... REQUIRES ${physics_requires}) and
target_link_libraries(physics PUBLIC ${physics_requires}) take it from there.
Kept apart from packages.py, which the bake's tools import: a bake key hashes
its tools' code, and these rules are not part of any bake.

Also writes the dependency diagram in docs/firmware/Layers.md from the same declarations:

    python launcher/tools/build/package_requires.py [--check]
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from generated_blocks import replace_block
from packages import LAUNCHER, include_dirs

CHECK = "python launcher/tools/build/package_requires.py --check"
DOC = "docs/firmware/Layers.md"
BLOCK = "package-requires-diagram"


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


def diagram(launcher=LAUNCHER):
    """The Mermaid graph of every package, one arrow from a package to each package it declares.
    Node ids are prefixed so a package named like a Mermaid keyword still renders."""
    packages = [include.parent for include in include_dirs(launcher)]
    lines = ["```mermaid", "flowchart BT"]
    lines += [f'    p_{p.name}["{p.name}/"]' for p in packages]
    lines += [f"    p_{p.name} --> p_{name}" for p in packages for name in requires(p)]
    lines.append("```")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        stale = replace_block(args.root / DOC, BLOCK, diagram(args.root / "launcher"), args.check, CHECK)
    except (ValueError, OSError) as error:
        parser.exit(2, f"{error}\n")
    state = f"stale: run python3 {CHECK.split()[1]} and git add {DOC}" if stale and args.check else "current"
    print(f"package requires diagram: {state}")
    return int(args.check and stale)


if __name__ == "__main__":
    raise SystemExit(main())
