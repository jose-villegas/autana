#!/usr/bin/env python3
"""Fail when a file outside scripts/device/ opens the board's serial port,
or a document names a board script this tree no longer has.

    python scripts/gates/check_device_access.py

Every command that touches the board goes through `scripts/device/device.py`,
which takes the device lock before it opens the port - see
docs/tools/Autana-CLI.md and docs/tools/Device-Lock.md. A file elsewhere that
opens pyserial's `Serial(`, invokes `idf_monitor`/`idf.py monitor`, runs
`idf.py ... flash`, or shells out to `esptool` bypasses that lock, so two
sessions can fight over one port. scripts/device/ and scripts/gates/ are
exempt by folder, and nothing else is.

The one esptool call that is not a bypass is one whose subcommand - the
first argument after `esptool` that is neither an option nor an option's
value - is `merge_bin` (or `merge-bin`): it writes an image file and opens no
port. Each esptool call on a line is judged by its own subcommand, a shell
comment is no part of the command, and a Python argument list is read across
the lines it spans.

A document naming one of `RETIRED_SCRIPTS` points at a file that does not
exist; the board is reached through `autana`, docs/tools/Autana-CLI.md.
"""
import pathlib
import re
import shlex
import sys

from tracked import tracked_files

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
ESPTOOL_MODULE_RE = re.compile(r'"-m",\s*"esptool"')
ESPTOOL_API_RE = re.compile(r"\besptool_main\b|^\s*import esptool\b")
ESPTOOL_SH_RE = re.compile(r"(?<![\w./-])esptool(\.py)?\b")
# Flashing opens the port exactly like monitoring does.
IDF_FLASH_RE = re.compile(r"\bidf(\.py)?\b[^\n#]*\bflash\b")

PRINT_LINE_RE = re.compile(r'^\s*(echo|printf)\b')

OFFLINE_SUBCOMMANDS = ("merge_bin", "merge-bin")
SHELL_SEPARATORS = ("&&", "||", ";", "|", "&")
# One element of a Python list: a string literal, or any other expression up
# to the next comma or closing bracket.
PY_ELEMENT_RE = re.compile(r"""\s*(?:"([^"\\\n]*)"|'([^'\\\n]*)'|([^,\]]*?))\s*([,\]]|$)""")


class Violation:
    def __init__(self, path, line, reason):
        self.path = path
        self.line = line
        self.reason = reason


def is_comment_or_print(path, line):
    stripped = line.strip()
    if not stripped:
        return True
    if path.endswith(".py") and stripped.startswith("#"):
        return True
    if path.endswith(".sh") and stripped.startswith("#"):
        return True
    return bool(PRINT_LINE_RE.match(line))


# scripts/device/ is the one place allowed to touch the port at all;
# scripts/gates/ is exempt too - a gate's own source and tests describe and
# exercise these exact patterns as data, never run them against hardware.
EXEMPT_PREFIXES = ("scripts/device/", "scripts/gates/")


def esptool_subcommand(arguments):
    """The subcommand among `arguments`, what follows `esptool`, or None. An
    option is taken to carry a value unless written `--option=value`, so a
    valueless flag hides the subcommand and the call counts as an opener."""
    takes_value = False
    for argument in arguments:
        if argument is None or argument in SHELL_SEPARATORS:
            return None
        if takes_value:
            takes_value = False
        elif argument.startswith("-"):
            takes_value = "=" not in argument
        else:
            return argument
    return None


def python_list_elements(text):
    """The elements of the Python list literal `text` continues, up to its
    closing bracket: a string literal's value, or None for any other
    expression."""
    elements = []
    position = 0
    while position < len(text):
        match = PY_ELEMENT_RE.match(text, position)
        if match.group(1) is not None or match.group(2) is not None:
            elements.append(match.group(1) if match.group(1) is not None else match.group(2))
        elif match.group(3).strip():
            elements.append(None)
        if match.group(4) != ",":
            break
        position = match.end()
    return elements


def shell_words(text):
    try:
        return shlex.split(text)
    except ValueError:
        return [None]


def opens_port(arguments):
    return esptool_subcommand(arguments) not in OFFLINE_SUBCOMMANDS


