#!/usr/bin/env python3
"""Report, write, or check the cache-line layout of measured code.

Each source folder owns code_layout.txt for its measured functions. Entry
markers also select functions, so an unrecorded marker fails the check.

  python launcher/tools/render/code_layout.py launcher/build
  python launcher/tools/render/code_layout.py --write launcher/build
  python launcher/tools/render/code_layout.py --check launcher/build

--same-code BASE lists every function whose instructions differ from another
build's, BASE a build folder or its ELF: a change meant to keep code the same
names what it did change.

  python launcher/tools/render/code_layout.py --same-code base/launcher.elf launcher/build
"""

import argparse
import re
import shutil
import subprocess
import sys
from collections import Counter, namedtuple
from pathlib import Path
from typing import NamedTuple

REPO = Path(__file__).resolve().parents[3]
LAUNCHER = REPO / "launcher"
SOURCES = LAUNCHER / "main"
PINNED = re.compile(r"CODE_ENTRY_OFFSET\(\d+\)\s+\w+\s+(\w+)\(")
CONFIG_HEADER = "# Config: "
DEFAULT_BASELINE_CONFIG = "CONFIG_LAUNCHER_SELFTEST"
LAYOUT_HEADER = "function\tkind\toffset\tbytes\tlines\tsource_line"
# Larger loop bodies are dominated by code size, not cache-line placement.
SMALL_LOOP_MAX_LINES = 4
LayoutRow = namedtuple("LayoutRow", "function kind offset bytes lines source_line")
LayoutEntry = namedtuple("LayoutEntry", "function kind address bytes lines source_line")


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


class Symbol(NamedTuple):
    address: int
    name: str
    size: int | None
    section: str


def symbols(nm, elf, functions=None, *, records=False):
    found = [] if records else {}
    for line in run(nm, "-n", "-S", str(elf)).splitlines():
        fields = line.split()
        if len(fields) == 4 and (functions is None or fields[3] in functions):
            record = Symbol(int(fields[0], 16), fields[3], int(fields[1], 16), fields[2])
            if records:
                found.append(record)
            else:
                found[record.address] = (record.name, record.size)
        elif records and len(fields) == 3 and (functions is None or fields[2] in functions):
            found.append(Symbol(int(fields[0], 16), fields[2], None, fields[1]))
    return found


def decode(objdump, elf, *arguments):
    return parse_objdump(run(objdump, "-d", *arguments, str(elf)))


def parse_objdump(text):
    found = []
    for line in text.splitlines():
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


XTENSA_LITERAL_BYTES = 4


def skip_pools(found, decode_from=None):
    """Drop literal targets and skipped pools, decoding code again after each pool."""
    if not found:
        return found
    start = found[0][0]
    end = found[-1][0] + found[-1][1]
    pools = set()
    while True:
        literals = {target_of(operands) for _, _, mnemonic, operands in found if mnemonic == "l32r"}
        pending = sorted(address for address in literals
                         if address is not None and start <= address <= end - XTENSA_LITERAL_BYTES and address not in pools)
        if not pending:
            repaired = _skip_jump_pools(found, decode_from)
            if repaired == found:
                return found
            found = repaired
            continue
        for address in pending:
            pools.add(address)
            resume = address + XTENSA_LITERAL_BYTES
            prefix = [ins for ins in found if ins[0] + ins[1] <= address]
            suffix = decode_from(resume) if decode_from else [ins for ins in found if ins[0] >= resume]
            found = prefix + [ins for ins in suffix if ins[0] < end]


