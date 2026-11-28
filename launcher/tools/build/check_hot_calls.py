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
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "render"))

import code_layout  # noqa: E402
import memory_map  # noqa: E402
import packages  # noqa: E402
from c_comments import blank_comments, is_cpp, scan, sources as comment_sources  # noqa: E402
from layout_pad import placed_sections  # noqa: E402
from objdump_calls import decoded_calls, symbol_base  # noqa: E402

ROM_RUNTIME = ("libc", "libgcc")
RUNTIME = (*ROM_RUNTIME, "libm")
RUNTIME_ARCHIVE = re.compile(r"(?:^|[/\\])(?:" + "|".join(RUNTIME) + r")\.a\(")
ROM_ASSIGNMENT = re.compile(r"^\s*(?:PROVIDE(?:_HIDDEN)?\s*\(\s*)?\w+\s*=\s*(0x[\da-fA-F]+)\s*\)?\s*;", re.M)
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


def linked_calls(objdump, elf, function, ranges, runtime):
    end = function.address + function.size

    def decode_from(start):
        return code_layout.decode(objdump, elf, f"--start-address={start:#x}", f"--stop-address={end:#x}")

    decoded = code_layout.skip_pools(decode_from(function.address), decode_from)
    for site, target, callee in decoded_calls(decoded):
        if target is not None and runtime(target):
            callee = symbol_base(callee)
            names = (name for start, end, name in ranges if start <= site < end) if ranges else (function.name,)
            yield from ((name, callee, site) for name in names)


def statement_lines(source, code, first):
    """The lines from `first` through the one closing its statement or signature."""
    if first > len(source) or not source[first - 1].strip():
        return range(first, first)
    for end in range(first, len(code) + 1):
        if code[end - 1].rstrip().endswith(STATEMENT_ENDS):
            return range(first, end + 1)
    return range(first, first + 1)


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
        source_lines = source.splitlines()
        code = blank_comments(source, cpp=is_cpp(path)).splitlines()
        resolved = path.resolve()
        for comment in scan(path, source):
            reasons = "\n".join(comment.raw_lines).split(ANNOTATION)[1:]
            if not reasons:
                continue
            marker_lines = (line for line, raw in zip(comment.line_range, comment.raw_lines)
                            for _ in range(raw.count(ANNOTATION)))
            lines = comment.line_range
            end = comment.spans[-1][1]
            tail = source[end:].split("\n", 1)[0]
            statement = statement_lines(source_lines, code, lines.stop) if comment.own_line and not tail.strip() else ()
            for index, (line, reason) in enumerate(zip(marker_lines, reasons, strict=True)):
                if comment.kind == "block":
                    reason = reason.partition("*/")[0]
                has_reason = bool(reason.strip(" /*\t\r\n"))
                note = (comment, index)
                notes[note] = (name, line, has_reason)
                if has_reason:
                    for covered_line in (*lines, *statement):
                        covered.setdefault((resolved, covered_line), set()).add(note)
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


def source_site(root, location):
    """Resolve an addr2line frame to the key used by the annotation index."""
    match = code_layout.SOURCE_LOCATION.fullmatch(location)
    if match and match[1] != "??" and int(match[2]):
        return (root / match[1]).resolve(), int(match[2])
    return None


def violations(root, calls, locations):
    covered, notes = annotations(root)
    used = set()
    bad = []
    for (function, callee, address), location in zip(calls, locations, strict=True):
        label = "??:0"
        if site := source_site(root, location):
            path, line = site
            found = covered.get(site)
            if found:
                used.update(found)
                continue
            relative = path.relative_to(root) if path.is_relative_to(root) else path
            label = f"{relative.as_posix()}:{line}"
        bad.append(f"{label}: {function} calls {callee} at {address:#x}")
    for note, (path, line, has_reason) in notes.items():
        if not has_reason:
            bad.append(f"{path}:{line}: Library call: annotation has no reason")
        elif note not in used:
            bad.append(f"{path}:{line}: Library call: annotation covers no C-runtime call")
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
