#!/usr/bin/env python3
"""Account for internal memory and enforce budgets at IRAM placement sites.

python launcher/tools/build/memory_map.py --source-only [--root ROOT]
python launcher/tools/build/memory_map.py --check [--require-pin] BUILD...
python launcher/tools/build/memory_map.py BUILD... --update-budgets (all pinned variants)
python launcher/tools/build/memory_map.py BUILD... --write
python launcher/tools/build/memory_map.py BUILD... --tables DIR
python launcher/tools/build/memory_map.py BUILD...  (print documentation tables)
Documentation modes require a release build.
Scratch experiments: launcher/tools/build/iram_what_if.py --help
"""
import argparse
from bisect import bisect_left
import fnmatch
import json
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

VARIANTS = ("release", "dev", "diag")
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from code_layout import LayoutError, find_tool, run, symbols
from doc_tables import markdown, parse_table, collapsed
import svg_bars
from compile_db import compile_flags
from generated_blocks import replace_block, blocks
from espressif import idf_python, idf_pin, idf_drift
from layout_pad import placed_sections
sys.path.insert(0, str(ROOT / "scripts/gates"))
from c_comments import scan

VARIANT_PATTERN = "|".join(map(re.escape, VARIANTS))
BUDGET = re.compile(r"IRAM budget: ((?:(?:" + VARIANT_PATTERN + r") )?\d+ B(?:, (?:(?:" + VARIANT_PATTERN + r") )?\d+ B)*)")
ATTRIBUTE = re.compile(r"\b(?:\w*IRAM_ATTR|\w*IRAM\w*_ATTR)\b|__attribute__\s*\(\(\s*section\s*\(\s*\"\.iram[^\"]*\"")
PLACEMENT = re.compile(r"\((?:noflash\w*|iram\w*)\)")
CONFIG_PLACEMENT = re.compile(r"CONFIG_\w*(?:(?:IN_IRAM|PLACE_\w*INTO_IRAM)\w*=y|PLACE_\w*INTO_FLASH=n)")
BLOCK = "internal-memory-map"
DOC = ROOT / "docs/memory/Internal-Memory-Map.md"
CHARTS = {"pool": "internal-pool.svg", "components": "iram-components.svg"}
REGENERATE = "python launcher/tools/build/memory_map.py launcher/build launcher/build.dev launcher/build.diag --write"
UPDATE_BUDGETS = REGENERATE.rsplit(" ", 1)[0] + " --update-budgets"


@dataclass
class Site:
    path: str
    line: int
    owner: str
    reason: str
    budget: int | dict | None
    archive: str
    entries: list | bool

    @property
    def label(self):
        return f"{self.path}:{self.line}"


@dataclass
class Input:
    archive: str
    object: str
    symbol: str
    size: int
    literal: bool = False


def preceding_comment(lines, index, kind):
    end = index
    if kind == "c":
        return next((comment.text for comment in scan(Path(), "\n".join(lines))
                     if comment.line + len(comment.raw_lines) == index + 1), "")
    else:
        while index and lines[index - 1].lstrip().startswith("#"):
            index -= 1
    return " ".join(re.sub(r"^\s*(?:/\*|\*|#)\s?|\*/", "", line).strip()
                    for line in lines[index:end])


def components(root, metadata=None):
    root = root.resolve()
    if metadata is None:
        launcher = root / "launcher"
        cmake = (launcher / "CMakeLists.txt").read_text(encoding="utf-8")
        declarations = re.findall(r"\bset\s*\(\s*EXTRA_COMPONENT_DIRS\b\s*([^)]*)\)", cmake, re.I)
        if len(declarations) > 1 or re.search(r"\blist\s*\(\s*APPEND\s+EXTRA_COMPONENT_DIRS\b", cmake, re.I):
            raise ValueError("unsupported repeated EXTRA_COMPONENT_DIRS declaration")
        extras = declarations[0] if declarations else None
        parents = [launcher / "components"]
        if extras:
            for entry in re.findall(r'"([^";]+)"|([^\s";]+)', extras):
                value = next(part for part in entry if part).replace("${CMAKE_CURRENT_LIST_DIR}", str(launcher))
                if "$" in value:
                    raise ValueError(f"unsupported EXTRA_COMPONENT_DIRS: {value}")
                parents.append(launcher / value)
        paths = [launcher / "main"] + [path for parent in parents for path in sorted(parent.glob("*")) if path.is_dir()]
        return {"lib" + path.name + ".a": path.resolve() for path in paths if path.resolve().is_relative_to(root)}
    return {"lib" + name + ".a": Path(path).resolve()
            for name, path in zip(metadata["build_components"], metadata["build_component_paths"])
            if name and Path(path).resolve().is_relative_to(root)
            and "managed_components" not in Path(path).parts}


