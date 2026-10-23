#!/usr/bin/env python3
"""Generate the fast-path reference from owner annotations and committed captures."""
import argparse
from dataclasses import dataclass
import math
from functools import lru_cache
from pathlib import Path
import re
import statistics
import sys

import math_bench_table as bench
from generated_blocks import replace_block

ROOT = bench.ROOT
HEADERS = Path("launcher/packages/math/include/math")
DOCUMENT = Path("docs/math/Fast-Paths.md")
BLOCK = "math-fast-paths"
COMMAND = "python launcher/tools/perf/fast_paths.py --check"
ANNOTATION = re.compile(r"^/\* MATH_FAST_PATH\(\s*(\w+),\s*(\w+),\s*(\w+)\) \*/", re.M)
FUNCTION = re.compile(r"^[ \t]*(?!(?:return|if|for|while|switch|case|else)\b)(?:(?:static[ \t]+)?(?:inline[ \t]+)?[\w*]+[ \t\n]+)(\w+)[ \t]*\([^;{}]*\)[ \t]*\{", re.M)
CALL = re.compile(r"\b(\w+)\s*\(")


@dataclass
class Annotation:
    key: str
    kind: str
    fast: str
    plain: str
    limit_name: str
    unit: str
    method: str
    target: str
    header: Path
    line: int
    limit: float
    description: str
    formula: str
    coverage: str


def annotations(root=ROOT):
    result = []
    source = (root / "launcher/packages/math/tests/suite_math_numbers.c").read_text(encoding="utf-8")
    contracts = {name: (unit, method) for name, unit, method in re.findall(
        r'MATHERROR name=(\w+) type=\w+ max=.*? unit=(ulp|relative|lsb) method=(proof|sweep)', source)}
    for header in sorted((root / HEADERS).rglob("*.h")):
        text = header.read_text(encoding="utf-8")
        matches = list(ANNOTATION.finditer(text))
        if len(matches) != len(re.findall(r"^/\* MATH_FAST_PATH\(", text, re.M)):
            raise ValueError(f"malformed fast-path annotation: {header}")
        for match in matches:
            fast, plain, limit_name = match.groups()
            following = FUNCTION.search(re.sub(r"__attribute__\(\(.*?\)\)", "", text[match.end():]))
            if not following:
                raise ValueError(f"missing function below {fast}")
            key, kind = following[1], fast.rsplit("_", 1)[1]
            if key not in contracts:
                raise ValueError(f"missing MATHERROR row for {fast}: {key}")
            define = re.search(r"^#define\s+" + limit_name + r"[ \t]+([^\n]+)$", text, re.M)
            if not define:
                raise ValueError(f"missing limit {limit_name}: {header}")
            value = define[1].strip().removesuffix("F").removesuffix("f")
            limit = float.fromhex(value) if value.lower().startswith("0x") else float(value)
            if not math.isfinite(limit) or limit < 0:
                raise ValueError(f"invalid limit {limit_name}")
            before = text[:match.start()]
            comments = [" ".join(re.sub(r"^\s*\* ?", "", body, flags=re.M).split())
                        for body in re.findall(r"/\*(.*?)\*/", before, re.S) if body.count("|") == 2]
            if not comments:
                raise ValueError(f"missing description, formula or coverage: {key}")
            description, formula, coverage = map(str.strip, comments[-1].split("|"))
            body = function_bodies(text)[key]
            target = "Target-specific" if re.search(r"^#(?:if|elif).*(?<!!)defined\(__XTENSA__\).*CONFIG_MATH_", body, re.M) else "Portable C"
            unit, method = contracts[key]
            result.append(Annotation(key, kind, fast, plain, limit_name, unit, method, target,
                                     header.relative_to(root), before.count("\n") + 1,
                                     limit, description, formula, coverage))
    if not result:
        raise ValueError("no fast-path annotations")
    return result


def validate_sections(text, items):
    sections = re.findall(r"^## (\w+)\s*$", text, re.M)
    expected = list(dict.fromkeys(item.key for item in items))
    if len(sections) != len(expected) or set(sections) != set(expected):
        raise ValueError(f"fast-path section mismatch: expected {expected}, found {sections}")


