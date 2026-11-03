#!/usr/bin/env python3
"""Generate the fast-path reference from owner annotations and committed captures."""
import argparse
from dataclasses import dataclass
import math
from functools import lru_cache
from pathlib import Path
import re
import sys

import math_bench_table as bench
from generated_blocks import replace_block

ROOT = bench.ROOT
HEADERS = Path("launcher/packages")
DOCUMENT = Path("docs/packages/Fast-Paths.md")
BLOCK = "package-fast-paths"
HOST_CAPTURE = Path("docs/math/data/math_error_host_capture.txt")
COMMAND = "python launcher/tools/perf/fast_paths.py --check"
ANNOTATION = re.compile(r"^/\* FAST_PATH\(\s*(\w+),\s*(\w+),\s*(\w+)(?:=([^ )]+) (ulp|relative|lsb|bit))?\) \*/", re.M)
FUNCTION = re.compile(r"^[ \t]*(?!(?:return|if|for|while|switch|case|else)\b)(?:(?:static[ \t]+)?(?:inline[ \t]+)?[\w*]+[ \t\n]+)(\w+)[ \t]*\([^;{}]*\)[ \t]*\{", re.M)
MEASUREMENT_SIGNIFICANT_DIGITS = 2
BIN_EDGE_DECIMAL_PLACES = 2
TARGET_ULONG_BITS = 32
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
    original: str
    coverage: str
    citations: tuple = ()
    bits: int | None = None
    period_bins: bool = False

    @property
    def anchor(self):
        return self.key.replace("_", "-")


def benchmark_links(items, rows):
    links = {}
    for item in items:
        keys = {item.fast, item.plain}
        keys.update(key for key, label, kind, _, _ in rows if (label, kind) == (item.key, item.kind))
        for key in keys:
            # A shared plain row belongs to the first section in page order.
            links.setdefault(key, f"../packages/Fast-Paths.md#{item.anchor}")
    return links


def package_headers(root):
    return sorted(path for include in (root / HEADERS).glob("*/include") for path in include.rglob("*.h"))


