#!/usr/bin/env python3
"""Account for internal memory and enforce budgets at IRAM placement sites.

python launcher/tools/build/memory_map.py --source-only
python launcher/tools/build/memory_map.py launcher/build launcher/build.dev launcher/build.diag --write
"""
import argparse
import ast
import fnmatch
import json
import re
import os
import shutil
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from code_layout import find_tool, run
from generated_blocks import replace_block
from espressif import idf_python
from layout_pad import placed_sections
sys.path.insert(0, str(ROOT / "scripts/gates"))
from c_comments import scan

BUDGET = re.compile(r"IRAM budget: (\d+) B\b")
ATTRIBUTE = re.compile(r"\b(?:\w*IRAM_ATTR|\w*IRAM\w*_ATTR)\b|__attribute__\s*\(\(\s*section\s*\(\s*\"\.iram[^\"]*\"")
PLACEMENT = re.compile(r"\((?:noflash\w*|iram\w*)\)")
CONFIG_PLACEMENT = re.compile(r"CONFIG_\w*(?:IN_IRAM|PLACE_\w*INTO_IRAM)\w*=(?:y|n)")
BLOCK = "internal-memory-map"
DOC = ROOT / "docs/notes/Board-and-Memory.md"
REGENERATE = "python launcher/tools/build/memory_map.py launcher/build launcher/build.dev launcher/build.diag --write"
OUR_ARCHIVES = {"libmain.a", "libesp32_s3_touch_amoled_1_8.a"}
WHAT_IF_BLOCK = "internal-memory-what-if"
WHAT_IF_COMMAND = "python launcher/tools/build/memory_map.py launcher/build launcher/build.dev launcher/build.diag --what-if launcher/test/build/memory-experiments --write"
WHAT_IF_OPTIONS = {
    "ringbuf-to-flash": {"CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH": "y", "CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH": "y"},
    "silent-assertions": {"CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE": "n", "CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT": "y", "CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT": "y"},
    "unused-flash-chips": {"CONFIG_SPI_FLASH_SUPPORT_" + vendor + "_CHIP": "n" for vendor in ("ISSI", "MXIC", "GD", "WINBOND", "BOYA", "TH", "MXIC_OPI")},
    "encrypted-read-write": {"CONFIG_SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE": "n"},
    "libc-locks": {"CONFIG_LIBC_LOCKS_PLACE_IN_IRAM": "n"},
    "i2c-isr": {"CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM": "n"},
    "dynamic-log-level": {"CONFIG_LOG_TAG_LEVEL_IMPL_NONE": "y", "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL": "n"},
    "heap-to-flash": {"CONFIG_SPI_MASTER_IN_IRAM": "n", "CONFIG_SPI_MASTER_ISR_IN_IRAM": "n", "CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH": "y"},
    "noflash-text": {},
    "gpspi-flash-hal": {},
    "freertos-non-isr": {},
}


def kconfig_options(build):
    idf = Path(build["metadata"]["idf_path"])
    options = {}
    paths = sorted((idf / "components").rglob("Kconfig*")) + [idf / "Kconfig"]
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"^\s*config (\w+)\s*\n", text, re.M):
            name = "CONFIG_" + match.group(1)
            if not re.search(r"IRAM|PLACE_.*INTO_FLASH|SPI_FLASH_(SUPPORT_.*_CHIP|ENABLE_ENCRYPTED_READ_WRITE|ROM_IMPL|AUTO_SUSPEND)|LOG_DYNAMIC_LEVEL_CONTROL|ASSERTIONS_SILENT|CHECKS_SILENT", name):
                continue
            component = path.relative_to(idf).parts[1] if path != idf / "Kconfig" else "compiler"
            stop = re.search(r"^\s*(?:config|menuconfig|endmenu)\b", text[match.end():], re.M)
            help_text = text[match.end():match.end() + stop.start()] if stop else text[match.end():]
            prompt = re.search(r'(?:bool|prompt)\s+"([^"]+)"', help_text)
            options[name] = (component, prompt.group(1) if prompt else "Internal selection", f"{path.relative_to(idf).as_posix()}:{text[:match.start()].count(chr(10)) + 1}")
    return options