def hardware_features(root, item):
    body = function_bodies((root / item.header).read_text(encoding="utf-8"))[item.key]
    symbols = sorted(set(re.findall(r"CONFIG_MATH_\w+", body)))
    config = (root / "launcher/packages/math/Kconfig").read_text(encoding="utf-8") if symbols else ""
    expected = set()
    for symbol in symbols:
        definition = re.search(r"    config " + symbol.removeprefix("CONFIG_") + r"\n(.*?)(?=\n    config |\nendmenu)", config, re.S)
        if not definition:
            raise ValueError(f"missing Kconfig definition: {symbol}")
        expected.update(op for op in bench.FPU_OPS if op in definition[1])
    expected.update(op for op in bench.FPU_OPS if op in body)
    return symbols, expected


def validate_hardware(root, items, fpu):
    for item in items:
        if item.target == "Portable C":
            continue
        _, expected = hardware_features(root, item)
        observed = set(fpu[item.fast].partition(";")[0].split())
        if not expected or not expected & observed:
            raise ValueError(f"{item.fast}: target wrapper lacks an expected seed/fused instruction ({sorted(expected)})")


def reference_pages(item, root=ROOT):
    pages = {item.header.parent.name.capitalize()}
    functions, owners = package_functions(root, with_owners=True)
    source = (root / "launcher/test/suites/suite_math_perf.c").read_text(encoding="utf-8")
    row_name = item.fast.rsplit("_", 1)[0]
    for line in source.splitlines():
        if re.search(r"\bX\(" + row_name + r",", line):
            if any(name in owners and owners[name].parent.name == "linear" for name in CALL.findall(line)):
                pages.add("Linear")
    return pages


def validate_links(root, items):
    for item in items:
        for page in reference_pages(item, root):
            path = root / "docs/math" / (page + ".md")
            if not path.exists() or f"Fast-Paths.md#{item.key.replace('_', '-')}" not in path.read_text(encoding="utf-8"):
                raise ValueError(f"{page}.md lacks link to section {item.key}")


def validate(items, rows, capture):
    manifest = {row[0]: row for row in rows}
    errors, bins = bench.parse_errors(capture), bench.parse_bins(capture)
    for item in items:
        for key in (item.fast, item.plain):
            if key not in manifest:
                raise ValueError(f"annotation row missing: {key}")
        identity = (item.key, item.kind)
        if identity in bins and identity not in errors:
            raise ValueError(f"missing error summary for bins: {identity}")
        if identity in errors:
            maximum, unit, method = re.fullmatch(r"(\S+) (\w+) \((\w+)\)", errors[identity]).groups()
            if unit != item.unit or method != item.method or any(row[2] != unit for row in bins.get(identity, [])):
                raise ValueError(f"error contract mismatch: {identity}")
            if max([float(maximum), *(row[1] for row in bins.get(identity, []))]) > item.limit:
                raise ValueError(f"measured error exceeds {item.limit_name}: {identity}")
            if max((row[1] for row in bins.get(identity, [])), default=0) > float(maximum):
                raise ValueError(f"bin exceeds error summary: {identity}")
    return errors, bins


def function_bodies(text):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    text = re.sub(r"__attribute__\(\(.*?\)\)", "", text)
    result = {}
    for match in FUNCTION.finditer(text):
        if match[1] in ("if", "for", "while", "switch"):
            continue
        depth, end = 1, match.end()
        while end < len(text) and depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        result[match[1]] = text[match.end():end - 1]
    return result