def annotations(root=ROOT):
    sys.path.insert(0, str(ROOT / "scripts/gates"))
    from check_doc_citations import citation_rows
    rows, problems = citation_rows(root)
    if problems:
        raise ValueError("; ".join(problems))
    citation_names = {row[1]: key for key, row in rows.items() if row[1]}
    result = []
    for header in package_headers(root):
        text = header.read_text(encoding="utf-8")
        matches = list(ANNOTATION.finditer(text))
        if len(matches) != len(re.findall(r"^/\* FAST_PATH\(", text, re.M)):
            raise ValueError(f"malformed fast-path annotation: {header}")
        package = root / HEADERS / header.relative_to(root / HEADERS).parts[0]
        source = "\n".join(path.read_text(encoding="utf-8") for path in sorted((package / "tests").glob("*.c"))) if matches else ""
        contracts = {name: (unit, method) for name, unit, method in re.findall(
            r'(?:MATH|FAST)ERROR name=(\w+) type=\w+ max=.*? unit=(ulp|relative|lsb|bit) method=(proof|sweep)', source)}
        for match in matches:
            fast, plain, limit_name, literal, literal_unit = match.groups()
            following = FUNCTION.search(re.sub(r"__attribute__\(\(.*?\)\)", "", text[match.end():]))
            if not following:
                raise ValueError(f"missing function below {fast}")
            key, kind = following[1], fast.rsplit("_", 1)[-1]
            if key not in contracts:
                raise ValueError(f"missing MATHERROR/FASTERROR row for {fast}: {key}")
            definition_text = text + "\n" + source if literal is not None else text
            define = re.search(r"^#define\s+" + limit_name + r"[ \t]+([^\n]+)$", definition_text, re.M)
            if not define:
                raise ValueError(f"missing limit {limit_name}: {header}")
            value = define[1].strip().removesuffix("F").removesuffix("f")
            bits_expression = re.fullmatch(r"\(1\.0F / \(float\)\(1UL << (\w+_BITS)\)\)", value)
            bits = None
            if bits_expression:
                owner = re.search(r"^#define\s+" + bits_expression[1] + r"[ \t]+([0-9]+)$", definition_text, re.M)
                if not owner or not 0 <= int(owner[1]) < TARGET_ULONG_BITS:
                    raise ValueError(f"invalid bits owner: {bits_expression[1]}")
                bits = int(owner[1])
                limit = 2.0 ** -bits
            else:
                limit = bench.error_number(value)
            if not math.isfinite(limit) or limit < 0:
                raise ValueError(f"invalid limit {limit_name}")
            if literal is not None and (float(literal) != limit or literal_unit != contracts[key][0]):
                raise ValueError(f"annotation/test limit mismatch: {limit_name}")
            if literal_unit == "bit":
                result.append(Annotation(key, "bits", fast, plain, limit_name, "bit", contracts[key][1],
                                         "portable", header.relative_to(root), text[:match.start()].count("\n") + 1,
                                         limit, "", "", "", ""))
                continue
            before = text[:match.start()]
            comments = [" ".join(re.sub(r"^\s*\* ?", "", body, flags=re.M).split())
                        for body in re.findall(r"/\*(.*?)\*/", before, re.S) if body.count("|") == 2]
            if not comments:
                raise ValueError(f"missing description, formula or coverage: {key}")
            description, formula, coverage = map(str.strip, comments[-1].split("|"))
            exact = re.search(r"/\* exact: (.*?) \*/\s*$", before)
            if not exact:
                raise ValueError(f"missing exact formula: {key}")
            cited = re.search(r"/\* source: (.*?) \*/\s*/\* exact: .*? \*/\s*$", before)
            names = [name.strip() for name in cited[1].split(";")] if cited else []
            for name in names:
                if name not in citation_names:
                    raise ValueError(f"unknown fast-path source: {name}")
            citations = tuple(citation_names[name] for name in names)
            body = function_bodies(text)[key]
            guard = re.search(r"^#(?:if|elif).*defined\(__XTENSA__\).*CONFIG_MATH_.*$", body, re.M)
            target = "portable"
            if guard:
                target = "portable; Xtensa needs fused" if "!defined(__XTENSA__)" in guard[0] else "Xtensa"
            unit, method = contracts[key]
            result.append(Annotation(key, kind, fast, plain, limit_name, unit, method, target,
                                     header.relative_to(root), before.count("\n") + 1,
                                     limit, description, formula, exact[1], coverage, citations, bits,
                                     bool(re.search(r'MATHERRORBIN name=' + re.escape(key)
                                                    + r' type=\w+ .*axis=period', source))))
    if not result:
        raise ValueError("no fast-path annotations")
    return result


def validate_sections(text, items):
    sections = re.findall(r"^## `?(\w+)`?(?: <sup>.*?</sup>)*\s*$", text, re.M)
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
        if item.target != "Xtensa":
            continue
        _, expected = hardware_features(root, item)
        observed = set(fpu[item.fast].partition(";")[0].split())
        if not expected or not expected & observed:
            raise ValueError(f"{item.fast}: target wrapper and callees lack an expected seed/fused instruction ({sorted(expected)})")


def reference_pages(item, root=ROOT):
    pages = {item.header.parent.name.capitalize()}
    functions, owners = package_functions(root, with_owners=True)
    source = (root / "launcher/packages/math/bench/suite_math_perf.c").read_text(encoding="utf-8")
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
            if not path.exists() or f"../packages/Fast-Paths.md#{item.anchor}" not in path.read_text(encoding="utf-8"):
                raise ValueError(f"{page}.md lacks link to section {item.key}")