def parse_budget(reason):
    match = BUDGET.search(reason)
    if not match:
        return None
    clause = match.group(1)
    forms = {len(part.split()) for part in clause.split(", ")}
    if len(forms) > 1:
        raise ValueError("mixed plain and per-variant IRAM budget syntax")
    if clause[0].isdigit():
        return int(clause.split()[0])
    entries = re.findall(r"(" + VARIANT_PATTERN + r") (\d+) B", clause)
    if len(dict(entries)) != len(entries):
        raise ValueError("duplicate IRAM budget variant")
    return {name: int(value) for name, value in entries}


def sites(root, owned=None):
    found = []
    owned = components(root) if owned is None else owned
    paths = sorted({p for base in owned.values() for p in base.rglob("*") if p.suffix in {".c", ".h", ".lf"}})
    paths += sorted((root / "launcher").glob("sdkconfig.defaults*"))
    attribute_aliases = set()
    for path in paths:
        if path.suffix in {".c", ".h"}:
            for alias, expression in re.findall(r"^#define\s+(\w+)\s+(.+)$", path.read_text(encoding="utf-8"), re.M):
                if ATTRIBUTE.search(expression):
                    attribute_aliases.add(alias)
    for path in paths:
        name = path.relative_to(root).as_posix()
        owner = "/".join(name.split("/")[:4]) + "/" if "/apps/" in name else "engine"
        lines = path.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            if path.suffix in {".c", ".h"}:
                if (not ATTRIBUTE.search(line) and not any(re.search(r"\b" + re.escape(alias) + r"\b", line)
                                                       for alias in attribute_aliases)) or line.lstrip().startswith(("//", "/*", "*", "#")):
                    continue
                declaration = " ".join(lines[index:index + 5]).split(";", 1)[0]
                declaration = re.sub(r"__attribute__\s*\(\(.*?\)\)", "", declaration)
                function = re.search(r"\b(\w+)\s*\([^;{}]*\)\s*\{", declaration)
                if not function:
                    raise ValueError(f"{name}:{index + 1}: unsupported IRAM declaration")
                entries = [(path.stem, function.group(1))]
                archive = next(archive for archive, directory in owned.items() if path.resolve().is_relative_to(directory.resolve()))
                kind = "c"
            elif path.suffix == ".lf":
                if not re.match(r"\[mapping:", line):
                    continue
                stop = next((i for i in range(index + 1, len(lines)) if lines[i].startswith("[")), len(lines))
                body = "\n".join(entry for entry in lines[index:stop] if not entry.lstrip().startswith("#"))
                if not PLACEMENT.search(body):
                    continue
                archive = re.search(r"archive:\s*(\S+)", body).group(1)
                entries = True
                kind = "hash"
            else:
                if not CONFIG_PLACEMENT.fullmatch(line):
                    continue
                archive, entries, kind = "", [], "hash"
            reason = preceding_comment(lines, index, kind)
            if not reason:
                raise ValueError(f"{name}:{index + 1}: missing IRAM reason")
            budget = parse_budget(reason)
            if entries and budget is None:
                raise ValueError(f"{name}:{index + 1}: missing IRAM budget")
            reason = BUDGET.sub("", reason).strip()
            if not re.search(r"[A-Za-z][^.?!]*[.?!]", reason) or len(re.findall(r"[A-Za-z]+", reason)) < 2 or re.search(r"\b(?:TODO|TBD|FIXME)\b", reason, re.I):
                raise ValueError(f"{name}:{index + 1}: missing IRAM reason beyond budget")
            found.append(Site(name, index + 1, owner, reason, budget, archive, entries))
    return found