@lru_cache(maxsize=4)
def package_functions(root, with_owners=False):
    sys.path.insert(0, str(ROOT / "scripts/gates"))
    from check_math_docs import HEADER_CALL, TEMPLATE_MACRO, MACRO_CALL, split_args
    macros, headers, functions, owners = {}, [], {}, {}
    for path in sorted((root / HEADERS).rglob("*.h")):
        text = path.read_text(encoding="utf-8")
        if path.name.endswith("_template.h"):
            for name, params, body in TEMPLATE_MACRO.findall(text):
                macros[name] = (split_args(params), body.replace("\\\n", "\n"))
        else:
            headers.append((path, text))

    def expand(name, arguments, owner, active=()):
        if name not in macros or name in active:
            return
        params, body = macros[name]
        bind = dict(zip(params, arguments))
        for param, value in bind.items():
            body = re.sub(r"\b" + param + r"\b", lambda _: value, body)
        body = re.sub(r"\s*##\s*", "", body)
        made = function_bodies(body)
        functions.update(made)
        owners.update(dict.fromkeys(made, owner))
        for callee, args in MACRO_CALL.findall(body):
            expand(callee, split_args(args), owner, (*active, name))

    for path, text in headers:
        for name, args in HEADER_CALL.findall(text):
            expand(name, split_args(args), path)
    for path, text in headers:
        made = function_bodies(text)
        functions.update(made)
        owners.update(dict.fromkeys(made, path))
    return (functions, owners) if with_owners else functions


def tables_for(item, root):
    functions = package_functions(root)
    text = (root / item.header).read_text(encoding="utf-8")
    tables = re.findall(r"static const \w+ (\w+)\[", text)
    active, visited = [item.key], set()
    while active:
        name = active.pop()
        if name in visited:
            continue
        visited.add(name)
        active.extend(set(CALL.findall(functions.get(name, ""))) & functions.keys() - visited)
    bodies = "\n".join(functions.get(name, "") for name in visited)
    return [name for name in tables if re.search(r"\b" + name + r"\b", bodies)]


def callers(root, key):
    functions = package_functions(root)
    reachable = {key}
    while True:
        expanded = reachable | {name for name, body in functions.items() if set(CALL.findall(body)) & reachable}
        if expanded == reachable:
            break
        reachable = expanded
    modules = {}
    sources = [path for folder in ("main", "packages") for path in (root / "launcher" / folder).rglob("*")]
    for path in sorted(sources):
        if path.suffix not in (".c", ".h") or any(part in ("tests", "tools") for part in path.parts):
            continue
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
        if path.is_relative_to(root / HEADERS):
            text = "\n".join(body for name, body in function_bodies(text).items() if name != key)
        used = set(CALL.findall(text)) & reachable
        if used:
            relative = path.relative_to(root / "launcher")
            if path.is_relative_to(root / HEADERS):
                module = "math/" + path.relative_to(root / HEADERS).parts[0]
            else:
                module = relative.parts[1] if len(relative.parts) > 2 else relative.parts[0]
            modules.setdefault("apps (not named)" if relative.parts[:2] == ("main", "apps") else module, set()).update(used)
    return modules


def parse_calls(text):
    windows, current = [], {}
    number = rf"{bench.NUMBER}(?:[eE][+-]?[0-9]+)?"
    for line in text.splitlines():
        if line == "MATHCALLS begin":
            if current:
                raise ValueError("unterminated call-count window")
            current = {}
        elif line == "MATHCALLS end":
            windows.append(current)
            current = {}
        elif line.startswith("MATHCALLS "):
            match = re.fullmatch(rf"MATHCALLS fn=(\w+) caller=(.+) per_frame=({number})", line)
            if not match or not math.isfinite(float(match[3])):
                raise ValueError(f"malformed call count: {line}")
            key, caller, value = match.groups()
            caller = caller.replace("\\", "/")
            owner = re.search(r"(?:^|/)(?:launcher/)?(main|packages)/(.+)", caller)
            if not owner:
                continue
            module = owner[2].split("/", 1)[0] if "/" in owner[2] else owner[1]
            identity = key, "apps (not named)" if owner[1] == "main" and module == "apps" else module
            current[identity] = current.get(identity, 0) + float(value)
    if current:
        raise ValueError("missing MATHCALLS end")
    identities = {identity for window in windows for identity in window}
    return {identity: (max(values), statistics.median(values)) for identity in identities
            for values in [[window.get(identity, 0) for window in windows]]}