def python_esptool_opens_port(text, start, end):
    return any(opens_port(python_list_elements(text[match.end():]))
               for match in ESPTOOL_MODULE_RE.finditer(text, start, end))


def shell_esptool_opens_port(line):
    command = line[:_comment_start(line)]
    return any(opens_port(shell_words(command[match.end():]))
               for match in ESPTOOL_SH_RE.finditer(command))


def _comment_start(line):
    """Where a shell comment starts on `line`: an unquoted `#` at the start
    of a word."""
    quote = None
    for index, char in enumerate(line):
        if quote:
            if char == quote:
                quote = None
        elif char in "'\"":
            quote = char
        elif char == "#" and (index == 0 or line[index - 1].isspace()):
            return index
    return len(line)


def serial_port_openers(root):
    """Every (path, line, reason) where a file outside scripts/device/ opens
    the board's serial port - pyserial, idf_monitor, idf.py flash or
    esptool."""
    root = pathlib.Path(root)
    found = []
    for path in tracked_files(root, ["*.py"]):
        if path.startswith(EXEMPT_PREFIXES) or not (root / path).is_file():
            continue
        text = (root / path).read_text(encoding="utf-8", errors="replace")
        has_serial_import = bool(SERIAL_IMPORT_RE.search(text))
        end = 0
        for number, line in enumerate(text.splitlines(keepends=True), 1):
            start, end = end, end + len(line)
            if is_comment_or_print(path, line):
                continue
            if has_serial_import and SERIAL_OPEN_RE.search(line):
                found.append(Violation(path, number, "opens a pyserial Serial()"))
            elif IDF_MONITOR_RE.search(line):
                found.append(Violation(path, number, "invokes idf_monitor"))
            elif ESPTOOL_API_RE.search(line) or python_esptool_opens_port(text, start, end):
                found.append(Violation(path, number, "invokes esptool"))
            elif IDF_FLASH_RE.search(line):
                found.append(Violation(path, number, "invokes idf/idf.py flash"))
    for path in tracked_files(root, ["*.sh"]):
        if path.startswith(EXEMPT_PREFIXES) or not (root / path).is_file():
            continue
        text = (root / path).read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            if is_comment_or_print(path, line):
                continue
            if IDF_MONITOR_RE.search(line):
                found.append(Violation(path, number, "invokes idf_monitor"))
            elif shell_esptool_opens_port(line):
                found.append(Violation(path, number, "invokes esptool"))
            elif IDF_FLASH_RE.search(line):
                found.append(Violation(path, number, "invokes idf/idf.py flash"))
    return found


RETIRED_WORD_RE = re.compile(
    r"(?<![A-Za-z0-9_.])(" + "|".join(re.escape(name) for name in RETIRED_SCRIPTS) + r")(?![A-Za-z0-9_])")

# Extensions this scan never reads as text - a binary that happens to
# contain these bytes is not a "mention" in the sense this check cares
# about, and decoding one as UTF-8 is wasted work at best.
BINARY_EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".otf", ".ttf",
                     ".woff", ".woff2", ".xcf", ".bin", ".elf", ".o", ".a", ".exe",
                     ".dll", ".pyc", ".gz", ".zip", ".wav", ".mp3", ".mp4"}


def retired_script_mentions(root):
    """Every (path, line, name) where a tracked text file names one of
    `RETIRED_SCRIPTS`. Every tracked file, not only docs: a stale name has
    turned up in CI workflow comments and .gitignore too. scripts/gates/ is
    exempt, the same self-reference reason serial_port_openers() already
    exempts it for."""
    root = pathlib.Path(root)
    found = []
    for path in tracked_files(root, []):
        if path.startswith("scripts/gates/") or pathlib.PurePosixPath(path).suffix.lower() in BINARY_EXTENSIONS:
            continue
        full = root / path
        if not full.is_file():
            continue
        text = full.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            for match in RETIRED_WORD_RE.finditer(line):
                found.append(Violation(path, number, f"names retired script {match.group(1)}"))
    return found


def check(root="."):
    openers = serial_port_openers(root)
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
