#!/usr/bin/env python3
"""Fail when a linked hot function calls the C runtime without a reason.

    python launcher/tools/build/check_hot_calls.py [build-dir]

The hot set comes from code layout pins and active first-party IRAM
attributes. The link map and loaded ROM scripts identify runtime targets
in the ELF. The default build is launcher/build.

Calls are attributed to the innermost inline frame in the containing
function's source file. Put `Library call: <reason>` in a comment on that
line or in the comment block directly above the statement or signature.
The note covers it through its closing `;`, `{` or `}`. Empty and
unused notes fail.

Exit codes: 0 clean, 1 violations, 2 tool or input error.
"""

import argparse
import re
import subprocess
import sys
from bisect import bisect_right
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import memory_map  # noqa: E402
import code_layout  # noqa: E402
import packages  # noqa: E402
from c_comments import scan, sources as comment_sources  # noqa: E402
from layout_pad import placed_sections  # noqa: E402
from objdump_calls import symbol_base  # noqa: E402

ROM_RUNTIME = ("libc", "libgcc")
RUNTIME = (*ROM_RUNTIME, "libm")
RUNTIME_ARCHIVE = re.compile(r"(?:^|[/\\])(?:" + "|".join(RUNTIME) + r")\.a\(")
ROM_ASSIGNMENT = re.compile(r"^\s*(?:PROVIDE(?:_HIDDEN)?\s*\(\s*)?\w+\s*=\s*(0x[\da-fA-F]+)\s*\)?\s*;", re.M)
DIRECT_CALL = frozenset(("call0", "call4", "call8", "call12", "j"))
INDIRECT_CALL = frozenset(("callx0", "callx4", "callx8", "callx12"))
NAMED_TARGET = r"([\da-f]+)\s+<([^>]+)>"
DIRECT_TARGET = re.compile(NAMED_TARGET)
LITERAL_TARGET = re.compile(r"(a\d+),.*\(" + NAMED_TARGET + r"\)")
ANNOTATION = "Library call:"
# A note above a statement or signature covers it through the line ending it.
STATEMENT_ENDS = (";", "{", "}")


def hot_functions(root, sources, link):
    hot_names = {pin for pins in code_layout.baselines(sources).values() for pin in pins}
    functions = [record for record in link["records"] if record.section.lower() in ("t", "w") and record.size]
    hot = {record: [] for record in functions if record.name in hot_names}
    missing = hot_names - {record.name for record in hot}
    by_address = {record.address: record for record in functions}
    starts = sorted(by_address)
    sites, active = memory_map.build_sites(root, link)
    for site in sites:
        if not site.path.endswith((".c", ".h")) or not site.path.startswith(packages.FIRST_PARTY) or not active(site):
            continue
        for required in site.required:
            _, symbol, path, _ = required
            inputs, instances = memory_map.required_copies(site, required, link["placed_inputs"], link["inlines"])
            copies = {by_address[item.address] for item in inputs if item.address in by_address
                      and memory_map.clone_base(by_address[item.address].name) == symbol}
            if not copies and not instances:
                missing.add(f"{path}: {symbol}")
            hot.update((copy, []) for copy in copies)
            for _, _, _, start, end in instances:
                index = bisect_right(starts, start)
                caller = by_address[starts[index - 1]] if index else None
                if caller is None or end > caller.address + caller.size:
                    raise code_layout.LayoutError(f"{path}: {symbol}: inline range {start:#x}–{end:#x} has no ELF caller")
                if caller not in hot or hot[caller]:
                    hot.setdefault(caller, []).append((start, end, symbol))
    if missing:
        raise code_layout.LayoutError("hot functions missing from launcher.elf: " + ", ".join(sorted(missing)))
    return hot


def executable_sections(elf):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.constants import SH_FLAGS

    with elf.open("rb") as stream:
        return {section.name for section in ELFFile(stream).iter_sections()
                if section["sh_flags"] & SH_FLAGS.SHF_EXECINSTR}


def runtime_ranges(map_text, ninja, idf, target, code_sections):
    ranges = [(address, address + size)
              for output, _, address, size, origin in placed_sections(map_text.splitlines())
              if output in code_sections and size and RUNTIME_ARCHIVE.search(origin)]
    rule = memory_map.ninja_rule(ninja, "launcher.elf")
    script_name = re.compile(re.escape(target) + r"\.rom\.(?:" + "|".join(ROM_RUNTIME) + r")[\w.-]*\.ld")
    flags = re.search(r"^  LINK_FLAGS = (.*)$", rule, re.M)
    scripts = set()
    if flags:
        for quoted, bare in re.findall(r'(?:^|\s)-T\s*(?:"([^"]+)"|(\S+))', flags[1]):
            name = (quoted or bare).replace("\\", "/").rsplit("/", 1)[-1]
            if script_name.fullmatch(name):
                scripts.add(name)
    directory = idf / "components" / "esp_rom" / target / "ld"
    rom = {int(address, 16) for script in scripts
           for address in ROM_ASSIGNMENT.findall((directory / script).read_text(encoding="utf-8"))}
    return lambda address: address in rom or any(start <= address < end for start, end in ranges)