def opportunities(builds, labels):
    options = kconfig_options(builds[0])
    rows = []
    for name, (component, description, source) in sorted(options.items()):
        if not any(name in b["config"] for b in builds):
            continue
        counts = [sum(i.size for i in b["inputs"] if i.archive == f"lib{component}.a") for b in builds]
        cost = "Flash cache misses; cache-off callers must stay in IRAM. Latency: not board-measured."
        recommendation = "needs a board measurement first"
        if component in {"esp_ringbuf", "newlib", "esp_libc"}:
            cost = "Cache-off ISR callers cannot use flash functions; audit console and libc callers. Latency: not board-measured."
        if "SPI_MASTER" in name:
            cost = "Present queue and callbacks gain cache misses; heap and FreeRTOS dependencies constrain placement. Latency: not board-measured."
        if "SPI_FLASH" in name:
            cost = "Flash operations disable cache; retain required cache-off driver paths. Latency: not board-measured."
        if "HEAP" in name:
            cost = "Allocation/free cache misses; SPI_MASTER_IN_IRAM and SPI_MASTER_ISR_IN_IRAM must be disabled. Latency: not board-measured."
        if "ASSERT" in name or "CHECKS" in name:
            cost = "Failure reports lose messages. Latency: not board-measured."
            recommendation = "candidate to move out (release)"
        if "RINGBUF" in name or "LIBC_LOCKS" in name or "LOG_DYNAMIC" in name:
            recommendation = "candidate to move out after caller audit"
        elif component in {"spi_flash", "esp_system", "esp_hw_support"}:
            recommendation = "keep cache-off paths; audit finer selections"
        if not any(counts):
            recommendation = "keep; no linked component IRAM to free"
        rows.append((name, *(b["config"].get(name, "not configured") for b in builds), description,
                     "not attributable; component IRAM " + "/".join(str(c) for c in counts) + " B (upper bound, not savings)",
                     cost + " <sup>[[60]](../Citations.md#60)</sup>", recommendation,
                     "bytes: attributed from map (component only)", source))
    return table(["Option", *labels, "What it places", "IRAM bytes", "Cost of moving it out", "Recommendation", "Measured", "IDF source"], rows)


def scratch_fragment(name, original, idf, baseline):
    if name == "noflash-text":
        return original.replace("(noflash)", "(noflash_text)")
    if name == "gpspi-flash-hal":
        symbols = sorted({item.symbol for item in baseline["inputs"]
                          if item.archive == "libhal.a" and item.object.startswith("spi_flash_hal_gpspi")})
        return original + "\n[mapping:memory_probe]\narchive: libhal.a\nentries:\n" + "\n".join(
            f"    spi_flash_hal_gpspi:{symbol} (default)" for symbol in symbols) + "\n"
    if name == "freertos-non-isr":
        text = (idf / "components/freertos/linker.lf").read_text(encoding="utf-8")
        start = text.index("if FREERTOS_PLACE_FUNCTIONS_INTO_FLASH = y:")
        selections = sorted(set(re.findall(r"^\s*(\w+)\s*:\s*(\w+)\s+\(default\)", text[start:], re.M)))
        # Queue APIs are in the measured present path; ISR APIs must retain cache-off reachability.
        keep = {"xQueueGenericSend", "xQueueReceive", "xQueueSemaphoreTake", "xQueueGiveFromISR", "xQueueGenericSendFromISR", "xQueueReceiveFromISR"}
        entries = [f"    {obj}:{symbol} (default)" for obj, symbol in selections if "FromISR" not in symbol and symbol not in keep and not obj.startswith("port")]
        return original + "\n[mapping:memory_probe]\narchive: libfreertos.a\nentries:\n" + "\n".join(entries) + "\n"
    return original


