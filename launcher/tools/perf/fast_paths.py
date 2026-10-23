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
TARGETS = {"MATH_TARGET_ESP32S3": "ESP32-S3 / Xtensa LX7", "MATH_TARGET_PORTABLE": "Portable C"}
ANNOTATION = re.compile(r"^/\* MATH_FAST_PATH\(\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(ulp|relative|lsb),\s*(proof|sweep),\s*(\w+)\) \*/", re.M)
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
    target_name: str
    target: str
    header: Path
    line: int
    limit: float
    description: str
    formula: str
    coverage: str


def annotations(root=ROOT):
    targets = TARGETS
    result = []
    for header in sorted((root / HEADERS).rglob("*.h")):
        text = header.read_text(encoding="utf-8")
        matches = list(ANNOTATION.finditer(text))
        declarations = re.findall(r"^/\* MATH_FAST_PATH\(", text, re.M)
        if len(matches) != len(declarations):
            raise ValueError(f"malformed fast-path annotation: {header}")
        for index, match in enumerate(matches):
            key, kind, fast, plain, limit_name, unit, method, target_name = match.groups()
            if target_name not in targets:
                raise ValueError(f"unknown fast-path target: {target_name}")
            define = re.search(r"^#define\s+" + limit_name + r"[ \t]+([^\n]+)$", text, re.M)
            if not define:
                raise ValueError(f"missing limit {limit_name}: {header}")
            value = define[1].removesuffix("F").removesuffix("f")
            limit = float.fromhex(value) if value.lower().startswith("0x") else float(value)
            if not math.isfinite(limit) or limit < 0:
                raise ValueError(f"invalid limit {limit_name}")
            before = text[:match.start()]
            if not before.rstrip().endswith("*/"):
                previous = matches[index - 1] if index else None
                if previous is None or previous[1] != key or text[previous.end():match.start()].strip():
                    raise ValueError(f"missing adjacent fast-path description: {key}")
            # Adjacent row pairs share the owner's description.
            comment = [" ".join(re.sub(r"^\s*\* ?", "", body, flags=re.M).split())
                       for body in re.findall(r"/\*(.*?)\*/", before, re.S) if body.count("|") == 2]
            if not comment:
                raise ValueError(f"missing description, formula or coverage: {key}")
            description, formula, coverage = map(str.strip, comment[-1].split("|"))
            result.append(Annotation(key, kind, fast, plain, limit_name, unit, method, target_name, targets[target_name],
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
        if item.target_name == "MATH_TARGET_PORTABLE":
            continue
        _, expected = hardware_features(root, item)
        observed = set(fpu[item.fast].partition(";")[0].split())
        if not expected or not expected & observed:
            raise ValueError(f"{item.fast}: target wrapper lacks an expected seed/fused instruction ({sorted(expected)})")


def reference_pages(item):
    pages = {item.header.parent.name.capitalize()}
    if item.fast.startswith(("normalize_", "nlerp_")):
        pages.add("Linear")
    return pages


def validate_links(root, items):
    for item in items:
        for page in reference_pages(item):
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
        if bins and (identity not in errors or identity not in bins):
            raise ValueError(f"missing error summary or bins: {identity}")
        if identity in errors:
            if bins and identity not in bins:
                raise ValueError(f"missing error summary or bins: {identity}")
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
def package_functions(root):
    sys.path.insert(0, str(ROOT / "scripts/gates"))
    from check_math_docs import HEADER_CALL, TEMPLATE_MACRO, MACRO_CALL, split_args
    macros, headers, functions = {}, [], {}
    for path in sorted((root / HEADERS).rglob("*.h")):
        text = path.read_text(encoding="utf-8")
        if path.name.endswith("_template.h"):
            for name, params, body in TEMPLATE_MACRO.findall(text):
                macros[name] = (split_args(params), body.replace("\\\n", "\n"))
        else:
            headers.append(text)
            functions.update(function_bodies(text))

    def expand(name, arguments, active=()):
        if name not in macros or name in active:
            return
        params, body = macros[name]
        bind = dict(zip(params, arguments))
        for param, value in bind.items():
            body = re.sub(r"\b" + param + r"\b", lambda _: value, body)
        body = re.sub(r"\s*##\s*", "", body)
        functions.update(function_bodies(body))
        for callee, args in MACRO_CALL.findall(body):
            expand(callee, split_args(args), (*active, name))

    for text in headers:
        for name, args in HEADER_CALL.findall(text):
            expand(name, split_args(args))
    return functions


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
        if path.is_relative_to(root / HEADERS) or path.suffix not in (".c", ".h") or any(part in ("tests", "tools") for part in path.parts):
            continue
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
        used = set(CALL.findall(text)) & reachable
        if used:
            relative = path.relative_to(root / "launcher")
            module = relative.parts[1] if len(relative.parts) > 2 else relative.parts[0]
            modules.setdefault(module, set()).update(used)
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
            identity = key, module
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
    def compiled(xtensa, settings):
        active = [True]
        selected = []
        for line in body.splitlines():
            guard = re.match(r"#if (.*)", line)
            if guard:
                expression = guard[1].replace("defined(__XTENSA__)", str(xtensa))
                expression = re.sub(r"CONFIG_MATH_\w+", lambda m: str(settings.get(m[0], False)), expression)
                expression = expression.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
                if not re.fullmatch(r"[ TrueFalsandornt()]+", expression):
                    raise ValueError(f"unsupported build condition: {guard[1]}")
                active.append(active[-1] and eval(expression, {"__builtins__": {}}))
            elif line == "#else":
                active[-1] = active[-2] and not active[-1]
            elif line == "#endif":
                active.pop()
            elif active[-1]:
                selected.append(line.strip())
        expressions = [line for line in selected if line.startswith("return ") or "= fmaf(" in line or "= fmodf(" in line]
        return list(dict.fromkeys(expressions))
    qemu_settings = {symbol: bool(re.search(r"^" + symbol + r"=y$", qemu, re.M)) for symbol in symbols}
    lines += [f"Fast branch: `{' ; '.join(conditions) or 'all builds'}`."]
    if symbols:
        for target, xtensa, settings in (("Host (!defined(__XTENSA__))", False, {}), ("QEMU", True, qemu_settings)):
            expressions = compiled(xtensa, settings)
            lines += [f"{target} compiles:", *[f"- `{expression}`" for expression in expressions]]
    else:
        lines += [f"Host and QEMU compile the same `{item.key}` expression; no CONFIG_MATH_* branch selects it."]
    return "\n\n".join(lines)



def plain_expression(item, root):
    text = (root / "launcher/test/suites/suite_math_perf.c").read_text(encoding="utf-8")
    name, kind = item.plain.rsplit("_", 1)
    match = re.search(r"X\(" + name + r',\s*"[^"]+",\s*' + kind + r",\s*\w+,\s*\w+,\s*(.*?)\)\s*\\", text)
    if not match:
        raise ValueError(f"plain row has no kernel expression: {item.plain}")
    return re.sub(r"\s+", " ", match[1])


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
            lines += [f"{manifest[pair.plain][1]}:", "", "```c", plain_expression(pair, root), "```", ""]
        lines += ["**What we do**", "", item.description, "", "```math", item.formula, "```", "", f"Implementation target: {item.target}.", "", "**Cost**", "",
                  "| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |", "|---|---|---:|---:|---:|"]
        for pair in group:
            fast, plain = measured.get(pair.fast), measured.get(pair.plain)
            ratio = f"{plain[0] / fast[0]:.2f}" if fast and plain else "-"
            for row in (pair.fast, pair.plain):
                values = measured.get(row)
                warm, cold = (f"{value:.1f}" for value in values[:2]) if values else ("-", "-")
                lines.append(f"| `{row}` | {target if values else '-'} | {warm} | {cold} | {ratio if row == pair.fast else '-'} |")
        error = errors.get((key, item.kind), "-")
        lines += ["", "**Error**", "", "| Measured target | Maximum error | Limit | Unit | Method |",
                  "|---|---:|---|---|---|",
                  f"| {target if error != '-' else '-'} | {error.split()[0]} | `{item.limit_name}` ({item.limit:g}) | {item.unit} | {item.method} |",
                  "", f"{item.method.capitalize()}: {item.coverage}.", ""]
        if (key, item.kind) in bins:
            lines += [f"![Error across the input range](../images/math/fast-path-{key}.png)", ""]
        else:
            lines += ["Error curve: - (capture pending).", ""]
        lines += ["**Size**", "", "Table bytes total the linked copies of each referenced table.", "", "| Row | Code bytes / region | Callee bytes | Table bytes |", "|---|---|---|---|"]
        for pair in group:
            for row in (pair.fast, pair.plain):
                records = sizes.get(row, [])
                code = [f"{size if size is not None else '-'} / {region}" for name, size, region in records if name == bench.ROW_SYMBOL_PREFIX + row]
                callees = [f"`{name}`: {size if size is not None else '-'} / {region}" for name, size, region in records
                           if name != bench.ROW_SYMBOL_PREFIX + row and region != "rodata"]
                tables = [f"`{name}`: {size}" for name, size, region in records if region == "rodata"]
                lines.append(f"| `{row}` | {', '.join(code) or '-'} | {', '.join(callees) or '-'} | {', '.join(tables) or '-'} |")
        symbols, _ = hardware_features(root, item)
        observed = ", ".join(f"`{op}`" for op in fpu[item.fast].partition(";")[0].split() if op in bench.FPU_OPS)
        gates = ", ".join(f"`{symbol}`" for symbol in symbols)
        feature = f"ELF instructions: {observed or '-'}. Gating symbols: {gates or '-'}."
        lines += ["", "**Builds**", "", feature, "", build_rows(item, root), "", "**Where it pays**", "",
                  "| Engine module | Calls through | Max calls/frame | Median calls/frame |", "|---|---|---:|---:|"]
        modules = callers(root, key)
        for module, functions in sorted(modules.items()):
            values = counts.get((key, module))
            high, median = (f"{value:.2f}" for value in values) if values else ("-", "-")
            spelling = "-" if module == "apps" else ", ".join(f"`{name}`" for name in sorted(functions))
            lines.append(f"| {module} | {spelling} | {high} | {median} |")
        if not modules:
            lines.append("| - | - | - | - |")
        lines.append("")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    root = args.root
    try:
        items = annotations(root)
        rows, fpu = bench.read_fpu(root / "docs/math/data/math_bench_fpu_ops.txt")
        capture = (root / "docs/math/data/math_bench_capture.txt").read_text(encoding="utf-8")
        errors, bins = validate(items, rows, capture)
        validate_hardware(root, items, fpu)
        validate_links(root, items)
        _, measured = bench.parse_capture(capture, rows, allow_missing=not bins)
        size_path = root / "docs/math/data/math_bench_sizes.txt"
        sizes = bench.parse_sizes(size_path.read_text(encoding="utf-8")) if size_path.exists() else {}
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
        if target and any(item.target_name != "MATH_TARGET_PORTABLE" and item.target != target for item in items):
            raise ValueError("capture target disagrees with fast-path annotation")
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