def callees(decoded):
    """Return (call site, target address, target name) in instruction order."""
    next_call = {}
    calls = []
    for address, _, mnemonic, operands in reversed(decoded):
        if mnemonic in INDIRECT_CALL:
            next_call[operands.strip()] = address
        elif mnemonic in DIRECT_CALL and (target := DIRECT_TARGET.fullmatch(operands.strip())):
            calls.append((address, int(target[1], 16), symbol_base(target[2])))
        elif mnemonic == "l32r" and (target := LITERAL_TARGET.fullmatch(operands.strip())):
            register, value, name = target.groups()
            calls.append((next_call.get(register, address), int(value, 16), symbol_base(name)))
    return sorted(calls)


def linked_calls(objdump, elf, function, ranges, runtime):
    end = function.address + function.size

    def decode_from(start):
        return code_layout.decode(objdump, elf, f"--start-address={start:#x}", f"--stop-address={end:#x}")

    decoded = code_layout.skip_pools(decode_from(function.address), decode_from)
    for site, target, callee in callees(decoded):
        if runtime(target):
            names = (name for start, end, name in ranges if start <= site < end) if ranges else (function.name,)
            yield from ((name, callee, site) for name in names)


def statement_lines(source, first):
    """The lines from `first` through the one closing its statement or signature."""
    lines = source.splitlines()
    if first > len(lines) or not lines[first - 1].strip():
        return range(first, first)
    end = first
    while end <= len(lines) and lines[end - 1].strip() and not lines[end - 1].rstrip().endswith(STATEMENT_ENDS):
        end += 1
    if end > len(lines) or not lines[end - 1].strip():
        return range(first, first + 1)
    return range(first, end + 1)


def annotations(root):
    """Index first-party notes by covered source line and retain their locations for stale checks."""
    covered = {}
    notes = {}
    for path in comment_sources(root, exclude=False):
        name = path.relative_to(root).as_posix()
        if not name.startswith(packages.FIRST_PARTY):
            continue
        source = path.read_text(encoding="utf-8")
        if ANNOTATION not in source:
            continue
        for comment in scan(path, source):
            _, marker, reason = comment.text.partition(ANNOTATION)
            if not marker:
                continue
            line = next(line for line, raw in zip(comment.line_range, comment.raw_lines) if ANNOTATION in raw)
            notes[comment] = (name, line)
            if not reason.strip():
                continue
            lines = comment.line_range
            for line in lines:
                covered.setdefault((path.resolve(), line), set()).add(comment)
            end = comment.spans[-1][1]
            tail = source[end:].split("\n", 1)[0]
            if comment.own_line and not tail.strip():
                for line in statement_lines(source, lines.stop):
                    covered.setdefault((path.resolve(), line), set()).add(comment)
    return covered, notes


def own_source(frames):
    """Select the innermost line in the containing function's source file."""
    outer = code_layout.SOURCE_LOCATION.fullmatch(frames[-1])
    if outer:
        for frame in frames:
            inner = code_layout.SOURCE_LOCATION.fullmatch(frame)
            if inner and Path(inner[1]) == Path(outer[1]) and int(inner[2]):
                return frame
    return frames[-1]


def violations(root, calls, locations):
    covered, notes = annotations(root)
    used = set()
    bad = []
    for (function, callee, address), location in zip(calls, locations, strict=True):
        match = code_layout.SOURCE_LOCATION.fullmatch(location)
        label = "??:0"
        if match and match[1] != "??" and int(match[2]):
            path = (root / match[1]).resolve()
            line = int(match[2])
            found = covered.get((path, line))
            if found:
                used.update(found)
                continue
            relative = path.relative_to(root) if path.is_relative_to(root) else path
            label = f"{relative.as_posix()}:{line}"
        bad.append(f"{label}: {function} calls {callee} at {address:#x}")
    bad.extend(f"{path}:{line}: Library call: annotation covers no C-runtime call"
               for note, (path, line) in notes.items() if note not in used)
    return sorted(bad)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build", nargs="?", type=Path, default=code_layout.LAUNCHER / "build")
    build = parser.parse_args(argv).build.resolve()
    root = code_layout.REPO
    try:
        if not (build / "launcher.elf").is_file():
            raise code_layout.LayoutError(f"no {build / 'launcher.elf'} - build the release image first")
        link = memory_map.read_link(build, root, inspect_inline=True)
        elf = link["elf"]
        metadata = link["metadata"]
        runtime = runtime_ranges(link["maptext"], link["ninja"],
                                 Path(metadata["idf_path"]), metadata["target"], executable_sections(elf))
        hot = hot_functions(root, code_layout.SOURCES, link)
        objdump = code_layout.find_tool("objdump")
        addr2line = code_layout.find_tool("addr2line")
        ordered = sorted({call for function, ranges in hot.items()
                          for call in linked_calls(objdump, elf, function, ranges, runtime)})
        frames = code_layout.source_frames(addr2line, elf, (address for _, _, address in ordered))
        locations = [own_source(frames[address]) for _, _, address in ordered]
        bad = violations(root, ordered, locations)
    except (ImportError, OSError, RuntimeError, ValueError, KeyError, code_layout.LayoutError, subprocess.CalledProcessError) as exc:
        print(f"check_hot_calls.py: {exc}", file=sys.stderr)
        if isinstance(exc, subprocess.CalledProcessError) and exc.stderr:
            print(exc.stderr.strip(), file=sys.stderr)
        return 2
    if bad:
        print("\n".join(bad))
        return 1
    print(f"no unannotated C-runtime calls in {len(hot)} hot functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