def what_if(baseline, directory, selected=None, before_build=None):
    directory.mkdir(parents=True, exist_ok=True)
    project = directory / "project"
    launcher = project / "launcher"
    if not launcher.exists():
        for relative in run("git", "-C", str(ROOT), "ls-files", "launcher", "third_party", "scripts", ".gitmodules").splitlines():
            source = ROOT / relative
            if not source.is_file():
                continue
            target = project / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        shutil.copytree(ROOT / "launcher/managed_components", launcher / "managed_components", dirs_exist_ok=True)
    build = launcher / "build.memory"
    config = build / "sdkconfig"
    build.mkdir(exist_ok=True)
    original_config = (baseline["build"] / "sdkconfig").read_text(encoding="utf-8")
    original_lf = (ROOT / "launcher/main/linker.lf").read_bytes()
    idf = Path(baseline["metadata"]["idf_path"])
    variant = "diag" if baseline["config"].get("CONFIG_LAUNCHER_SELFTEST") == "y" else "dev" if baseline["config"].get("CONFIG_LAUNCHER_DEVELOPMENT") == "y" else "release"
    result_file = directory / "results.json"
    results = json.loads(result_file.read_text(encoding="utf-8")) if result_file.exists() else {}
    rows = []
    for name in selected or WHAT_IF_OPTIONS:
        values = WHAT_IF_OPTIONS[name]
        content = original_config
        for option, value in values.items():
            content = re.sub(r"^(?:" + re.escape(option) + r"=.*|# " + re.escape(option) + r" is not set)\n?", "", content, flags=re.M)
            content += f"\n{option}={value}\n"
        config.write_text(content, encoding="utf-8")
        defaults = launcher / "sdkconfig.defaults"
        defaults.write_text("\n".join(line for line in (ROOT / "launcher/sdkconfig.defaults").read_text(encoding="utf-8").splitlines()
                                      if line.split("=", 1)[0] not in values) + "\n", encoding="utf-8")
        fragment = scratch_fragment(name, original_lf.decode("utf-8"), idf, baseline)
        (launcher / "main/linker.lf").write_bytes(fragment.encode("utf-8"))
        log = directory / (name + ".log")
        print(f"scratch build: {name} -> {log}", flush=True)
        if before_build:
            subprocess.run(before_build, check=True)
        command = ['bash', '-c', '. "$1"; idf_init "$2" "" "$3" || exit; idf -B "$4" -D "SDKCONFIG=$5" reconfigure && idf_in "$2" ninja -j 2 -C "$4" launcher.elf',
                   'memory-map', str(ROOT / 'launcher/tools/build/idf.sh'), str(launcher.resolve()),
                   str(ROOT / 'launcher/tools/build'), str(build.resolve()), str(config.resolve())]
        environment = dict(os.environ, IDF_PATH=str(idf))
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, env=environment, stdout=stream, stderr=subprocess.STDOUT, timeout=1800)
        if result.returncode:
            failures = [line.strip() for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                        if re.search(r"error:|CMake Error|FAILED:|Error", line)]
            rows.append((name, "build failed", "not measured", "not measured", "; ".join(failures[:2]) or f"exit {result.returncode}; see {log.name}"))
        else:
            measured = read_build(build)
            wrong = [key for key, value in values.items() if measured["config"].get(key, "n") != value]
            if wrong:
                rows.append((name, "config rejected", "not measured", "not measured", ", ".join(wrong)))
            else:
                def bytes_in(b, prefix):
                    return sum(size for section, (_, size) in b["sections"].items() if section.startswith(prefix)
                               and section not in {".dram0.dummy", ".iram0.text_end"})
                rows.append((name, bytes_in(baseline, ".iram") - bytes_in(measured, ".iram"),
                             bytes_in(baseline, ".dram") - bytes_in(measured, ".dram"),
                             measured["constants"]["_heap_start"] - baseline["constants"]["_heap_start"],
                             "bytes: scratch build; latency: not board-measured; " + ", ".join(f"{k}={v}" for k, v in values.items())))
        results[variant + ":" + name] = [variant, *rows[-1]]
        result_file.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        # Preserve results after every experiment; a failed later build must not hide earlier measurements.
        body = f"> Generated by: `{WHAT_IF_COMMAND}`\n\nPositive IRAM/DRAM deltas are bytes freed. Heap-start delta is negative when the heap grows. Each row is isolated; savings are not additive.\n\n" + table(["Variant", "Experiment", "IRAM freed B", "DRAM freed B", "Heap-start delta B", "Measured / constraint"], [results[key] for key in sorted(results)])
        (directory / (WHAT_IF_BLOCK + ".md")).write_text(body, encoding="utf-8")
    return body


