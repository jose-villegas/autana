#!/usr/bin/env python3
"""Each pinned framework version beside the one this machine has.

    python scripts/deps/versions.py           # every row
    python scripts/deps/versions.py --drift   # only the rows that differ; exit 1 if any

Informational: nothing here installs or changes a tool. docs/tools/Framework-Versions.md
says how to match each row.
"""
import argparse
import contextlib
import io
import importlib.metadata
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools/build"))
sys.path.insert(0, str(ROOT / "scripts/lib"))
sys.path.insert(0, str(ROOT / "launcher/tools"))
from bake.produce import PIN  # noqa: E402  (the exact-pin line a bake key counts)
from espressif import idf_commit, idf_path, idf_pin  # noqa: E402
import pinned_tool  # noqa: E402

MISSING = "not found"


def output(*command, cwd=ROOT):
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def pinned_requirements(root=ROOT):
    """(file, package, version) of every exact pin in a tracked requirements*.txt."""
    files = output("git", "ls-files", "--", ":(glob)**/requirements*.txt", cwd=root) or ""
    for name in files.splitlines():
        for line in (root / name).read_text(encoding="utf-8").splitlines():
            if match := PIN.match(line.strip()):
                yield name, match.group(1), match.group(2)


def installed(package):
    try:
        return importlib.metadata.version(package)
    except importlib.metadata.PackageNotFoundError:
        return MISSING


def resolved_version(tool):
    """The version of the binary the gates would run, or why there is none."""
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            binary, _ = pinned_tool.resolve(tool)
    except SystemExit:
        return MISSING
    return pinned_tool.tool_version(binary)


def rows(root=ROOT):
    """(framework, pinned, here) for each pin."""
    pin = idf_pin()
    yield "ESP-IDF commit", pin["commit"], idf_commit(idf_path()) or MISSING
    for tool in ("clang-format", "clang-tidy"):
        yield tool, pin["esp_clang"], resolved_version(tool)
    yield "Python", (root / ".python-version").read_text().strip(), ".".join(map(str, sys.version_info[:3]))
    node = output("node", "--version")
    yield "Node", (root / ".node-version").read_text().strip(), node.lstrip("v") if node else MISSING
    for name, package, version in pinned_requirements(root):
        yield f"{package} ({name})", version, installed(package)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--drift", action="store_true", help="only rows that differ; exit 1 if any")
    args = parser.parse_args(argv)
    table = [(name, pinned, here) for name, pinned, here in rows() if not args.drift or pinned != here]
    width = max((len(name) for name, _, _ in table), default=0)
    for name, pinned, here in table:
        mark = "" if pinned == here else "  <- differs"
        print(f"{name:<{width}}  pinned {pinned:<12}  here {here}{mark}")
    return 1 if args.drift and table else 0


if __name__ == "__main__":
    sys.exit(main())
