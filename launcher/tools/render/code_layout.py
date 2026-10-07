#!/usr/bin/env python3
"""Report, write, or check the cache-line layout of pinned render code.

The pinned functions are the ones launcher/main/render/*.c marks with
RENDER_ENTRY_OFFSET, so adding or removing a pin changes the checked layout.

  python launcher/tools/render/code_layout.py launcher/build.diag
  python launcher/tools/render/code_layout.py --write launcher/build.diag
  python launcher/tools/render/code_layout.py --check launcher/build.diag
"""

import argparse
import re
import shutil
import subprocess
import sys
from collections import Counter, namedtuple
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
LAUNCHER = REPO / "launcher"
RENDER = LAUNCHER / "main" / "render"
BASELINE = RENDER / "code_layout.txt"
PINNED = re.compile(r"RENDER_ENTRY_OFFSET\(\d+\)\s+\w+\s+(\w+)\(")
LAYOUT_HEADER = "function\tkind\toffset\tbytes\tlines"
LayoutRow = namedtuple("LayoutRow", "function kind offset bytes lines")
LayoutEntry = namedtuple("LayoutEntry", "function kind address bytes lines")


class LayoutError(Exception):
    pass


def pinned_functions(sources):
    return [name for text in sources for name in PINNED.findall(text)]


FUNCTIONS = pinned_functions(path.read_text(encoding="utf-8") for path in sorted(RENDER.glob("*.c")))


def run(tool, *arguments):
    return subprocess.run([tool, *arguments], check=True, capture_output=True, text=True).stdout


def find_tool(name):
    executable = "xtensa-esp32s3-elf-" + name
    found = shutil.which(executable)
    if found:
        return found
    sys.path.insert(0, str(LAUNCHER / "tools" / "build"))
    from espressif import espressif_tools_root

    pattern = "tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/%s*" % executable
    for candidate in sorted(espressif_tools_root().glob(pattern)):
        if candidate.is_file():
            return str(candidate)
    raise LayoutError("no %s on PATH or under the ESP-IDF tools" % executable)