@dataclass
class Site:
    path: str
    line: int
    owner: str
    reason: str
    budget: int | None
    archive: str
    entries: list
    guards: tuple = ()

    @property
    def label(self):
        return f"{self.path}:{self.line}"


@dataclass
class Input:
    archive: str
    object: str
    symbol: str
    size: int


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


def sites(root):
    found = []
    roots = [root / "launcher/main", root / "launcher/components/esp32_s3_touch_amoled_1_8"]
    paths = sorted({p for base in roots for p in base.rglob("*") if p.suffix in {".c", ".h", ".lf"}})
    paths += sorted((root / "launcher").glob("sdkconfig.defaults*"))
    aliases = {}
    attribute_aliases = set()
    for path in paths:
        if path.suffix in {".c", ".h"}:
            for alias, expression in re.findall(r"^#define\s+(\w+)\s+(.+)$", path.read_text(encoding="utf-8"), re.M):
                options = tuple(re.findall(r"\bCONFIG_\w+\b", expression))
                if options:
                    aliases[alias] = expression
                if ATTRIBUTE.search(expression):
                    attribute_aliases.add(alias)
            for expression, alias in re.findall(r"^#if ([^\n]+)\n#define\s+(\w+)\s+1\b", path.read_text(encoding="utf-8"), re.M):
                options = tuple(re.findall(r"\bCONFIG_\w+\b", expression))
                if options:
                    aliases[alias] = expression
    for path in paths:
        name = path.relative_to(root).as_posix()
        owner = "/".join(name.split("/")[:4]) + "/" if "/apps/" in name else "engine"
        lines = path.read_text(encoding="utf-8").splitlines()
        guards = []
        for index, line in enumerate(lines):
            if path.suffix in {".c", ".h"}:
                if re.match(r"\s*#\s*(?:if|ifdef)\b", line):
                    expression = re.sub(r"^\s*#\s*(?:if|ifdef)\s+", "", line)
                    expression = re.sub(r"\b\w+\b", lambda match: "(" + aliases[match.group()] + ")" if match.group() in aliases else match.group(), expression)
                    guards.append([expression, [expression]])
                elif re.match(r"\s*#\s*else\b", line) and guards:
                    guards[-1][0] = "!(" + " || ".join(guards[-1][1]) + ")"
                elif re.match(r"\s*#\s*elif\b", line) and guards:
                    expression = re.sub(r"^\s*#\s*elif\s+", "", line)
                    guards[-1][0] = "!(" + " || ".join(guards[-1][1]) + ") && (" + expression + ")"
                    guards[-1][1].append(expression)
                elif re.match(r"\s*#\s*endif\b", line) and guards:
                    guards.pop()
                if (not ATTRIBUTE.search(line) and not any(re.search(r"\b" + re.escape(alias) + r"\b", line)
                                                       for alias in attribute_aliases)) or line.lstrip().startswith(("//", "/*", "*", "#")):
                    continue
                declaration = " ".join(lines[index:index + 5])
                declaration = re.sub(r"__attribute__\s*\(\(.*?\)\)", "", declaration)
                function = re.search(r"\b(\w+)\s*\([^;{}]*\)\s*\{", declaration)
                if not function:
                    raise ValueError(f"{name}:{index + 1}: unsupported IRAM declaration")
                entries = [(path.stem, function.group(1))]
                archive = "libmain.a" if "main/" in name else "libesp32_s3_touch_amoled_1_8.a"
                kind = "c"
            elif path.suffix == ".lf":
                if not re.match(r"\[mapping:", line):
                    continue
                stop = next((i for i in range(index + 1, len(lines)) if lines[i].startswith("[")), len(lines))
                body = "\n".join(lines[index:stop])
                if not PLACEMENT.search(body):
                    continue
                archive = re.search(r"archive:\s*(\S+)", body).group(1)
                entries = []
                for entry in lines[index:stop]:
                    if PLACEMENT.search(entry):
                        selector = entry.strip().split()[0].split(":")
                        entries.append((selector[0], selector[1] if len(selector) == 2 else "*"))
                kind = "hash"
            else:
                if not CONFIG_PLACEMENT.fullmatch(line):
                    continue
                archive, entries, kind = "", [], "hash"
            reason = preceding_comment(lines, index, kind)
            if not reason:
                raise ValueError(f"{name}:{index + 1}: missing IRAM reason")
            budget = BUDGET.search(reason)
            if entries and not budget:
                raise ValueError(f"{name}:{index + 1}: missing IRAM budget")
            if len(BUDGET.sub("", reason).strip(" .")) < 8:
                raise ValueError(f"{name}:{index + 1}: missing IRAM reason beyond budget")
            found.append(Site(name, index + 1, owner, reason, int(budget.group(1)) if budget else None,
                              archive, entries, tuple(group[0] for group in guards)))
    return found