def build_rows(item, root):
    body = function_bodies((root / item.header).read_text(encoding="utf-8"))[item.key]
    conditions = re.findall(r"^#(?:if|elif) (.*CONFIG_MATH_.*)$", body, re.M)
    symbols = sorted(set(re.findall(r"CONFIG_MATH_\w+", "\n".join(conditions))))
    qemu = (root / "launcher/sdkconfig.defaults.qemu").read_text(encoding="utf-8")
    kconfig = (root / "launcher/packages/math/Kconfig").read_text(encoding="utf-8")
    lines = []
    for symbol in symbols:
        option = symbol.removeprefix("CONFIG_")
        definition = re.search(r"    config " + option + r"\n(.*?)(?=\n    config |\nendmenu)", kconfig, re.S)
        help_text = " ".join(definition[1].split("help", 1)[1].split()) if definition else ""
        value = re.search(r"^" + symbol + r"=(\w+)$", qemu, re.M)
        disabled = re.search(r"^# " + symbol + r" is not set$", qemu, re.M)
        setting = value[1] if value else "n" if disabled else "default"
        lines.append(f"- `{symbol}`: {help_text} QEMU: `{setting}`.")
    guard = " ; ".join(conditions) or "all builds (portable C)"
    lines += [f"Host: `!defined(__XTENSA__)`; source conditions: `{guard}`.",
              f"QEMU: `CONFIG_LAUNCHER_QEMU`; source conditions: `{guard}` (values above).",
              f"Target: source conditions `{guard}`; configured values come from the capture build."]
    return "\n\n".join(lines)



def plain_expression(item, root):
    text = (root / "launcher/test/suites/suite_math_perf.c").read_text(encoding="utf-8")
    match = re.search(r"/\* MATH_PLAIN\(" + item.plain + r"\): (.*?) \*/", text)
    if not match:
        raise ValueError(f"plain row has no C description: {item.plain}")
    return match[1]


def render(root, items, rows, measured, errors, bins, sizes, counts, fpu, target):
    manifest = {row[0]: row for row in rows}
    groups = {}
    for item in items:
        groups.setdefault(item.key, []).append(item)
    lines = []
    for key, group in groups.items():
        item = group[0]
        lines += [f'<a id="{key.replace("_", "-")}"></a>', "", f"## {key}", "", "**Plain way**", ""]
        for pair in group:
            lines += [plain_expression(pair, root) + ".", ""]
        lines += ["**What we do**", "", item.description, "", "```math", item.formula, "```", "", f"Implementation target: {item.target}.", "", "**Cost**", "",
                  "| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |", "|---|---|---:|---:|---:|"]
        for pair in group:
            fast, plain = measured.get(pair.fast), measured.get(pair.plain)
            ratio = f"{plain[0] / fast[0]:.2f}" if fast and plain else "-"
            for row in (pair.fast, pair.plain):
                values = measured.get(row)
                warm, cold = (f"{value:.1f}" for value in values[:2]) if values else ("-", "-")
                lines.append(f"| `{row}` | {target if values else '-'} | {warm} | {cold} | {ratio if row == pair.fast else 'n/a'} |")
        error = errors.get((key, item.kind), "-")
        lines += ["", "**Error**", "", "| Measured target | Maximum error | Limit | Unit | Method |",
                  "|---|---:|---|---|---|",
                  f"| {target if error != '-' else '-'} | {error.split()[0]} | `{item.limit_name}` ({item.limit:g}) | {item.unit} | {item.method} |",
                  "", f"{item.method.capitalize()}: {item.coverage}.", ""]
        if (key, item.kind) in bins:
            lines += [f"![Error across the input range](../images/math/fast-path-{key}.png)", ""]
        else:
            lines += ["Error curve: - (capture pending).", ""]
        lines += ["**Size**", "", "| Row | Code bytes / region | Callee bytes | Table bytes |", "|---|---|---|---|"]
        for pair in group:
            for row in (pair.fast, pair.plain):
                records = sizes.get(row, [])
                code = [f"{size if size is not None else '-'} / {region}" for name, size, region in records if name == bench.ROW_SYMBOL_PREFIX + row]
                callees = [f"`{name}`: {size if size is not None else '-'} / {region}" for name, size, region in records
                           if name != bench.ROW_SYMBOL_PREFIX + row and region != "rodata"]
                tables = [f"`{name}`: {size}" for name, size, region in records if region == "rodata"]
                lines.append(f"| `{row}` | {', '.join(code) or '-'} | {', '.join(callees) or ('none' if records else '-')} | {', '.join(tables) or ('0' if records else '-')} |")
        symbols, _ = hardware_features(root, item)
        observed = ", ".join(f"`{op}`" for op in fpu[item.fast].partition(";")[0].split() if op in bench.FPU_OPS)
        gates = ", ".join(f"`{symbol}`" for symbol in symbols)
        feature = f"ELF instructions: {observed or 'none'}. Gating symbols: {gates or 'none'}."
        lines += ["", "**Builds**", "", feature, "", build_rows(item, root), "", "**Where it pays**", "",
                  "| Engine module | Calls through | Max calls/frame | Median calls/frame |", "|---|---|---:|---:|"]
        modules = callers(root, key)
        for module, functions in sorted(modules.items()):
            values = counts.get((key, module))
            high, median = (f"{value:.2f}" for value in values) if values else ("-", "-")
            spelling = ", ".join(f"`{name}`" for name in sorted(functions))
            lines.append(f"| {module} | {spelling} | {high} | {median} |")
        if not modules:
            lines += ["", "No production callers found."]
        lines.append("")
    return "\n".join(lines)