def validate(items, rows, capture):
    manifest = {row[0]: row for row in rows}
    errors, bins = bench.parse_errors(capture), bench.parse_bins(capture)
    axes = bench.parse_bin_axes(capture)
    identities = {(label, kind) for _, label, kind, _, _ in rows}
    identities.update((item.key, item.kind) for item in items)
    unmatched = errors.keys() - identities
    if unmatched:
        raise ValueError(f"error measurements match no row: {sorted(unmatched)}")
    for item in items:
        for key in (item.fast, item.plain):
            if key not in manifest:
                raise ValueError(f"annotation row missing: {key}")
        identity = (item.key, item.kind)
        if item.unit == "relative" and item.period_bins and identity in errors:
            worst_bin_line(bins.get(identity, []))
        if identity in bins and identity not in errors:
            raise ValueError(f"missing error summary for bins: {identity}")
        if identity in errors:
            maximum, unit, method = re.fullmatch(r"(\S+) (\w+) \((\w+)\)", errors[identity]).groups()
            if unit != item.unit or method != item.method or any(row[2] != unit for row in bins.get(identity, [])):
                raise ValueError(f"error contract mismatch: {identity}")
            fractional = item.unit == "ulp" and axes.get(identity) == "period"
            checked_bins = [] if fractional else bins.get(identity, [])
            if max([float(maximum), *(row[1] for row in checked_bins)]) > item.limit:
                raise ValueError(f"measured error exceeds {item.limit_name}: {identity}")
            if max((row[1] for row in checked_bins), default=0) > float(maximum):
                raise ValueError(f"bin exceeds error summary: {identity}")
    return errors, bins


def error_evidence(root, items, rows, capture):
    errors, bins = validate(items, rows, capture)
    sources = dict.fromkeys(errors, "hardware")
    host = root / HOST_CAPTURE
    if host.exists():
        text = host.read_text(encoding="utf-8")
        if not bench.parse_target(text, execution="host"):
            raise ValueError("host errors require a target record (MATHTARGET)")
        host_errors, host_bins = validate(items, rows, text)
        if errors.keys() & host_errors.keys() or bins.keys() & host_bins.keys():
            raise ValueError("duplicate hardware and host error evidence")
        errors.update(host_errors)
        bins.update(host_bins)
        sources.update(dict.fromkeys(host_errors, "host"))
    return errors, bins, sources


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
    from check_package_docs import HEADER_CALL, TEMPLATE_MACRO, MACRO_CALL, split_args
    macros, headers, functions, owners = {}, [], {}, {}
    for path in package_headers(root):
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
    functions, owners = package_functions(root, with_owners=True)
    owner = owners.get(key)
    owner_package = root / HEADERS / owner.relative_to(root / HEADERS).parts[0] if owner else None
    reachable = {key}
    while True:
        expanded = reachable | {name for name, body in functions.items() if set(CALL.findall(body)) & reachable}
        if expanded == reachable:
            break
        reachable = expanded
    modules = {}
    sources = [path for folder in ("main", "packages") for path in (root / "launcher" / folder).rglob("*")]
    for path in sorted(sources):
        if (path.suffix not in (".c", ".h") or (owner_package is not None and path.is_relative_to(owner_package))
                or any(part in ("tests", "tools", "bench") for part in path.parts)):
            continue
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
        used = set(CALL.findall(text)) & reachable
        if used:
            relative = path.relative_to(root / "launcher")
            module = relative.parts[1] if len(relative.parts) > 2 else relative.parts[0]
            modules.setdefault("apps (not named)" if relative.parts[:2] == ("main", "apps") else module, set()).update(used)
    return modules


def build_rows(item, root):
    symbols, _ = hardware_features(root, item)
    qemu = (root / "launcher/sdkconfig.defaults.qemu").read_text(encoding="utf-8")
    disabled = any(re.search(r"^# " + symbol + r" is not set$|^" + symbol + r"=n$", qemu, re.M) for symbol in symbols)
    gates = ", ".join(f"`{symbol}`" for symbol in symbols) or "none"
    return f"Builds: {gates}; QEMU {'plain' if disabled else 'fast'}; host {'plain' if item.target == 'Xtensa' else 'fast'}."


def _description_fields(item):
    inputs, algorithm = re.split(r", (?=[a-z])", item.description.removeprefix("For "), maxsplit=1)
    symbols = re.findall(r"(?:reciprocal(?:-root)? seed|reciprocal root|fused remainder) [a-z]\b", algorithm)
    if "; " in algorithm:
        return algorithm.split(" samples", 1)[0] + " interpolation", inputs + "; " + algorithm.removesuffix(".")
    algorithm = re.sub(r" (?:y|r)\b| give reciprocal root y", "", algorithm).removesuffix(".")
    return algorithm, "; ".join([inputs, *symbols])


