#!/usr/bin/env python3
"""Fail when a file outside scripts/device/ opens the board's serial port,
or a document names a script retired in favour of the `autana` CLI.

    python scripts/gates/check_device_access.py

Every command that touches the board goes through `scripts/device/device.py`,
which takes the device lock before it opens the port - see
docs/tools/Autana-CLI.md and docs/tools/Device-Lock.md. A file elsewhere that
opens pyserial's `Serial(`, invokes `idf_monitor`/`idf.py monitor`, or shells
out to `esptool` bypasses that lock, so two sessions can fight over one port.
`ALLOWLIST` is the explicit, small list of exceptions (QEMU is not the board).

A retired script named in a document is a dead pointer - see
`RETIRED_SCRIPTS` for the list and `docs/tools/Autana-CLI.md` for what
replaced each one.
"""
import pathlib
import re
import subprocess
import sys

ALLOWLIST = "scripts/gates/device_access_allowlist.txt"

# Basenames only: a doc may spell one with or without its folder
# (`screenshot.sh`, `launcher/tools/screenshot.sh`), and both name the same
# dead file.
RETIRED_SCRIPTS = (
    "screenshot.sh",
    "monitor.sh",
    "run_device_tests.sh",
    "collect_device_results.py",
    "capture_selftest.py",
    "capture_runsuite.py",
)

SERIAL_OPEN_RE = re.compile(r"\bserial\.Serial\s*\(|(?<![.\w])Serial\s*\(")
SERIAL_IMPORT_RE = re.compile(r"^\s*(import serial\b|from serial\b)", re.MULTILINE)
# Naming idf_monitor in prose ("the same problem idf_monitor has") is
# common and not a violation; only an actual invocation shape is - the
# script file, its own entry point, or an `idf.py ... monitor` command line.
IDF_MONITOR_RE = re.compile(r"\bidf_monitor\.py\b|\bidf_monitor_main\b|\bidf(\.py)?\b[^\n#]*\bmonitor\b")
ESPTOOL_PY_RE = re.compile(r'"-m",\s*"esptool"|\besptool_main\b|^\s*import esptool\b', re.MULTILINE)
ESPTOOL_SH_RE = re.compile(r"(?<![\w./-])esptool(\.py)?\b")

# A line that only prints text (an echo/printf, or a Python string literal
# assigned to nothing executable) is not an invocation - build_flash.sh's own
# "letting esptool pick the port" status line is exactly this shape.
PRINT_LINE_RE = re.compile(r'^\s*(echo|printf)\b')


class Violation:
    def __init__(self, path, line, reason):
        self.path = path
        self.line = line
        self.reason = reason


def tracked_files(root, patterns):
    result = subprocess.run(["git", "ls-files", *patterns], cwd=root,
                            capture_output=True, text=True, check=True)
    return [line for line in result.stdout.splitlines() if line]


def is_comment_or_print(path, line):
    stripped = line.strip()
    if not stripped:
        return True
    if path.endswith(".py") and stripped.startswith("#"):
        return True
    if path.endswith(".sh") and stripped.startswith("#"):
        return True
    return bool(PRINT_LINE_RE.match(line))


def serial_port_openers(root):
    """Every (path, line, reason) where a file outside scripts/device/ opens
    the board's serial port - pyserial, idf_monitor, or esptool."""
    root = pathlib.Path(root)
    found = []
    for path in tracked_files(root, ["*.py"]):
        if path.startswith("scripts/device/") or not (root / path).is_file():
            continue
        text = (root / path).read_text(encoding="utf-8", errors="replace")
        has_serial_import = bool(SERIAL_IMPORT_RE.search(text))
        for number, line in enumerate(text.splitlines(), 1):
            if is_comment_or_print(path, line):
                continue
            if has_serial_import and SERIAL_OPEN_RE.search(line):
                found.append(Violation(path, number, "opens a pyserial Serial()"))
            elif IDF_MONITOR_RE.search(line):
                found.append(Violation(path, number, "invokes idf_monitor"))
            elif ESPTOOL_PY_RE.search(line):
                found.append(Violation(path, number, "invokes esptool"))
    for path in tracked_files(root, ["*.sh"]):
        if path.startswith("scripts/device/") or not (root / path).is_file():
            continue
        text = (root / path).read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            if is_comment_or_print(path, line):
                continue
            if IDF_MONITOR_RE.search(line):
                found.append(Violation(path, number, "invokes idf_monitor"))
            elif ESPTOOL_SH_RE.search(line):
                found.append(Violation(path, number, "invokes esptool"))
    return found


def allowlist(root):
    """{path: reason}."""
    path = pathlib.Path(root) / ALLOWLIST
    allowed = {}
    if not path.exists():
        return allowed
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not all(fields):
            raise ValueError(f"{path}:{number}: expected path, reason")
        allowed[fields[0]] = fields[1]
    return allowed


RETIRED_WORD_RE = re.compile(
    r"(?<![A-Za-z0-9_.])(" + "|".join(re.escape(name) for name in RETIRED_SCRIPTS) + r")(?![A-Za-z0-9_])")


def retired_script_mentions(root):
    """Every (doc, line, name) where a tracked document names a retired
    script - the exact list device_report.sh and autana now replace."""
    root = pathlib.Path(root)
    found = []
    for path in tracked_files(root, ["*.md"]):
        text = (root / path).read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            for match in RETIRED_WORD_RE.finditer(line):
                found.append(Violation(path, number, f"names retired script {match.group(1)}"))
    return found


def check(root="."):
    allowed = allowlist(root)
    openers = [v for v in serial_port_openers(root) if v.path not in allowed]
    retired = retired_script_mentions(root)
    return openers, retired


def main(argv):
    if argv:
        print("usage: check_device_access.py", file=sys.stderr)
        return 2
    try:
        openers, retired = check(".")
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    for v in openers:
        print(f"{v.path}:{v.line}: {v.reason} outside scripts/device/")
    for v in retired:
        print(f"{v.path}:{v.line}: {v.reason}")
    total = len(openers) + len(retired)
    print(f"{len(openers)} serial port opener(s) outside scripts/device/, "
          f"{len(retired)} retired-script mention(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