def check(placements, inputs, config):
    used = Counter()
    names = {}
    for item in inputs:
        matches = [site for site in placements if fnmatch.fnmatch(item.archive, site.archive)
                   and any(fnmatch.fnmatch(item.object.removesuffix(".obj").removesuffix(".o").removesuffix(".c"), obj)
                           and fnmatch.fnmatch(item.symbol, symbol) for obj, symbol in site.entries)]
        if len(matches) > 1:
            # A function attribute and a mapping may describe the same code; the function owns its budget.
            matches = sorted(matches, key=lambda site: site.path.endswith(".lf"))[:1]
        if not matches:
            if item.archive in OUR_ARCHIVES and item.size:
                raise ValueError(f"{item.archive}({item.object}): {item.symbol}: IRAM symbol has no site")
            continue
        site = matches[0]
        used[site.label] += item.size
        names.setdefault(site.label, set()).add(item.symbol)
    report = []
    for site in placements:
        size = used[site.label]
        if not site.entries:
            continue
        if size > site.budget:
            raise ValueError(f"{site.label}: {size} B exceeds IRAM budget {site.budget} B")
        if not size and not any(not condition(guard, config) for guard in site.guards):
            raise ValueError(f"{site.label}: placed nothing in this build")
        report.append((site, size, sorted(names.get(site.label, []))))
    return report


def condition(expression, config):
    expression = re.sub(r"defined\s*\(?\s*(\w+)\s*\)?", r"\1", expression)
    def token(match):
        name = match.group()
        if name.startswith("CONFIG_"):
            return "True" if config.get(name, "n") == "y" else "False"
        if name == "ESP_PLATFORM":
            return "True"
        return name
    expression = re.sub(r"\b[A-Za-z_]\w*\b", token, expression)
    expression = expression.replace("&&", " and ").replace("||", " or ")
    expression = re.sub(r"!(?!=)", " not ", expression).strip()
    node = ast.parse(expression, mode="eval").body
    def evaluate(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, (bool, int)):
            return bool(node.value)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return not evaluate(node.operand)
        if isinstance(node, ast.BoolOp):
            values = [evaluate(value) for value in node.values]
            return all(values) if isinstance(node.op, ast.And) else any(values)
        raise ValueError(f"unsupported IRAM guard: {expression}")
    return evaluate(node)


