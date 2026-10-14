#!/usr/bin/env python3
"""Report, write, or check the cache-line layout of measured code.

Each source folder owns code_layout.txt for its measured functions. Entry
markers also select functions, so an unrecorded marker fails the check.

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
SOURCES = LAUNCHER / "main"
PINNED = re.compile(r"CODE_ENTRY_OFFSET\(\d+\)\s+\w+\s+(\w+)\(")
CONFIG_HEADER = "# Config: "
DEFAULT_BASELINE_CONFIG = "CONFIG_LAUNCHER_SELFTEST"
LAYOUT_HEADER = "function\tkind\toffset\tbytes\tlines"
LayoutRow = namedtuple("LayoutRow", "function kind offset bytes lines")
LayoutEntry = namedtuple("LayoutEntry", "function kind address bytes lines")


class LayoutError(Exception):
    pass


def pinned_functions(sources):
    return [name for text in sources for name in PINNED.findall(text)]


def baselines(root):
    owners = {path.parent for path in root.rglob("code_layout.txt")}
    pins = {}
    for source in sorted(root.rglob("*.c")):
        functions = pinned_functions([source.read_text(encoding="utf-8")])
        if functions:
            owners.add(source.parent)
            pins.setdefault(source.parent, []).extend(functions)
    return {owner / "code_layout.txt": pins.get(owner, []) for owner in sorted(owners)}


def baseline_applies(text, sdkconfig):
    config = next(
        (line.removeprefix(CONFIG_HEADER) for line in text.splitlines() if line.startswith(CONFIG_HEADER)),
        DEFAULT_BASELINE_CONFIG,
    )
    return config + "=y" in sdkconfig.splitlines()


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


def symbols(nm, elf, functions=None):
    found = {}
    for line in run(nm, "-n", "-S", str(elf)).splitlines():
        fields = line.split()
        if len(fields) == 4 and (functions is None or fields[3] in functions):
            found[int(fields[0], 16)] = (fields[3], int(fields[1], 16))
    return found


def decode(objdump, elf, *arguments):
    found = []
    for line in run(objdump, "-d", *arguments, str(elf)).splitlines():
        match = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+([a-z][a-z0-9.]*)\s*(.*)$", line)
        if match:
            found.append((int(match[1], 16), len(match[2]) // 2, match[3], match[4]))
    return found


def instructions(objdump, elf, function, end):
    """The function's instructions, and how to decode its code again from
    one address on, where the linear sweep fell out of step."""
    return decode(objdump, elf, "--disassemble=%s" % function), lambda start: decode(
        objdump, elf, "--start-address=0x%x" % start, "--stop-address=0x%x" % end
    )


def target_of(operands):
    match = re.search(r"\b([0-9a-f]{4,})\s+<", operands)
    return int(match[1], 16) if match else None


# The compiler places a literal pool among the code and jumps over it with a
# short forward j. objdump decodes the pool's bytes as instructions, and a
# "branch" among them moves with the addresses the pool holds, not with the
# code, so it differs from build to build.
POOL_JUMP_MAX = 32


def skip_pools(found, decode_from=None):
    """The instructions outside any span a short forward j jumps over that
    nothing outside it branches into: code skipped that way, an else block,
    is always entered by a branch, and a pool never is. A span entered past
    its start holds a pool and then code the sweep read out of step, so the
    pool is dropped and the code decoded again from where it is entered."""
    spans = []
    for address, size, mnemonic, operands in found:
        target = target_of(operands)
        if mnemonic == "j" and target is not None and 0 < target - (address + size) <= POOL_JUMP_MAX:
            spans.append((address + size, target))

    def inside(address, span):
        return span[0] <= address < span[1]

    def entries(span):
        return sorted(
            target_of(operands)
            for address, _, _, operands in found
            if target_of(operands) is not None and not inside(address, span) and inside(target_of(operands), span)
        )

    starts = {ins[0] for ins in found}
    kept = list(found)
    for span in spans:
        entered = entries(span)
        if not entered:
            kept = [ins for ins in kept if not inside(ins[0], span)]
        elif entered[0] not in starts and decode_from is not None:
            again = []
            for ins in decode_from(entered[0]):
                if ins[0] in starts:
                    break
                again.append(ins)
            resume = again[-1][0] + again[-1][1] if again else entered[0]
            kept = [ins for ins in kept if ins[0] < span[0]] + again + [ins for ins in kept if ins[0] >= resume]
    return kept


def line_span(start, end, line_size):
    return ((end - 1) // line_size) - (start // line_size) + 1


def layout_entries(elf, nm, objdump, line_size, functions=None):
    if functions is None:
        functions = [function for group in baselines(SOURCES).values() for function in group]
    located = {name: (address, size) for address, (name, size) in symbols(nm, elf, functions).items()}
    missing = [function for function in functions if function not in located]
    if missing:
        raise LayoutError("pinned functions missing from %s: %s" % (elf, ", ".join(missing)))
    entries = []
    for function in functions:
        address, size = located[function]
        entries.append(LayoutEntry(function, "function", address, size, line_span(address, address + size, line_size)))
        for instruction_address, size, mnemonic, operands in skip_pools(*instructions(objdump, elf, function, address + size)):
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


def format_layout(rows, build="launcher/build.diag", metadata=()):
    lines = [
        "# Generated by: python launcher/tools/render/code_layout.py --write %s" % build,
        *metadata,
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
    parser.add_argument("--baseline", type=Path, help="select one source folder baseline")
    parser.add_argument("--nm", help="Xtensa nm override")
    parser.add_argument("--objdump", help="Xtensa objdump override")
    parser.add_argument("build", nargs="?", default=str(LAUNCHER / "build.diag"))
    args = parser.parse_args(argv)

    try:
        requested_build = Path(args.build)
        build = requested_build.resolve()
        elf = build / "launcher.elf"
        if not elf.is_file():
            raise LayoutError("no %s - build the diagnostics image first" % elf)
        line_size = cache_line_size(build)
        owners = baselines(SOURCES)
        if args.baseline:
            selected = args.baseline.resolve()
            owners = {path: functions for path, functions in owners.items() if path.resolve() == selected}
            if not owners:
                raise LayoutError("no baseline source folder for %s" % args.baseline)
        sdkconfig = (build / "sdkconfig").read_text(encoding="utf-8")
        failed = False
        for baseline, functions in owners.items():
            text = baseline.read_text(encoding="utf-8") if baseline.is_file() else ""
            if not baseline_applies(text, sdkconfig):
                if args.baseline:
                    raise LayoutError("%s does not apply to this build configuration" % baseline)
                continue
            entries = layout_entries(elf, args.nm or find_tool("nm"), args.objdump or find_tool("objdump"), line_size, functions)
            rows = layout_rows(entries, line_size)
            if args.write:
                metadata = [line for line in text.splitlines() if line.startswith(CONFIG_HEADER)]
                baseline.write_text(format_layout(rows, build_name(requested_build), metadata), encoding="utf-8", newline="\n")
                print("wrote %s (%d rows, %d-byte lines)" % (baseline, len(rows), line_size))
            elif args.check:
                if not baseline.is_file():
                    raise LayoutError("no %s - regenerate it with --write" % baseline)
                differences = layout_differences(parse_layout(baseline.read_text(encoding="utf-8")), rows)
                if differences:
                    print("code_layout.py: measured layout changed in %s:" % baseline)
                    print("\n".join("  " + difference for difference in differences))
                    print("Measure both revisions with launcher/tools/perf/perf_compare.sh; "
                          "accept and re-record with --write --baseline, or fix the layout.")
                    failed = True
                else:
                    print("measured layout matches %s (%d rows, %d-byte lines)" % (baseline, len(rows), line_size))
            else:
                print(baseline)
                display_report(entries, line_size)
        return 1 if failed else 0
    except (LayoutError, subprocess.CalledProcessError) as exc:
        print("code_layout.py: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
