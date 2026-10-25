"""Fill each launcher/packages/<name>/README.md's generated header index, one
row per header under its include/ with its banner sentence.

    python launcher/tools/gen/package_headers.py [--check]
"""
import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/gates"))
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
sys.path.insert(0, str(ROOT / "launcher/tools"))

from build.packages import package_part
from check_file_headers import file_purpose
from generated_blocks import replace_block
from tracked import tracked_files

CHECK = "python launcher/tools/gen/package_headers.py --check"


def package_headers(root):
    """{README: [(header, purpose)]} for each launcher/packages/<name>/: a row per header under
    its include/, spelled from the package folder."""
    root = pathlib.Path(root).resolve()
    apis = {}
    for path in tracked_files(root):
        part = package_part(path)
        if part is None or part[1] != "include" or not path.endswith(".h"):
            continue
        package = f"launcher/packages/{part[0]}/"
        purpose = file_purpose(path, (root / path).read_text(encoding="utf-8"))
        apis.setdefault(package + "README.md", []).append((path.removeprefix(package), purpose))
    return {readme: sorted(rows) for readme, rows in apis.items()}


def table(rows):
    lines = ["| Header | Purpose |", "|---|---|"]
    for path, purpose in rows:
        purpose = purpose.replace("|", "&#124;").replace("<", "&lt;").replace(">", "&gt;")
        lines.append(f"| [{path}]({path}) | {purpose} |")
    return "\n".join(lines)


def update(root, check=False):
    root = pathlib.Path(root)
    stale = []
    for readme, headers in package_headers(root).items():
        package = pathlib.PurePosixPath(readme).parent.name.replace("_", "-")
        if replace_block(root / readme, f"package-{package}-headers", table(headers), check, CHECK):
            stale.append(readme)
    state = (f"stale: run python3 launcher/tools/gen/package_headers.py and git add {' '.join(stale)}"
             if stale and check else "current")
    print(f"package headers: {state}")
    return int(check and bool(stale))


def main():
    """The README command, rooted in this checkout unless a fixture is supplied."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        return update(args.root, args.check)
    except (ValueError, OSError, SyntaxError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