def _size_cell(row, sizes):
    records = sizes.get(row, [])
    regions = {}
    for _, size, region in records:
        if region != "rodata":
            regions.setdefault(region, []).append(size)
    parts = ["ROM" if region == "ROM" else f"{sum(values)} {region}"
             for region, values in sorted(regions.items())]
    return " + ".join(parts) or "-"


def _inline_expressions(text):
    return re.sub(r"\[[^]\n]+[)\]]|\b(?:[a-z]|len|sinf|MATH\w+)\b", lambda match: f"`{match[0]}`", text)


def plain_expression(item, root):
    text = (root / "launcher/packages/math/bench/suite_math_perf.c").read_text(encoding="utf-8")
    match = re.search(r"/\* MATH_PLAIN\(" + item.plain + r"\): (.*?) \*/", text)
    if not match:
        raise ValueError(f"plain row has no C description: {item.plain}")
    expression = match[1].split(", adjusted", 1)[0]
    return expression if "isqrt64" in expression else f"`{expression}`"


def format_limit(value):
    fraction, exponent = math.frexp(value)
    return f"2^{exponent - 1}" if fraction == 0.5 and value < 1 else f"{value:g}"


def limit_label(item):
    unit = ("step" if item.limit == 1 else "steps") if item.unit == "ulp" else "rel" if item.unit == "relative" else item.unit
    bits = f", {item.bits} bits" if item.bits is not None else ""
    return f"{format_limit(item.limit)} {unit}{bits}"


def worst_bin_line(points):
    if len(points) < 2:
        raise ValueError("missing period bins for relative error")
    points = sorted(points)
    worst = max(range(len(points)), key=lambda index: points[index][1])
    centre, maximum, _ = points[worst]
    left = (points[worst - 1][0] + centre) / 2 if worst else centre - (points[1][0] - centre) / 2
    right = (centre + points[worst + 1][0]) / 2 if worst + 1 < len(points) else centre + (centre - points[-2][0]) / 2
    other = max(value for index, (_, value, _) in enumerate(points) if index != worst)
    return (f"Worst bin: mantissa {left:.{BIN_EDGE_DECIMAL_PLACES}f}-{right:.{BIN_EDGE_DECIMAL_PLACES}f} "
            f"at {format_measurement(maximum)}; every other bin <= {ceil_measurement(other)}.")


def ceil_measurement(value):
    """Rounds up to the measurement digits, so a stated bound is never below the value."""
    if value == 0:
        return "0"
    digits = MEASUREMENT_SIGNIFICANT_DIGITS - 1 - math.floor(math.log10(value))
    return format_measurement(math.ceil(value * 10 ** digits) / 10 ** digits)


def format_measurement(value):
    rounded = f"{value:.{MEASUREMENT_SIGNIFICANT_DIGITS}g}"
    mantissa, separator, exponent = rounded.partition("e")
    if separator:
        return f"{float(rounded):.0f}" if int(exponent) >= 0 else f"{mantissa}e{int(exponent)}"
    return rounded


def format_error(evidence, source):
    if evidence == "-":
        return evidence
    value, unit, *_ = evidence.split()
    unit = ("step" if float(value) == 1 else "steps") if unit == "ulp" else "rel" if unit == "relative" else unit
    origin = " (host)" if source == "host" else ""
    return f"{format_measurement(float(value))} {unit}{origin}"


