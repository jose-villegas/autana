#!/usr/bin/env python3
"""Measure isolated IRAM placement experiments in a fresh scratch snapshot.

python launcher/tools/build/iram_what_if.py BUILD... --work DIR [--experiment NAME] [--jobs N] [--before-build COMMAND...] [--write]
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from memory_map import ROOT, DOC, read_build
from generated_blocks import replace_block
from doc_tables import markdown
from scratch import scratch_tree
from code_layout import LayoutError

WHAT_IF_BLOCK = "internal-memory-what-if"
WHAT_IF_COMMAND = "python launcher/tools/build/iram_what_if.py launcher/build launcher/build.dev launcher/build.diag --work launcher/test/build/memory-experiments --write"
DEFAULT_JOBS = 2
OPTIONS = {
    "ringbuf-to-flash": {"variants": ("release", "dev", "diag"), "config": {"CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH": "y", "CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH": "y"}},
    "silent-assertions": {"variants": ("release",), "config": {"CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE": "n", "CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT": "y", "CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT": "y"}},
    "unused-flash-chips": {"variants": ("release",), "config": {"CONFIG_SPI_FLASH_SUPPORT_" + vendor + "_CHIP": "n" for vendor in ("ISSI", "MXIC", "GD", "WINBOND", "BOYA", "TH", "MXIC_OPI")}},
    "encrypted-read-write": {"variants": ("release",), "config": {"CONFIG_SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE": "n"}},
    "libc-locks": {"variants": ("release",), "config": {"CONFIG_LIBC_LOCKS_PLACE_IN_IRAM": "n"}},
    "i2c-isr": {"variants": ("release",), "config": {"CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM": "n"}},
    "dynamic-log-level": {"variants": ("release", "dev", "diag"), "config": {"CONFIG_LOG_TAG_LEVEL_IMPL_NONE": "y", "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL": "n"}},
    "heap-to-flash": {"variants": ("release", "dev", "diag"), "config": {"CONFIG_SPI_MASTER_IN_IRAM": "n", "CONFIG_SPI_MASTER_ISR_IN_IRAM": "n", "CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH": "y"}},
    "noflash-text": {"variants": ("release", "dev", "diag"), "config": {}},
    "gpspi-flash-hal": {"variants": ("release",), "config": {}},
    "freertos-non-isr": {"variants": ("release", "dev", "diag"), "config": {}},
}

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


def rewrite_config(original, values):
    content = original
    for option, value in values.items():
        pattern = r"^(?:" + re.escape(option) + r"=.*|# " + re.escape(option) + r" is not set)\n?"
        content = re.sub(pattern, "", content, flags=re.M)
        content += f"\n{option}={value}\n"
    return content


def result_row(name, baseline, measured):
    values = OPTIONS[name]["config"]
    wrong = [key for key, value in values.items() if measured["config"].get(key, "n") != value]
    if wrong:
        return name, "config rejected", "not measured", "not measured", ", ".join(wrong)
    def bytes_in(build, prefix):
        return sum(size for section, (_, size) in build["sections"].items() if section.startswith(prefix)
                   and section not in {".dram0.dummy", ".iram0.text_end"})
    change = {"noflash-text": "our `(noflash)` entries become `(noflash_text)`",
              "gpspi-flash-hal": "libhal `spi_flash_hal_gpspi*` to flash",
              "freertos-non-isr": "IDF's flash-placement selections, except FromISR, port and present-path queue calls"}
    return (name, bytes_in(baseline, ".iram") - bytes_in(measured, ".iram"),
            bytes_in(baseline, ".dram") - bytes_in(measured, ".dram"),
            measured["constants"]["_heap_start"] - baseline["constants"]["_heap_start"],
            change.get(name, ", ".join(f"{k}={v}" for k, v in values.items())))


def what_if(template, directory, project, selected=None, before_build=None, jobs=DEFAULT_JOBS):
    launcher = project / "launcher"
    build = launcher / "build.memory"
    build.mkdir(exist_ok=True)
    config = build / "sdkconfig"
    original_config = (template["build"] / "sdkconfig").read_text(encoding="utf-8")
    original_lf = (launcher / "main/linker.lf").read_text(encoding="utf-8")
    defaults = launcher / "sdkconfig.defaults"
    original_defaults = defaults.read_text(encoding="utf-8")
    idf = Path(template["metadata"]["idf_path"])
    variant = template["variant"]
    rows, baseline = [], None
    try:
        for name in [None, *experiments(variant, selected)]:
            values = OPTIONS[name]["config"] if name else {}
            config.write_text(rewrite_config(original_config, values), encoding="utf-8")
            defaults.write_text("\n".join(line for line in original_defaults.splitlines()
                                          if line.split("=", 1)[0] not in values) + "\n", encoding="utf-8")
            fragment = scratch_fragment(name, original_lf, idf, baseline)
            (launcher / "main/linker.lf").write_text(fragment, encoding="utf-8")
            log = directory / (variant + "-" + (name or "baseline") + ".log")
            print(f"scratch build: {name or 'baseline'} -> {log}", flush=True)
            if before_build:
                subprocess.run(before_build, check=True)
            command = ['bash', '-c', '. "$1"; idf_init "$2" "" "$3" || exit; idf -B "$4" -D "SDKCONFIG=$5" reconfigure && idf_in "$2" ninja -j "$6" -C "$4" launcher.elf && sh "$3/write_build_id.sh" "$4" "$7"',
                       'memory-map', str(ROOT / 'launcher/tools/build/idf.sh'), str(launcher.resolve()),
                       str(ROOT / 'launcher/tools/build'), str(build.resolve()), str(config.resolve()), str(jobs), variant]
            environment = dict(os.environ, IDF_PATH=str(idf))
            with log.open("w", encoding="utf-8") as stream:
                result = subprocess.run(command, env=environment, stdout=stream, stderr=subprocess.STDOUT, timeout=1800)
            if result.returncode:
                if name is None:
                    raise ValueError(f"{variant}: baseline build failed; see {log}")
                failures = [line.strip() for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                            if re.search(r"error:|CMake Error|FAILED:|Error", line)]
                row = (name, "build failed", "not measured", "not measured", "; ".join(failures[:2]) or f"exit {result.returncode}; see {log.name}")
            else:
                measured = read_build(build, project)
                if name is None:
                    baseline = measured
                    continue
                row = result_row(name, baseline, measured)
            rows.append((variant, *row))
    finally:
        defaults.write_text(original_defaults, encoding="utf-8")
        (launcher / "main/linker.lf").write_text(original_lf, encoding="utf-8")
    return baseline, rows


def experiments(variant, selected=None):
    names = list(OPTIONS) if selected is None else selected
    unknown = set(names) - OPTIONS.keys()
    if unknown:
        raise ValueError("unknown experiment: " + ", ".join(sorted(unknown)))
    return [name for name in names if variant in OPTIONS[name]["variants"]]


def render_results(baselines, rows):
    legend = "; ".join(f"{b['build_id']} (IDF {b['idf_commit']})" for b in baselines)
    return f"> Generated by: `{WHAT_IF_COMMAND}`\n\nBytes are scratch-build deltas on the local IDF named below; latency and cache-off safety are not board-measured. Positive IRAM/DRAM deltas are bytes freed. Heap-start delta is negative when the heap grows. Each row is isolated; savings are not additive.\n\n{legend}\n\n" + markdown(["Variant", "Experiment", "IRAM freed B", "DRAM freed B", "Heap-start delta B", "Change"], rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("builds", nargs="+", type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--experiment", action="append", choices=OPTIONS)
    parser.add_argument("--before-build", nargs="+")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        builds = [dict(build=build, metadata=json.loads((build / "project_description.json").read_text(encoding="utf-8")),
                       variant=(build / "build_id.txt").read_text(encoding="utf-8").strip().rsplit("-", 1)[-1]) for build in args.builds]
        for name in ("results.json", WHAT_IF_BLOCK + ".md"):
            (args.work / name).unlink(missing_ok=True)
        project = scratch_tree(args.work, ROOT, fresh=True)
        shutil.copytree(ROOT / "launcher/managed_components", project / "launcher/managed_components", dirs_exist_ok=True)
        baselines, rows = [], []
        for build in builds:
            baseline, measured = what_if(build, args.work, project, args.experiment, args.before_build, args.jobs)
            baselines.append(baseline)
            rows.extend(measured)
        body = render_results(baselines, rows)
        (args.work / (WHAT_IF_BLOCK + ".md")).write_text(body, encoding="utf-8")
        if args.write:
            replace_block(DOC, WHAT_IF_BLOCK, body)
        return 0
    except (ValueError, OSError, LayoutError, subprocess.SubprocessError) as error:
        print(f"FAIL IRAM what-if: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