def parse_sizes(text):
    result = {}
    seen = set()
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise ValueError(f"expected four size fields: {line}")
        key, symbol, size, region = fields
        if (not re.fullmatch(r"\w+", key) or not bench.CALL_SYMBOL.fullmatch(symbol)
                or not (size.isdecimal() or size == "-" and region == "ROM") or region not in ("flash", "IRAM", "rodata", "ROM") or (key, symbol) in seen):
            raise ValueError(f"invalid or duplicate size record: {line}")
        seen.add((key, symbol))
        result.setdefault(key, []).append((symbol, int(size) if size != "-" else None, region))
    return result


def elf_sizes(rows, fpu, located, sections=(), root=ROOT):
    owners = {item.fast: item for item in annotations(root)}
    lines = ["# row\tsymbol\tbytes\tregion; sizes belong to the --from-elf build"]
    for key, *_ in rows:
        calls = fpu[key].partition("calls ")[2].split(", ")
        names = [bench.ROW_SYMBOL_PREFIX + key, *(name for name in calls if name and not name.startswith("*"))]
        if key in owners:
            names.extend(tables_for(owners[key], root))
        for name in sorted(set(names)):
            matches = [record for record in located if record.name == name or record.name.startswith(name + ".")]
            if not matches:
                raise ValueError(f"size symbol missing from ELF: {name}")
            letters = {record.section.upper() for record in matches}
            if letters <= {"A"}:
                region = "ROM"
            elif letters <= {"R"}:
                region = "rodata"
            else:
                code_sections = {section for record in matches
                          for section, begin, size in sections if begin <= record.address < begin + size}
                if ".flash.rodata" in code_sections:
                    region = "rodata"
                elif code_sections and all("iram" in section for section in code_sections):
                    region = "IRAM"
                elif code_sections and all(section.startswith(".flash") for section in code_sections):
                    region = "flash"
                else:
                    raise ValueError(f"unknown ELF code section: {name}: {code_sections}")
            size = "-" if any(record.size is None for record in matches) else str(sum(record.size for record in matches))
            lines.append(f"{key}\t{name}\t{size}\t{region}")
    return "\n".join(lines) + "\n"