def matches_site(item, site):
    return (fnmatch.fnmatch(item.archive, site.archive) and
            any(fnmatch.fnmatch(item.object.removesuffix(".obj").removesuffix(".o").removesuffix(".c"), obj)
                and fnmatch.fnmatch(item.symbol, symbol) for obj, symbol in site.entries))


def check(placements, inputs, variant, owned, active, idf_commit=None, pinned_commit=None, exact=True):
    errors = []
    drift = idf_drift(idf_commit, pinned_commit) if idf_commit is not None else None
    used = Counter()
    names = {}
    for item in inputs:
        matches = [site for site in placements if matches_site(item, site)]
        if len(matches) > 1:
            # A function attribute and a mapping may describe the same code; the function owns its budget.
            matches = sorted(matches, key=lambda site: site.path.endswith(".lf"))[:1]
        if not matches:
            if item.archive in owned and item.size:
                errors.append(f"{item.archive}({item.object}): {item.symbol}: IRAM symbol has no site")
            continue
        site = matches[0]
        used[site.label] += item.size
        names.setdefault(site.label, set()).add(item.symbol)
    report = []
    for site in placements:
        size = used[site.label]
        if site.budget is None:
            continue
        enabled = active(site)
        if not enabled:
            if size:
                errors.append(f"{site.label}: compiled-out site owns {size} B")
        else:
            if not size:
                errors.append(f"{site.label}: placed nothing in this build")
            if exact:
                budget = site.budget
                if isinstance(budget, dict):
                    if variant not in budget:
                        errors.append(f"{site.label}: missing {variant} IRAM budget")
                        continue
                    budget = budget[variant]
                if size != budget:
                    message = f"{site.label}: measured {size} B, budget {budget} B; set the budget to {size} B"
                    if drift:
                        print(f"NOTE IDF drift: {drift}: {message}")
                    else:
                        errors.append(f"{message}; run `{UPDATE_BUDGETS}`")
        report.append((site, size, sorted(names.get(site.label, []))))
    if errors:
        raise ValueError("\n".join(errors))
    return report

def preprocessed_lines(build, root, paths=None):
    alive = set()
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    for entry in commands:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        if not source.resolve().is_relative_to(root) or (paths is not None and source.resolve() not in paths):
            continue
        command = compile_flags(entry)
        output = subprocess.run([*command, "-E", entry["file"]], cwd=entry["directory"], check=True, capture_output=True, text=True).stdout
        path, number = None, 0
        for line in output.splitlines():
            marker = re.match(r'^# (\d+) "([^"]+)"', line)
            if marker:
                number = int(marker[1])
                path = Path(marker[2])
                if not path.is_absolute():
                    path = Path(entry["directory"]) / path
                path = path.resolve()
            else:
                if path and line.strip():
                    alive.add((path, number))
                number += 1
    return alive


def fragment_entries(site, root, config):
    lines = (root / site.path).read_text(encoding="utf-8").splitlines()
    stack, entries = [], []
    for line in lines[site.line:]:
        if line.startswith("["):
            break
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        branch = re.fullmatch(r"\s*(if|elif) (\w+) = (y|n):", line)
        otherwise = line.strip() == "else:"
        while stack and (indent < stack[-1][0] or (indent == stack[-1][0] and not (otherwise or branch and branch[1] == "elif"))):
            stack.pop()
        if branch:
            kind, name, value = branch.groups()
            condition = config.get("CONFIG_" + name, "n") == value
            if kind == "if":
                stack.append([indent, condition, condition])
            elif stack and stack[-1][0] == indent:
                stack[-1][1] = condition and not stack[-1][2]
                stack[-1][2] |= condition
            else:
                raise ValueError(f"{site.label}: elif without if")
        elif otherwise:
            if not stack or stack[-1][0] != indent:
                raise ValueError(f"{site.label}: else without if")
            stack[-1][1] = not stack[-1][2]
            stack[-1][2] = True
        elif re.match(r"\s*(?:if|elif)\b", line):
            raise ValueError(f"{site.label}: unsupported linker condition: {line.strip()}")
        elif PLACEMENT.search(line) and all(enabled for _, enabled, _ in stack):
            selector = line.strip().split()[0].split(":")
            entries.append((selector[0], selector[1] if len(selector) == 2 else "*"))
    return entries