def render(root, items, rows, measured, errors, bins, sizes, fpu, target, sources=None, input_counts=None):
    sys.path.insert(0, str(ROOT / "scripts/gates"))
    from check_doc_citations import shown_link
    groups = {}
    for item in items:
        groups.setdefault(item.key, []).append(item)
    lines = [f"Measured target: {target}.", ""]
    replacements, costs, builds = [], [], []
    entry_details = {}
    for item in items:
        link = f"[`{item.key}`](#{item.anchor})"
        if len(groups[item.key]) > 1:
            link += f" ({item.fast.split('_', 1)[0]})"
        features = set(fpu[item.fast].partition(";")[0].split()) & set(bench.FPU_OPS)
        if item.target.startswith("portable;"):
            features &= {"madd.s", "msub.s"}
        operations = ", ".join(f"`{op}`" for op in sorted(features, key=lambda op: (not op.endswith("0.s"), op)))
        hardware = item.target
        if operations and item.target != "portable":
            hardware += (": " if item.target == "Xtensa" else " ") + operations
        expression = plain_expression(item, root)
        short_expression = "isqrt64 normalization" if "isqrt64" in expression else expression
        if "quaternion" in expression:
            short_expression = "blend + " + short_expression
        replacements.append([link, short_expression, _description_fields(item)[0]])
        fast, plain = measured.get(item.fast), measured.get(item.plain)
        warm_fast, cold_fast = [format_measurement(value) for value in fast[:2]] if fast else ["-", "-"]
        warm_plain, cold_plain = [format_measurement(value) for value in plain[:2]] if plain else ["-", "-"]
        ratio = format_measurement(plain[0] / fast[0]) if fast and plain else "-"
        identity = item.key, item.kind
        error = format_error(errors.get(identity, "-"), (sources or {}).get(identity))
        costs.append([link, f"{warm_fast} / {warm_plain} ({ratio}×)", f"{cold_fast} / {cold_plain}", error])
        table_bytes = sum(size for _, size, region in sizes.get(item.fast, []) if region == "rodata")
        modules = ", ".join(f"`{module}`" for module in sorted(callers(root, item.key))) or "none"
        builds.append([link, _size_cell(item.fast, sizes), _size_cell(item.plain, sizes), str(table_bytes) if table_bytes else "-"])
        entry_details[item.fast] = (hardware, expression, modules)
    for title, headings, entries in (
        ("What it replaces", "Fast path | Plain way | Ours", replacements),
        ("Cost and error", "Fast path | Warm cycles: fast / plain (×) | Cold cycles: fast / plain | Max error", costs),
        ("Size and builds", "Fast path | Fast code bytes | Plain code bytes | Table bytes", builds),
    ):
        lines += [f"### {title}", "", "| " + headings + " |", "|" + "---|" * len(headings.split(" | "))]
        lines.extend("| " + " | ".join(cells) + " |" for cells in entries)
        lines.append("")
    lines.pop()
    for key, group in groups.items():
        item = group[0]
        citations = "".join(" " + shown_link(root, key) for key in dict.fromkeys(
            key for row in group for key in row.citations))
        lines += ["", f'<a id="{item.anchor}"></a>', "", f"## `{key}`{citations}", "",
                  "| Original | Fast path |", "|---|---|",
                  f"| $`{item.original}`$ | $`{item.formula}`$ |", "", f"Symbols: {_inline_expressions(_description_fields(item)[1])}.", "",
                  f"{item.method.capitalize()}: {_inline_expressions(item.coverage)}.", ""]
        hardware, _, modules = entry_details[item.fast]
        lines += [f"Target: {hardware}.", "", build_rows(item, root), "", f"Callers: {modules}.", ""]
        for row in group:
            _, expression, _ = entry_details[row.fast]
            lines += [f"Bench rows `{row.fast}` vs `{row.plain}`; limit `{row.limit_name}` "
                      f"({limit_label(row)}); {row.method}.", "",
                      f"Plain way: {expression}.", ""]
        identity = key, item.kind
        if item.unit == "relative" and item.period_bins and identity in errors:
            lines += [worst_bin_line(bins.get(identity, [])), ""]
        if identity in errors and float(errors[identity].split()[0]) == 0:
            count = (input_counts or {}).get(identity)
            sweep = f"{count} inputs of the sweep" if count is not None else "the sweep"
            lines += [f"Exact: no error over {sweep}."]
            continue
        lines += [f"![Error across the input range](../images/math/fast-path-{key}.png)" if (key, item.kind) in bins
                  else "Error curve: - (capture pending)."]
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
        # A table reachable only through a branch this build compiled out (#if) is not linked.
        tables = set(tables_for(owners[key], root)) if key in owners else set()
        for name in sorted(set(names) | tables):
            matches = [record for record in located if record.name == name or record.name.startswith(name + ".")]
            if not matches and name in tables:
                continue
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


