#!/usr/bin/env python3
"""Each pinned framework version beside the one this machine has.

    python scripts/deps/versions.py           # every row
    python scripts/deps/versions.py --drift   # only the rows that differ; exit 1 if any

Informational: nothing here installs or changes a tool. docs/tools/Framework-Versions.md
says how to match each row.
"""
import argparse
import importlib.metadata
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools/build"))
sys.path.insert(0, str(ROOT / "scripts/lib"))
from espressif import idf_path, idf_pin  # noqa: E402
import pinned_tool  # noqa: E402

REQUIREMENT = re.compile(r"^([A-Za-z0-9_.-]+)==(\S+)")
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
            if match := REQUIREMENT.match(line):
                yield name, match.group(1), match.group(2)


def installed(package):
    try:
        return importlib.metadata.version(package)
    except importlib.metadata.PackageNotFoundError:
        return MISSING


def rows(root=ROOT):
    """(framework, pinned, here) for each pin."""
    pin = idf_pin()
    yield "ESP-IDF commit", pin["commit"], output("git", "-C", str(idf_path()), "rev-parse", "HEAD") or MISSING
    for tool in ("clang-format", "clang-tidy"):
        found = [pinned_tool.tool_version(c) for c in pinned_tool.candidates(tool)]
        here = pin["esp_clang"] if pin["esp_clang"] in found else next((v for v in found if v), MISSING)
        yield tool, pin["esp_clang"], here
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