def configuration(text):
    values = dict(re.findall(r"^(CONFIG_\w+)=(.*)$", text, re.M))
    values.update({name: "n" for name in re.findall(r"^# (CONFIG_\w+) is not set$", text, re.M)})
    return values


def rounding(script, end):
    split = script[script.index(".iram0.text_end"):]
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
            inputs.append(Input(archive, obj, symbol, size))
        else:
            members = [(name, count) for name, (start, count) in elf_symbols.items()
                       if address <= start < address + size and count and not name.startswith("$")]
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
        inputs.append(Input(archive, obj, iram_names.get((archive, obj, key), key + ".literal"), size))
    return inputs, sections, constants


def read_build(build):
    metadata = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    elf = build / metadata["app_elf"]
    symbols = {}
    for line in run(find_tool("nm"), "-n", "-S", str(elf)).splitlines():
        fields = line.split()
        if len(fields) == 4:
            symbols[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    mapfile = elf.with_suffix(".map")
    inputs, sections, constants = map_inputs(mapfile.read_text(encoding="utf-8"), symbols)
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
               re.findall(r"^(iram0_0_seg|dram0_0_seg)\s+(0x[\da-f]+)\s+(0x[\da-f]+)", mapfile.read_text(encoding="utf-8"), re.M)}
    return dict(build=build, metadata=metadata, inputs=inputs, sections=sections, constants=constants,
                size=size, align=align, pad=pad, slack=slack, config=config, regions=regions)


def table(headers, rows):
    def cell(value):
        return str(value).replace("|", "\\|").replace("\n", " ")
    return "\n".join(["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers),
                      *("| " + " | ".join(cell(value) for value in row) + " |" for row in rows)]) + "\n"