def _skip_jump_pools(found, decode_from):
    """A skipped span with no incoming branch is a pool. An entry past its
    start marks code decoded out of step after a pool or alignment bytes."""
    spans = []
    for address, size, mnemonic, operands in found:
        target = target_of(operands)
        if mnemonic == "j" and target is not None and target > address + size:
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
    if not functions:
        return []
    located = {name: (address, size) for address, (name, size) in symbols(nm, elf, functions).items()}
    missing = [function for function in functions if function not in located]
    if missing:
        raise LayoutError("pinned functions missing from %s: %s" % (elf, ", ".join(missing)))
    entries = []
    source_addresses = []
    for function in functions:
        address, size = located[function]
        entries.append(LayoutEntry(function, "function", address, size, line_span(address, address + size, line_size), 0))
        source_addresses.append(address)
        for instruction_address, size, mnemonic, operands in skip_pools(*instructions(objdump, elf, function, address + size)):
            target = target_of(operands)
            if mnemonic in ("loop", "loopnez", "loopgtz") and target is not None:
                start, end, kind = instruction_address + size, target, mnemonic
            elif target is not None and address <= target < instruction_address and (mnemonic == "j" or mnemonic.startswith("b")):
                start, end, kind = target, instruction_address + size, mnemonic + "-back"
            else:
                continue
            if end > start and line_span(start, end, line_size) <= SMALL_LOOP_MAX_LINES:
                entries.append(LayoutEntry(function, kind, start, end - start, line_span(start, end, line_size), 0))
                source_addresses.append(instruction_address)
    locations = []
    # Inline instructions need the caller's line in the pinned function.
    for location in run(find_tool("addr2line"), "-a", "-i", "-e", str(elf),
                        *("0x%x" % address for address in source_addresses)).splitlines():
        if re.fullmatch(r"0x[0-9a-fA-F]+", location):
            locations.append("")
        elif locations:
            locations[-1] = location
    if len(locations) != len(entries):
        raise LayoutError("addr2line returned an incomplete source map for %s" % elf)
    first_lines = {}
    for index, (entry, location) in enumerate(zip(entries, locations)):
        match = re.match(r"(.+):(\d+)(?:\s.*)?$", location)
        if not match or int(match[2]) == 0:
            raise LayoutError("no source line for %s at 0x%x" % (entry.function, source_addresses[index]))
        source, line = match[1], int(match[2])
        if entry.kind == "function":
            first_lines[entry.function] = (source, line)
        first_source, first_line = first_lines[entry.function]
        if source != first_source:
            raise LayoutError("%s loop source differs from its function: %s" % (entry.function, location))
        entries[index] = entry._replace(source_line=line - first_line)
    return entries


def layout_rows(entries, line_size):
    return [LayoutRow(entry.function, entry.kind, entry.address % line_size, entry.bytes, entry.lines, entry.source_line) for entry in entries]


def format_layout(rows, build="launcher/build", metadata=()):
    lines = [
        "# Generated by: python launcher/tools/render/code_layout.py --write %s" % build,
        *metadata,
        LAYOUT_HEADER,
    ]
    lines.extend("%s\t%s\t%d\t%d\t%d\t%d" % row for row in rows)
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
        if len(fields) != 6:
            raise LayoutError("layout line %d: expected six tab-separated columns" % line_number)
        try:
            rows.append(LayoutRow(fields[0], fields[1], *(int(value) for value in fields[2:])))
        except ValueError as exc:
            raise LayoutError("layout line %d: offset, bytes, lines and source_line must be integers" % line_number) from exc
    if not header_seen:
        raise LayoutError("layout has no %r header" % LAYOUT_HEADER)
    return rows


def indexed_rows(rows, functions):
    counts = Counter()
    indexed = {}
    for row in rows:
        if row.function not in functions:
            continue
        key = (row.function, row.kind, row.source_line)
        counts[key] += 1
        indexed[(*key, counts[key])] = row
    return indexed


def row_name(key):
    function, kind, source_line, ordinal = key
    if kind == "function":
        return "%s function" % function
    name = "%s %s@%+d" % (function, kind, source_line)
    return name if ordinal == 1 else "%s[%d]" % (name, ordinal)


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


FUNCTION_HEADER = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
# An address beside the symbol it names: only the symbol is kept, so code that
# moved but did not change compares equal.
NAMED_ADDRESS = re.compile(r"\b[0-9a-f]{4,}\s+(<[^>]+>)")


def resync(found, decode_from):
    """Decode again from each branch target the linear sweep stepped over, as
    alignment bytes before a loop entered only from inside a skipped span."""
    while found:
        starts = {ins[0] for ins in found}
        end = found[-1][0] + found[-1][1]
        missed = sorted(target for target in (target_of(operands) for _, _, mnemonic, operands in found if mnemonic != "l32r")
                        if target is not None and found[0][0] <= target < end and target not in starts)
        if not missed:
            break
        again = []
        for ins in decode_from(missed[0]):
            if ins[0] in starts:
                break
            again.append(ins)
        resume = again[-1][0] + again[-1][1] if again else missed[0]
        found = [ins for ins in found if ins[0] + ins[1] <= missed[0]] + again + [ins for ins in found if ins[0] >= resume]
    return found