def cache_line_size(build):
    sdkconfig = build / "sdkconfig"
    if not sdkconfig.is_file():
        raise LayoutError("no %s - build the diagnostics image first" % sdkconfig)
    match = re.search(
        r"^CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE=(\d+)$",
        sdkconfig.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if not match:
        raise LayoutError("CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE is not set in %s" % sdkconfig)
    return int(match.group(1))


def symbols(nm, elf, functions=FUNCTIONS):
    found = {}
    for line in run(nm, "-n", "-S", str(elf)).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3] in functions:
            found[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    return found


def instructions(objdump, elf, function):
    found = []
    for line in run(objdump, "-d", "--disassemble=%s" % function, str(elf)).splitlines():
        match = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+([a-z][a-z0-9.]*)\s*(.*)$", line)
        if match:
            found.append((int(match[1], 16), len(match[2]) // 2, match[3], match[4]))
    return found


def target_of(operands):
    match = re.search(r"\b([0-9a-f]{4,})\s+<", operands)
    return int(match[1], 16) if match else None


# The compiler places a literal pool among the code and jumps over it with a
# short forward j. objdump decodes the pool's bytes as instructions, and a
# "branch" among them moves with the addresses the pool holds, not with the
# code, so it differs from build to build.
POOL_JUMP_MAX = 32


def skip_pools(found):
    """The instructions outside any span a short forward j jumps over that
    nothing outside it branches into: code skipped that way, an else block,
    is always entered by a branch, and a pool never is."""
    spans = []
    for address, size, mnemonic, operands in found:
        target = target_of(operands)
        if mnemonic == "j" and target is not None and 0 < target - (address + size) <= POOL_JUMP_MAX:
            spans.append((address + size, target))

    def inside(address, span):
        return span[0] <= address < span[1]

    pools = [
        span
        for span in spans
        if not any(
            inside(target_of(operands), span)
            for address, _, _, operands in found
            if target_of(operands) is not None and not inside(address, span)
        )
    ]
    return [ins for ins in found if not any(inside(ins[0], pool) for pool in pools)]


def line_span(start, end, line_size):
    return ((end - 1) // line_size) - (start // line_size) + 1


def layout_entries(elf, nm, objdump, line_size, functions=FUNCTIONS):
    located = symbols(nm, elf, functions)
    missing = [function for function in functions if function not in located]
    if missing:
        raise LayoutError("pinned functions missing from %s: %s" % (elf, ", ".join(missing)))
    entries = []
    for function in functions:
        address, size = located[function]
        entries.append(LayoutEntry(function, "function", address, size, line_span(address, address + size, line_size)))
        for instruction_address, size, mnemonic, operands in skip_pools(instructions(objdump, elf, function)):
            target = target_of(operands)
            if mnemonic in ("loop", "loopnez", "loopgtz") and target is not None:
                start = instruction_address + size
                entries.append(LayoutEntry(function, mnemonic, start, target - start, line_span(start, target, line_size)))
            elif target is not None and target < instruction_address and (mnemonic == "j" or mnemonic.startswith("b")):
                end = instruction_address + size
                entries.append(
                    LayoutEntry(
                        function,
                        mnemonic + "-back",
                        target,
                        end - target,
                        line_span(target, end, line_size),
                    )
                )
    return entries


def layout_rows(entries, line_size):
    return [LayoutRow(entry.function, entry.kind, entry.address % line_size, entry.bytes, entry.lines) for entry in entries]


def format_layout(rows, build="launcher/build.diag"):
    lines = [
        "# Generated by: python launcher/tools/render/code_layout.py --write %s" % build,
        LAYOUT_HEADER,
    ]
    lines.extend("%s\t%s\t%d\t%d\t%d" % row for row in rows)
    return "\n".join(lines) + "\n"


def parse_layout(text):
    rows = []
    header_seen = False
    for line_number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if not header_seen:
            if line != LAYOUT_HEADER:
                raise LayoutError("layout line %d: expected %r" % (line_number, LAYOUT_HEADER))
            header_seen = True
            continue
        fields = line.split("\t")
        if len(fields) != 5:
            raise LayoutError("layout line %d: expected five tab-separated columns" % line_number)
        try:
            rows.append(LayoutRow(fields[0], fields[1], *(int(value) for value in fields[2:])))
        except ValueError as exc:
            raise LayoutError("layout line %d: offset, bytes and lines must be integers" % line_number) from exc
    if not header_seen:
        raise LayoutError("layout has no %r header" % LAYOUT_HEADER)
    return rows


def indexed_rows(rows, functions):
    counts = Counter()
    indexed = {}
    for row in rows:
        if row.function not in functions:
            continue
        key = (row.function, row.kind)
        counts[key] += 1
        indexed[(row.function, row.kind, counts[key])] = row
    return indexed


def row_name(key):
    function, kind, ordinal = key
    if kind == "function":
        return "%s function" % function
    return "%s %s[%d]" % (function, kind, ordinal)


def layout_differences(expected, actual):
    expected_pins = {row.function for row in expected if row.kind == "function"}
    actual_pins = {row.function for row in actual if row.kind == "function"}
    differences = ["%s function: pin removed" % function for function in sorted(expected_pins - actual_pins)]
    differences.extend("%s function: pin added" % function for function in sorted(actual_pins - expected_pins))

    common = expected_pins & actual_pins
    expected_rows = indexed_rows(expected, common)
    actual_rows = indexed_rows(actual, common)
    for key in sorted(expected_rows.keys() | actual_rows.keys()):
        if key not in actual_rows:
            differences.append("%s: row removed" % row_name(key))
            continue
        if key not in expected_rows:
            differences.append("%s: row added" % row_name(key))
            continue
        before, after = expected_rows[key], actual_rows[key]
        changed = []
        for field in ("offset", "bytes", "lines"):
            old, new = getattr(before, field), getattr(after, field)
            if old != new:
                changed.append("%s %d -> %d" % (field, old, new))
        if changed:
            differences.append("%s: %s" % (row_name(key), ", ".join(changed)))
    return differences


def display_report(entries, line_size):
    print("| function | kind | address | offset | bytes | lines |")
    print("|---|---|---:|---:|---:|---:|")
    for entry in entries:
        print(
            "| %s | %s | 0x%08x | %d | %d | %d |"
            % (entry.function, entry.kind, entry.address, entry.address % line_size, entry.bytes, entry.lines)
        )


def build_name(build):
    try:
        return build.resolve().relative_to(REPO).as_posix()
    except ValueError:
        return build.as_posix()


def main(argv=None):
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="replace the committed layout")
    mode.add_argument("--check", action="store_true", help="compare with the committed layout")
    parser.add_argument("--nm", help="Xtensa nm override")
    parser.add_argument("--objdump", help="Xtensa objdump override")
    parser.add_argument("build", nargs="?", default=str(LAUNCHER / "build.diag"))
    args = parser.parse_args(argv)

    try:
        build = Path(args.build)
        elf = build / "launcher.elf"
        if not elf.is_file():
            raise LayoutError("no %s - build the diagnostics image first" % elf)
        line_size = cache_line_size(build)
        entries = layout_entries(elf, args.nm or find_tool("nm"), args.objdump or find_tool("objdump"), line_size)
        rows = layout_rows(entries, line_size)
        if args.write:
            BASELINE.write_text(format_layout(rows, build_name(build)), encoding="utf-8", newline="\n")
            print("wrote %s (%d rows, %d-byte lines)" % (BASELINE, len(rows), line_size))
            return 0
        if args.check:
            if not BASELINE.is_file():
                raise LayoutError("no %s - regenerate it with --write" % BASELINE)
            differences = layout_differences(parse_layout(BASELINE.read_text(encoding="utf-8")), rows)
            if differences:
                print("code_layout.py: pinned render layout changed:")
                print("\n".join("  " + difference for difference in differences))
                print(
                    "Measure both revisions on the board with the Sponza perf suite through "
                    "launcher/tools/perf/perf_compare.sh."
                )
                print("If the change is slower, retune RENDER_ENTRY_OFFSET; then regenerate with:")
                print("  python launcher/tools/render/code_layout.py --write %s" % build_name(build))
                return 1
            print("pinned render layout matches %s (%d rows, %d-byte lines)" % (BASELINE, len(rows), line_size))
            return 0
        display_report(entries, line_size)
        return 0
    except (LayoutError, subprocess.CalledProcessError) as exc:
        print("code_layout.py: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