def update_links(root, items, check=False):
    groups = {"Scalar": [], "Linear": []}
    for item in items:
        for page in reference_pages(item, root):
            link = f"[{item.key}](../packages/Fast-Paths.md#{item.anchor})"
            if link not in groups.setdefault(page, []):
                groups[page].append(link)
    groups["README"] = ["[Fast-path evidence](../packages/Fast-Paths.md)"]
    for page, links in groups.items():
        path = root / "docs/math" / (page + ".md")
        changed = replace_block(path, "math-fast-links-" + page.lower(), (", ".join(links) + " pairs costs, bounds and callers." if page == "README" else "Fast-path evidence: " + ", ".join(links) + "."), check=check, command=COMMAND)
        if check and changed:
            raise ValueError(f"{page}.md fast-path links are stale")


def render_exact(root, items):
    lines = [""] if items else []
    for item in items:
        package = item.header.relative_to(HEADERS).parts[0]
        modules = ", ".join(f"`{module}`" for module in sorted(callers(root, item.key))) or "none"
        lines += ["", f'<a id="{item.anchor}"></a>', "", f"## `{item.key}`", "",
                  "| Fast path | Reference | Limit |", "|---|---|---|",
                  f"| `{item.fast}` | `{item.plain}` | `{item.limit_name}` = {limit_label(item)} |", "",
                  f"Contract and build choices: [{package} package](../../launcher/packages/{package}/README.md).", "",
                  f"Callers: {modules}.", "",
                  f"Correctness: [{package} tests](../../launcher/packages/{package}/tests); {item.method}."]
    return "\n".join(lines)


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
        all_items = annotations(root)
        items = [item for item in all_items if item.unit != "bit"]
        exact_items = [item for item in all_items if item.unit == "bit"]
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
        errors, bins, sources = error_evidence(root, items, rows, capture)
        displayed_errors = {
            key: re.sub(r"(\S+) ulp", lambda m: m[1] + (" step" if float(m[1]) == 1 else " steps"), value) + (" (host)" if sources[key] == "host" else "")
            for key, value in errors.items()}
        _, measured = bench.parse_capture(capture, rows, allow_missing=not bins)
        page = root / DOCUMENT
        page_text = page.read_text(encoding="utf-8")
        section_order = re.findall(r"^## `?(\w+)`?(?: <sup>.*?</sup>)*\s*$", page_text, re.M)
        if section_order:
            if args.check:
                validate_sections(page_text, all_items)
            items.sort(key=lambda item: section_order.index(item.key) if item.key in section_order else len(section_order))
        elif args.check:
            validate_sections(page_text, all_items)
        links = benchmark_links(items, rows)
        # Fast-Paths renders the error of every linked row except the plain references.
        plain = {item.plain for item in items}
        error_links = {key: link for key, link in links.items() if key not in plain}
        benchmark_body = bench.render_table(rows, measured, bench.load_baseline(root / "launcher/tools/quality/icount_baseline.txt"),
                                            fpu, links=links, errors=displayed_errors, error_links=error_links)
        benchmark_changed = replace_block(root / "docs/math/Benchmark.md", "math-benchmark", benchmark_body,
                                          check=args.check, command=COMMAND)
        if args.check and benchmark_changed:
            raise ValueError("benchmark generated table is stale")
        validate_hardware(root, items, fpu)
        update_links(root, items, check=args.check)
        size_path = root / "docs/math/data/math_bench_sizes.txt"
        sizes = parse_sizes(size_path.read_text(encoding="utf-8")) if size_path.exists() else {}
        target = bench.parse_target(capture)
        if bins and not target:
            raise ValueError("error bins require a hardware MATHTARGET capture record")
        body = render(root, items, rows, measured if target else {}, errors if target else {}, bins, sizes,
                      fpu, target or "-", sources, bench.parse_input_counts(capture))
        body += render_exact(root, exact_items)
        changed = replace_block(page, BLOCK, body, check=args.check, command=COMMAND)
        if args.check and changed:
            raise ValueError("fast-path generated sections are stale")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"fast_paths: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