def render(builds, placements):
    labels = ["diag" if b["config"].get("CONFIG_LAUNCHER_SELFTEST") == "y" else
              "dev" if b["config"].get("CONFIG_LAUNCHER_DEVELOPMENT") == "y" else "release" for b in builds]
    lines = [f"> Generated by: `{REGENERATE}`", "", "Bytes from each build's ELF, link map, sdkconfig and ESP-IDF size JSON. Heap is a link-time range, not runtime free memory.", ""]
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
    lines += [table(["Region / boundary", *labels], rows), "Split follows `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE`; alignment and prefetch padding are read from the linked scripts and map.", ""]
    components = [Counter() for _ in builds]
    for b, counts in zip(builds, components):
        for item in b["inputs"]:
            counts[item.archive] += item.size
    names = sorted(set().union(*components), key=lambda name: (-max(c[name] for c in components), name))
    component_rows = [(name, "ours" if name in OUR_ARCHIVES else "IDF / managed", *(c[name] for c in components)) for name in names]
    component_rows.append(("Linker fill / split slack", "unattributed", *(sum(size for key, (_, size) in b["sections"].items() if key.startswith(".iram")) - sum(c.values()) for b, c in zip(builds, components))))
    lines += [table(["IRAM component", "Ownership", *labels], component_rows)]
    reports = [check(placements, b["inputs"], b["config"]) for b in builds]
    rows = []
    for index, (site, _, _) in enumerate(reports[0]):
        rows.append((site.owner, site.label, ", ".join(sorted({n for report in reports for n in report[index][2]})),
                     *(report[index][1] for report in reports), site.budget, site.reason))
    lines += [table(["Owner", "Placement site", "Symbols", *labels, "Budget B", "Reason"], rows)]
    differences = []
    for name in names:
        counts = [c[name] for c in components]
        groups = [Counter() for _ in builds]
        for b, group in zip(builds, groups):
            for item in b["inputs"]:
                if item.archive == name:
                    group[(item.object, item.symbol)] += item.size
        changed = sorted(n for n in set().union(*groups) if len({g[n] for g in groups}) > 1)
        if not changed:
            continue
        differences.append((name, *counts, "sum of changed input sections", ""))
        for obj, symbol in changed:
            site = next((site for report in reports for site, _, symbols in report if symbol in symbols), None)
            reason = ", ".join(site.guards) if site and site.guards else "linked section differs; causal option not isolated"
            transitions = []
            for index in range(1, len(builds)):
                if groups[index - 1][(obj, symbol)] == groups[index][(obj, symbol)]:
                    continue
                options = sorted(k for k in builds[index - 1]["config"].keys() | builds[index]["config"].keys()
                                 if builds[index - 1]["config"].get(k, "n") != builds[index]["config"].get(k, "n") and
                                 any(word in k for word in ("LAUNCHER", "ASSERT", "HEAP", "DEBUG", "LOG", "UNITY")))
                transitions.append(f"{labels[index - 1]} to {labels[index]}: " + ", ".join(options))
            differences.append((name + ":" + obj + ":" + symbol, *(g[(obj, symbol)] for g in groups), reason, "; ".join(transitions)))
    lines += ["#### Differs between variants", "", "Symbol changes are map-derived; differing options are correlations, not isolated causal measurements. A zero means absent from IRAM, not necessarily absent from the image.", "", table(["Component / symbol group", *labels, "Why / evidence", "Differing Kconfig"], differences)]
    release = builds[0]
    sizes = [sum(size for name, (_, size) in release['sections'].items() if name.startswith('.iram') and name != '.iram0.text_end'),
             release['slack'], release['sections']['.dram0.data'][1], release['sections']['.dram0.bss'][1],
             sum(release['regions']['dram0_0_seg']) - release['constants']['_heap_start']]
    lines += ["#### Release shared block", "", "The split slack includes the prefetch pad. Boxes show linked bytes in address order; width is schematic.", "", "```mermaid", "flowchart LR",
              "    " + " --- ".join(f'{node}["{label}<br/>{count} B"]' for node, label, count in zip(
                  ("iram", "slack", "data", "bss", "heap"),
                  ("IRAM", "Split slack", "DRAM data", "BSS", "Heap range"), sizes)), "```", "",
              "#### ESP-IDF IRAM options", "", opportunities(builds, labels)]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("builds", nargs="*", type=Path)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--tables", type=Path)
    parser.add_argument("--what-if", type=Path)
    parser.add_argument("--experiment", action="append", choices=WHAT_IF_OPTIONS)
    parser.add_argument("--before-build", nargs="+", help="optional command run before each scratch build")
    args = parser.parse_args()
    try:
        placements = sites(args.root)
        print(f"IRAM source gate: {len(placements)} sites checked")
        if args.source_only:
            return 0
        if not args.builds:
            parser.error("build directories required unless --source-only")
        builds = [read_build(build) for build in args.builds]
        if args.what_if:
            for build in builds:
                body = what_if(build, args.what_if, args.experiment, args.before_build)
            if args.write:
                replace_block(DOC, WHAT_IF_BLOCK, body)
            return 0
        for build in builds:
            for site, size, names in check(placements, build["inputs"], build["config"]):
                print(f"{build['build'].name}: {site.owner} {site.label}: {size}/{site.budget} B" +
                      (" (note: compiled out)" if not size else ""))
        body = render(builds, placements)
        if args.write:
            replace_block(DOC, BLOCK, body)
        elif args.tables:
            args.tables.mkdir(parents=True, exist_ok=True)
            (args.tables / (BLOCK + ".md")).write_text(body, encoding="utf-8")
        else:
            print(body)
        return 0
    except (ValueError, OSError) as error:
        print(f"FAIL IRAM: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