def function_code(text, decode_range=None, precise=()):
    """{function: [instruction]} from `objdump -d` text, each instruction its
    mnemonic and operands without addresses, literal loads without their
    pool's place. A name two static functions share gets #2 on the second.
    Functions named in `precise` are decoded again past each pool through
    decode_range(start, end), as layout_entries() does: pool bytes the linear
    sweep read as code would differ with the addresses they hold."""
    found = {}
    name, body = None, []

    def close(end):
        if name is not None:
            key, copy = name, 1
            while key in found:
                copy += 1
                key = "%s#%d" % (name, copy)
            again = (lambda start: decode_range(start, end)) if key in precise else None
            found[key] = [
                "l32r " + operands.split(",")[0] if mnemonic == "l32r"
                else ("%s %s" % (mnemonic, NAMED_ADDRESS.sub(r"\1", operands))).rstrip()
                for _, _, mnemonic, operands in (resync(skip_pools(body, again), again) if again else skip_pools(body))
            ]

    for line in text.splitlines():
        header = FUNCTION_HEADER.match(line)
        if header:
            close(int(header[1], 16))
            name, body = header[2], []
        elif name is not None:
            body.extend(parse_objdump(line))
    close(body[-1][0] + body[-1][1] if body else 0)
    return found


def code_differences(base, head):
    """Functions whose instructions differ between two function_code() maps,
    then those only one of them has."""
    changed = sorted(name for name in base.keys() & head.keys() if base[name] != head[name])
    return (["%s: changed (%d -> %d instructions)" % (name, len(base[name]), len(head[name])) for name in changed]
            + ["%s: removed" % name for name in sorted(base.keys() - head.keys())]
            + ["%s: added" % name for name in sorted(head.keys() - base.keys())])


def elf_of(path):
    elf = path / "launcher.elf" if path.is_dir() else path
    if not elf.is_file():
        raise LayoutError("no %s - build it first" % elf)
    return elf


def same_code(base, head, objdump):
    elfs = [elf_of(path) for path in (base, head)]
    texts = [run(objdump, "-d", str(elf)) for elf in elfs]
    rough = [function_code(text) for text in texts]
    suspects = {name for name in rough[0].keys() & rough[1].keys() if rough[0][name] != rough[1][name]}
    codes = [
        function_code(text, lambda start, end, elf=elf: decode(objdump, elf, "--start-address=0x%x" % start,
                                                                "--stop-address=0x%x" % end), suspects)
        for text, elf in zip(texts, elfs)
    ] if suspects else rough
    differences = code_differences(*codes)
    if differences:
        print("code_layout.py: %d functions differ from %s:" % (len(differences), base))
        print("\n".join("  " + difference for difference in differences))
    else:
        print("every function matches %s" % base)


def display_report(entries, line_size):
    print("| function | kind | address | offset | bytes | lines | source_line |")
    print("|---|---|---:|---:|---:|---:|---:|")
    for entry in entries:
        print(
            "| %s | %s | 0x%08x | %d | %d | %d | %d |"
            % (entry.function, entry.kind, entry.address, entry.address % line_size, entry.bytes, entry.lines, entry.source_line)
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
    mode.add_argument("--same-code", type=Path, metavar="BASE", help="list functions whose code differs from BASE's")
    parser.add_argument("--baseline", type=Path, help="select one source folder baseline")
    parser.add_argument("--nm", help="Xtensa nm override")
    parser.add_argument("--objdump", help="Xtensa objdump override")
    parser.add_argument("build", nargs="?", default=str(LAUNCHER / "build"))
    args = parser.parse_args(argv)

    try:
        if args.same_code:
            same_code(args.same_code, Path(args.build), args.objdump or find_tool("objdump"))
            return 0
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
        if (args.check or args.write) and not any(
            baseline_applies(path.read_text(encoding="utf-8") if path.is_file() else "", sdkconfig)
            for path in owners
        ):
            raise LayoutError("no baseline applies to build %s" % requested_build)
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