def rekey_errors(items, rows, errors):
    labels = {key: (label, kind) for key, label, kind, _, family in rows}
    result = dict(errors)
    for item in items:
        identity = item.key, item.kind
        if identity in errors:
            result[labels[item.fast]] = errors[identity]
            if identity not in labels.values():
                result.pop(identity, None)
    return result


def update_links(root, items, check=False):
    groups = {"Scalar": [], "Linear": []}
    for item in items:
        for page in reference_pages(item, root):
            link = f"[{item.key}](Fast-Paths.md#{item.key.replace('_', '-')})"
            if link not in groups.setdefault(page, []):
                groups[page].append(link)
    groups["README"] = ["[Fast-path evidence](Fast-Paths.md)"]
    for page, links in groups.items():
        path = root / "docs/math" / (page + ".md")
        changed = replace_block(path, "math-fast-links-" + page.lower(), (", ".join(links) + " pairs costs, bounds and callers." if page == "README" else "Fast-path evidence: " + ", ".join(links) + "."), check=check, command=COMMAND)
        if check and changed:
            raise ValueError(f"{page}.md fast-path links are stale")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--from-elf", type=Path)
    parser.add_argument("--objdump")
    parser.add_argument("--nm")
    args = parser.parse_args(argv)
    root = args.root
    try:
        items = annotations(root)
        rows, fpu = bench.read_fpu(root / "docs/math/data/math_bench_fpu_ops.txt")
        if args.from_elf:
            import hashlib
            from code_layout import find_tool, run, symbols
            located = symbols(args.nm or find_tool("nm"), args.from_elf, records=True)
            dump = run(args.objdump or find_tool("objdump"), "-h", str(args.from_elf))
            sections = [(name, int(address, 16), int(size, 16)) for name, size, address in re.findall(
                r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)", dump, re.M)]
            body = elf_sizes(rows, fpu, located, sections, root)
            provenance = "# ELF sha256=" + hashlib.sha256(args.from_elf.read_bytes()).hexdigest() + "\n"
            (root / "docs/math/data/math_bench_sizes.txt").write_bytes((provenance + body).encode())
        capture = (root / "docs/math/data/math_bench_capture.txt").read_text(encoding="utf-8")
        errors, bins = validate(items, rows, capture)
        build, measured = bench.parse_capture(capture, rows, allow_missing=True)
        benchmark_body = bench.render_table(rows, measured, bench.load_baseline(root / "launcher/tools/quality/icount_baseline.txt"),
                                            fpu, build, rekey_errors(items, rows, errors))
        benchmark_changed = replace_block(root / "docs/math/Benchmark.md", "math-benchmark", benchmark_body,
                                          check=args.check, command=COMMAND)
        if args.check and benchmark_changed:
            raise ValueError("benchmark generated table is stale")
        validate_hardware(root, items, fpu)
        update_links(root, items, check=args.check)
        _, measured = bench.parse_capture(capture, rows, allow_missing=not bins)
        size_path = root / "docs/math/data/math_bench_sizes.txt"
        sizes = parse_sizes(size_path.read_text(encoding="utf-8")) if size_path.exists() else {}
        count_path = root / "docs/math/data/math_calls_capture.txt"
        counts = parse_calls(count_path.read_text(encoding="utf-8")) if count_path.exists() else {}
        page = root / DOCUMENT
        page_text = page.read_text(encoding="utf-8")
        section_order = re.findall(r"^## (\w+)\s*$", page_text, re.M)
        if section_order:
            if args.check:
                validate_sections(page_text, items)
            items.sort(key=lambda item: section_order.index(item.key) if item.key in section_order else len(section_order))
        elif args.check:
            validate_sections(page_text, items)
        target = bench.parse_target(capture)
        if bins and not target:
            raise ValueError("error bins require a hardware MATHTARGET capture record")
        body = render(root, items, rows, measured if target else {}, errors if target else {}, bins, sizes, counts,
                      fpu, target or "-")
        changed = replace_block(page, BLOCK, body, check=args.check, command=COMMAND)
        if args.check and changed:
            raise ValueError("fast-path generated sections are stale")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"fast_paths: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