def build_sites(root, build):
    placements = sites(root, build["owned"])
    c_sites = [root / site.path for site in placements if site.path.endswith((".c", ".h"))]
    paths = None if any(path.suffix == ".h" for path in c_sites) else set(c_sites)
    alive = preprocessed_lines(build["build"], root, paths)
    enabled = {}
    for site in placements:
        if site.path.endswith(".lf"):
            site.entries = fragment_entries(site, root, build["config"])
            enabled[site.label] = bool(site.entries)
        else:
            enabled[site.label] = (root / site.path, site.line) in alive
    return placements, lambda site: enabled[site.label]


def configuration(text):
    values = dict(re.findall(r"^(CONFIG_\w+)=(.*)$", text, re.M))
    values.update({name: "n" for name in re.findall(r"^# (CONFIG_\w+) is not set$", text, re.M)})
    return values


def rounding(script, end):
    region = re.search(r"\.iram0\.text_end(?:\s*\([^)]*\))?\s*:[^{]*\{([^}]*)\}", script)
    if not region:
        raise ValueError("linker script has no IRAM text_end region")
    split = region.group(1)
    alignment = re.search(r"\.\s*=\s*ALIGN\(([^)]+)\)", split)
    if not alignment:
        raise ValueError("linker script has no IRAM split alignment")
    def value(expression):
        try:
            return int(expression.strip(), 0)
        except ValueError:
            definition = re.search(r"\b" + re.escape(expression.strip()) + r"\s*=\s*(0x[\da-fA-F]+|\d+)\s*;", script)
            if not definition:
                raise ValueError(f"unresolved linker alignment: {expression}")
            return int(definition.group(1), 0)
    granularity = value(alignment.group(1))
    pad = re.search(r"\.\s*\+=\s*([^;]+);", split[:alignment.start()])
    prefetch = value(pad.group(1)) if pad else 0
    return granularity, prefetch, ((end + prefetch + granularity - 1) // granularity) * granularity - end


def map_inputs(text, elf_symbols):
    section = ""
    pending = ""
    inputs = []
    sections = {}
    constants = {}
    literals = []
    iram_names = {}
    addresses = sorted(elf_symbols)
    for line in text.splitlines():
        wrapped = re.match(r"^(\.\S+)\s*$", line)
        if wrapped:
            section = wrapped.group(1)
            pending = section
            continue
        output = re.match(r"^(\.\S+)\s+(0x[\da-f]+)\s+(0x[\da-f]+)", line)
        if output:
            section, address, size = output.groups()
            sections[section] = (int(address, 16), int(size, 16))
        if pending == section:
            output = re.match(r"\s+(0x[\da-f]+)\s+(0x[\da-f]+)\s*$", line)
            if output:
                sections[section] = tuple(int(value, 16) for value in output.groups())
        constant = re.match(r"\s+(0x[\da-f]+)\s+(_\w+)\s*=", line)
        if constant:
            constants[constant.group(2)] = int(constant.group(1), 16)
    for output, input_section, address, size, origin in placed_sections(text.splitlines()):
        if not output or not output.startswith(".iram"):
            continue
        match = re.search(r"([^/\\\s]+\.a)\(([^)]+)\)", origin)
        if not match:
            continue
        archive, obj = match.groups()
        if not size:
            continue
        if input_section.startswith(".iram") and input_section.endswith(".literal"):
            literals.append((archive, obj, input_section.removesuffix(".literal"), size))
            continue
        if input_section.startswith((".text.", ".literal.")):
            symbol = input_section.split(".", 2)[2]
            inputs.append(Input(archive, obj, symbol, size, input_section.startswith(".literal.")))
        else:
            members = [(name, count) for start in addresses[bisect_left(addresses, address):bisect_left(addresses, address + size)]
                       for name, count in [elf_symbols[start]] if count and not name.startswith("$")]
            if len(members) == 1:
                inputs.append(Input(archive, obj, members[0][0], size))
                iram_names[(archive, obj, input_section)] = members[0][0]
            elif members:
                total = sum(count for _, count in members)
                for number, (name, count) in enumerate(members):
                    inputs.append(Input(archive, obj, name, count + (size - total if number == 0 else 0)))
            else:
                inputs.append(Input(archive, obj, input_section, size))
    for archive, obj, key, size in literals:
        inputs.append(Input(archive, obj, iram_names.get((archive, obj, key), key + ".literal"), size, True))
    return inputs, sections, constants


def build_variant(build):
    build_id = (build / "build_id.txt").read_text(encoding="utf-8").strip()
    variant = build_id.rsplit("-", 1)[-1]
    if variant not in VARIANTS:
        raise ValueError(f"unknown build variant: {build_id}")
    return build_id, variant


def read_build(build, root=ROOT):
    metadata = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    elf = build / metadata["app_elf"]
    elf_symbols = symbols(find_tool("nm"), elf)
    mapfile = elf.with_suffix(".map")
    maptext = mapfile.read_text(encoding="utf-8")
    inputs, sections, constants = map_inputs(maptext, elf_symbols)
    size = json.loads(run(idf_python(), str(Path(metadata["idf_path"]) / "tools/idf_size.py"),
                          "--format", "json2", str(mapfile)))
    scripts = build / "esp-idf/esp_system/ld"
    script = "\n".join((scripts / name).read_text(encoding="utf-8") for name in ("sections.ld", "memory.ld"))
    # The map evaluates linker constants even when their definitions use arithmetic.
    script += "\n" + "\n".join(f"{key} = {value};" for key, value in constants.items())
    end = sections[".iram0.text"][0] + sections[".iram0.text"][1]
    align, pad, slack = rounding(script, end)
    config = configuration((build / "sdkconfig").read_text(encoding="utf-8"))
    regions = {name: (int(address, 16), int(size, 16)) for name, address, size in
               re.findall(r"^(iram0_0_seg|dram0_0_seg)\s+(0x[\da-f]+)\s+(0x[\da-f]+)", maptext, re.M)}
    build_id, variant = build_variant(build)
    idf_commit = run("git", "-C", metadata["idf_path"], "rev-parse", "HEAD").strip()
    return dict(build=build, metadata=metadata, owned=components(root, metadata), build_id=build_id, variant=variant, idf_commit=idf_commit, inputs=inputs, sections=sections, constants=constants,
                size=size, align=align, pad=pad, slack=slack, config=config, regions=regions)


def update_budgets(root, builds):
    labels = [build["variant"] for build in builds]
    if len(labels) != len(set(labels)):
        raise ValueError("duplicate budget build variant")
    missing = [variant for variant in VARIANTS if variant not in labels]
    if missing:
        raise ValueError("budget update missing variants: " + ", ".join(missing))
    pin = idf_pin(root)
    for build in builds:
        drift = idf_drift(build["idf_commit"], pin["commit"])
        if drift:
            raise ValueError(f"cannot update budgets: {drift}")
    measured, files = {}, {}
    for build in builds:
        for site, size, _ in build["report"]:
            if size:
                measured.setdefault((site.path, site.line), {})[build["variant"]] = size
    for (name, line), sizes in sorted(measured.items()):
        if name not in files:
            files[name] = ((root / name).read_bytes().decode("utf-8"), [])
        text, edits = files[name]
        prefix = "".join(text.splitlines(keepends=True)[:line - 1])
        matches = list(BUDGET.finditer(prefix))
        values = set(sizes.values())
        clause = str(next(iter(values))) + " B" if len(values) == 1 else ", ".join(
            f"{variant} {sizes[variant]} B" for variant in VARIANTS if variant in sizes)
        previous = matches[-1].group(1)
        print(f"{name}:{line}: {previous} -> {clause}")
        edits.append((matches[-1].start(1), matches[-1].end(1), clause))
    for name, (text, edits) in files.items():
        for start, end, clause in sorted(edits, reverse=True):
            text = text[:start] + clause + text[end:]
        (root / name).write_bytes(text.encode("utf-8"))
    print(f"Updated IRAM budgets: {len(measured)} active sites")


def render(builds):
    builds = sorted(builds, key=lambda b: VARIANTS.index(b["variant"]))
    labels = [b["variant"] for b in builds]
    headers = [f"{b['build_id']} (IDF {b['idf_commit'][:12]})" for b in builds]
    release = next((build for label, build in zip(labels, builds) if label == "release"), None)
    if release is None:
        raise ValueError("documentation tables require a release build")
    lines = [f"> Generated by: `{REGENERATE}`", "", "Bytes from each build's ELF, link map, sdkconfig and ESP-IDF size JSON. Heap is a link-time range, not runtime free memory.", ""]
    pin = idf_pin()
    for commit in dict.fromkeys(b["idf_commit"] for b in builds if idf_drift(b["idf_commit"], pin["commit"])):
        lines.extend([f"Built on IDF {commit[:12]}; launcher/idf-pin.json pins {pin['commit'][:12]}, so measured bytes can differ from the budgets.", ""])
    rows = []
    for name in ("iram0_0_seg", "dram0_0_seg"):
        rows.append((name + " shared alias", *(f"{hex(b['regions'][name][0])}–{hex(sum(b['regions'][name]))} ({b['regions'][name][1]} B)" for b in builds)))
    rows.append(("DIRAM used (idf_size)", *(next(region['used'] for region in b['size']['layout'] if region['name'] == 'DIRAM') for b in builds)))
    keys = sorted({name for b in builds for name in b["sections"] if name.startswith((".iram", ".dram"))})
    for key in keys:
        rows.append((key, *(b["sections"].get(key, (0, 0))[1] for b in builds)))
    for label, field in (("Split granularity", "align"), ("Prefetch pad", "pad"), ("Slack including pad", "slack")):
        rows.append((label, *(b[field] for b in builds)))
    for key in ("_iram_end", "_data_start", "_bss_start", "_bss_end", "_heap_start", "_heap_low_start"):
        if any(key in b["constants"] for b in builds):
            rows.append((key, *(hex(b["constants"][key]) if key in b["constants"] else "absent" for b in builds)))
    lines += [collapsed("Regions and boundaries", markdown(["Region / boundary", *headers], rows)), "Split follows `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE`; alignment and prefetch padding are read from the linked scripts and map.", ""]
    components = [Counter() for _ in builds]
    for b, counts in zip(builds, components):
        for item in b["inputs"]:
            counts[item.archive] += item.size
    names = sorted(set().union(*components), key=lambda name: (-max(c[name] for c in components), name))
    placements = [Counter() for _ in builds]
    for build, counts in zip(builds, placements):
        for item in build["inputs"]:
            placement = "our sites" if item.archive == "libmain.a" or any(
                matches_site(item, site) for site, _, _ in build["report"]) else "IDF"
            counts[(item.archive, placement)] += item.size
        ours = sum(size for (_, placement), size in counts.items() if placement == "our sites")
        if ours != sum(size for _, size, _ in build["report"]):
            raise ValueError(f"{build['variant']}: our sites component bytes differ from site table")
    component_rows = [(name, placement, *(c[(name, placement)] for c in placements))
                      for name in names for placement in ("our sites", "IDF")
                      if any(c[(name, placement)] for c in placements)]
    component_rows.append(("Linker fill / split slack", "unattributed", *(sum(size for key, (_, size) in b["sections"].items() if key.startswith(".iram")) - sum(c.values()) for b, c in zip(builds, components))))
    lines += [collapsed("IRAM components by placement", markdown(["IRAM component", "Placement", *labels], component_rows))]
    reports = [b["report"] for b in builds]
    rows = []
    site_labels = sorted({site.label for report in reports for site, _, _ in report})
    for label in site_labels:
        records = [next((record for record in report if record[0].label == label), None) for report in reports]
        site = next(record[0] for record in records if record)
        budget = ", ".join(f"{name} {value}" for name, value in site.budget.items()) if isinstance(site.budget, dict) else site.budget
        rows.append((site.owner, site.label, ", ".join(sorted({name for record in records if record for name in record[2]})),
                     *(record[1] if record else "not built" for record in records), budget, site.reason))
    show_owner = any(row[0] != "engine" for row in rows)
    if not show_owner:
        rows = [row[1:] for row in rows]
    lines += [markdown([*(["Owner"] if show_owner else []), "Placement site", "Symbols", *labels, "Budget B", "Reason"], rows)]
    differences = []
    for name in names:
        counts = [c[name] for c in components]
        if len(set(counts)) == 1:
            continue
        groups = [Counter() for _ in builds]
        for b, group in zip(builds, groups):
            for item in b["inputs"]:
                if item.archive == name and not item.literal:
                    group[(item.object, item.symbol)] += item.size
        changed = sorted(symbol for symbol in set().union(*groups) if len({g[symbol] for g in groups}) > 1)
        for obj, symbol in changed:
            print(f"{name}:{obj}:{symbol}: " + ", ".join(f"{label} {group[(obj, symbol)]} B" for label, group in zip(labels, groups)))
        exclusive = []
        for index, group in enumerate(groups):
            unique = [symbol for symbol in group if all(not other[symbol] for j, other in enumerate(groups) if j != index)]
            exclusive.append(", ".join(f"{obj}:{symbol} ({group[(obj, symbol)]} B)" for obj, symbol in sorted(unique, key=lambda symbol: (-group[symbol], symbol))))
        differences.append((name, *counts, *exclusive))
    lines += ["#### Differs between variants", "", "Exclusive symbols are absent from IRAM in every other listed variant. The console reports the full symbol-level diff.", "",
              collapsed("Variant differences", markdown(["Component", *labels, *(label + " only" for label in labels)], differences))]
    return "\n".join(lines)


def chart(kind, body):
    if kind not in CHARTS:
        raise ValueError(f"unknown chart kind: {kind}")
    headers, components = parse_table(body, "IRAM component", ("Placement", "release"))
    variants = [variant for variant in VARIANTS if variant in headers]
    region_headers, regions = parse_table(body, "Region / boundary")
    _, sites_table = parse_table(body, "Placement site", variants)
    region_rows = {row["Region / boundary"]: row for row in regions}
    def region(name, variant):
        if name not in region_rows:
            raise ValueError(f"Region / boundary: missing row {name}")
        column = next((column for column in region_headers if f"-{variant} (IDF " in column), None)
        if column is None:
            raise ValueError(f"Region / boundary: missing column for {variant}")
        return region_rows[name][column]
    def number(value):
        return 0 if value == "not built" else int(value)
    ours = {}
    for variant in variants:
        ours[variant] = sum(number(row[variant]) for row in components if row["Placement"] == "our sites")
        if ours[variant] != sum(number(row[variant]) for row in sites_table):
            raise ValueError(f"{variant}: our sites component bytes differ from site table")
    def alias(name, variant):
        value = region(name + " shared alias", variant)
        match = re.fullmatch(r"(0x[0-9a-f]+)–(0x[0-9a-f]+) \((\d+) B\)", value)
        if not match:
            raise ValueError(f"invalid shared alias: {value}")
        start, end, size = match.groups()
        if int(end, 16) - int(start, 16) != int(size):
            raise ValueError(f"inconsistent shared alias: {value}")
        return start, end, int(size)
    command = f"python launcher/tools/build/memory_map.py --chart {kind} docs/memory/images/{CHARTS[kind]}"
    if kind == "pool":
        rows, scales = [], []
        legend = [("ESP-IDF IRAM", svg_bars.IDF_FILL), ("IRAM we place", svg_bars.OURS_FILL),
                  ("Split slack incl. pad", svg_bars.SLACK_FILL), ("DRAM data + BSS", svg_bars.DRAM_FILL),
                  ("Internal heap", svg_bars.HEAP_FILL)]
        for variant in variants:
            start, end, size = alias("dram0_0_seg", variant)
            iram = sum(int(region(name, variant)) for name in region_rows if name.startswith('.iram') and name != '.iram0.text_end')
            slack = int(region("Slack including pad", variant))
            dram = sum(int(region(name, variant)) for name in (".dram0.data", ".dram0.bss"))
            heap = int(end, 16) - int(region("_heap_start", variant), 16)
            counts = (iram - ours[variant], ours[variant], slack, dram, heap)
            if sum(counts) != size:
                raise ValueError(f"{variant}: pool segments do not sum to shared alias")
            rows.append((variant, [(label, count, fill) for (label, fill), count in zip(legend, counts)],
                         f"{heap / svg_bars.KB_BYTES:.1f} KB heap"))
            scales.append(size)
        start, end, _ = alias("dram0_0_seg", "release")
        iram_start, _, _ = alias("iram0_0_seg", "release")
        return svg_bars.bars(command, "Shared internal memory pool", "Linked ranges in address order; heap is not runtime free memory", rows, max(scales),
                             (f"IRAM alias starts at {iram_start}; axis shows the DRAM alias", "1 KB = 1024 B"), legend, (start, end))
    idf = Counter()
    fill = None
    for row in components:
        if row["Placement"] == "IDF":
            idf[row["IRAM component"]] += int(row["release"])
        elif row["IRAM component"] == "Linker fill / split slack":
            fill = int(row["release"]) - int(region("Slack including pad", "release"))
    if fill is None:
        raise ValueError("IRAM component: missing row Linker fill / split slack")
    ordered = sorted(idf.items(), key=lambda item: (-item[1], item[0]))
    selected = [(name.removeprefix('lib').removesuffix('.a'), count) for name, count in ordered[:svg_bars.COMPONENT_TOP_COUNT]]
    selected.extend([("other", sum(count for _, count in ordered[svg_bars.COMPONENT_TOP_COUNT:])), ("Linker fill", fill)])
    rows = [(label, [(label, count, svg_bars.IDF_FILL)], f"{count} B") for label, count in selected]
    totals = [f"{variant}: {sum(int(row[variant]) for row in components if row['Placement'] == 'IDF')} B IDF-placed" for variant in variants]
    return svg_bars.bars(command, "ESP-IDF IRAM by component — release", f"Top {svg_bars.COMPONENT_TOP_COUNT} archives; remaining archives summed as other", rows,
                         max(count for _, count in selected), ("; ".join(totals), "Unattributed linker fill excludes split slack and prefetch pad."))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("builds", nargs="*", type=Path)
    parser.add_argument("--root", type=Path, default=ROOT)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--source-only", action="store_true")
    modes.add_argument("--write", action="store_true")
    modes.add_argument("--tables", type=Path)
    modes.add_argument("--check", action="store_true")
    modes.add_argument("--update-budgets", action="store_true")
    modes.add_argument("--chart", nargs=2, metavar=("KIND", "OUT"))
    parser.add_argument("--require-pin", action="store_true", help="fail when a build differs from the pinned IDF commit")
    args = parser.parse_args()
    try:
        if args.chart:
            kind, out = args.chart
            svg_bars.write(out, chart(kind, blocks(DOC.read_text(encoding="utf-8"))[BLOCK][2]))
            return 0
        if args.source_only:
            placements = sites(args.root)
            print(f"IRAM source gate: {len(placements)} sites checked (launcher/CMakeLists.txt and IDF project component discovery)")
            return 0
        if not args.builds:
            parser.error("build directories required unless --source-only")
        builds = [read_build(build, args.root) for build in args.builds]
        pin = idf_pin(args.root)
        errors = []
        for build in builds:
            try:
                placements, active = build_sites(args.root.resolve(), build)
                drift = idf_drift(build["idf_commit"], pin["commit"])
                if args.require_pin and drift:
                    errors.append(f"{build['variant']}: IDF drift: {drift}")
                build["report"] = check(placements, build["inputs"], build["variant"], build["owned"], active, build["idf_commit"], pin["commit"], exact=not args.update_budgets)
            except ValueError as error:
                errors.append(f"{build['variant']}: {error}")
                continue
            if args.update_budgets:
                continue
            for site, size, names in build["report"]:
                print(f"{build['variant']}: {site.owner} {site.label}: {size} B" +
                      (" (note: compiled out)" if not size else ""))
            print(f"PASS IRAM {build['variant']}: {len(build['report'])} sites checked")
        if errors:
            raise ValueError("\n".join(errors))
        if args.update_budgets:
            update_budgets(args.root, builds)
            return 0
        if args.check:
            return 0
        body = render(builds)
        if args.write:
            replace_block(DOC, BLOCK, body)
            for kind, name in CHARTS.items():
                svg_bars.write(DOC.parent / "images" / name, chart(kind, blocks(DOC.read_text(encoding="utf-8"))[BLOCK][2]))
        elif args.tables:
            args.tables.mkdir(parents=True, exist_ok=True)
            (args.tables / (BLOCK + ".md")).write_text(body, encoding="utf-8")
        else:
            print(body)
        return 0
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or "compiler command failed").strip().splitlines()[-1]
        print(f"FAIL IRAM: {detail}", file=sys.stderr)
        return 1
    except (ValueError, OSError, LayoutError, subprocess.SubprocessError) as error:
        print(f"FAIL IRAM: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
