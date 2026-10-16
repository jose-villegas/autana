"""Render timeline variants and compare each with the shipped settings.

    python launcher/tools/boot_anim/compare_floor_variants.py --out DIR \
        --frame MS --variant 'LABEL:{"mesh_dash":1}'

Timeline and generated header bytes are restored even when a render fails.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

LAUNCHER = pathlib.Path(__file__).resolve().parents[2]
TIMELINE = LAUNCHER / "main/boot/boot_anim_timeline.json"
HEADER = TIMELINE.with_suffix(".h")
QEMU_TIMEOUT_S = 900
DIFFERENCE_CROPS = 3
FLOOR_SUITE_NAME = "run_boot_floor_cost_suite"


def run(*args, **kwargs):
    subprocess.run(args, cwd=LAUNCHER.parent, check=True, **kwargs)


def render(directory, frames):
    run("bash", "launcher/tools/render/scenes/boot_anim_render_host.sh",
        "--build-only", "-o", str(directory))
    binary = directory / ("boot_anim_render.exe" if sys.platform == "win32" else "boot_anim_render")
    for frame in frames:
        run(str(binary), str(frame), "-o", str(directory / (str(frame) + ".bmp")))


def measure(directory, counts, icount):
    directory.mkdir(parents=True, exist_ok=True)
    if counts:
        build = ('. launcher/tools/build/host_make.sh; '
                 'floor_make=$(find_make) || exit 1; floor_jobs=$(host_jobs ""); '
                 '"$floor_make" -C launcher/test/build -f host.mk -j "$floor_jobs" all')
        run("bash", "-c", build)
        binary = LAUNCHER / "test/build" / ("host_tests.exe" if sys.platform == "win32" else "host_tests")
        environment = dict(os.environ, AUTANA_ASSET_DIR=str(LAUNCHER / "test/build/assets"))
        with (directory / "host.log").open("w", encoding="utf-8") as log:
            run(str(binary), "--run", FLOOR_SUITE_NAME, env=environment, stdout=log)
    if icount:
        run("bash", "launcher/test/run_qemu_tests.sh", "--suite", FLOOR_SUITE_NAME,
            "--icount", "--timeout", str(QEMU_TIMEOUT_S))
        shutil.copyfile(LAUNCHER / "build.qemu.shell/qemu_serial.log", directory / "qemu.log")


def cost_table(output, labels, main_log, destination):
    sys.path.insert(0, str(LAUNCHER / "tools/quality"))
    from icount_gate import parse_log
    sys.path.insert(0, str(LAUNCHER / "tools"))
    from build.c_header import read_define
    suite = LAUNCHER / "test/suites/suite_boot_floor_icount.c"
    prefix = read_define(suite, "FLOOR_ROW_PREFIX")
    total_key = read_define(suite, "FLOOR_TOTAL_ROW")
    reject_key = read_define(suite, "FLOOR_REJECT_ROW")
    point_key = prefix + read_define(suite, "FLOOR_POINT_ROW")
    long_key = prefix + read_define(suite, "FLOOR_LONG_ROW")
    count_prefix = re.escape(read_define(suite, "FLOOR_COUNT_PREFIX"))
    frames = [read_define(LAUNCHER / "main/boot/boot_anim_timeline.h", "BOOT_ANIM_IMAGE_START_MS")
              + read_define(suite, "FLOOR_CROSSFADE_OFFSET_MS"), read_define(suite, "FLOOR_EDGE_PEAK_MS")]
    baseline = parse_log(main_log.read_text(encoding="utf-8"))
    lines = ["| Variant | ms | Nodes (ref) | Edges | Pixel writes | Projection + rejection | Setup est. | Pixels est. | Total | Delta main | Delta % | Gap |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for frame in frames:
        total = baseline[f"{prefix}{frame}_{total_key}"]
        lines.append(f"| main rings | {frame} | - | - | - | - | - | - | {total:,.0f} | 0 | 0.0% | - |")
    calibration = []
    for label in labels:
        directory = output / label
        rows = parse_log((directory / "qemu.log").read_text(encoding="utf-8"))
        counts = {int(ms): (int(nodes), int(edges), int(pixels)) for ms, nodes, edges, pixels in re.findall(
            rf"{count_prefix} ms=(\d+) nodes=(\d+) edges=(\d+) pixels=(\d+)",
            (directory / "host.log").read_text(encoding="utf-8"))}
        pixel = (rows[long_key] - rows[point_key]) / (read_define(suite, "FLOOR_LINE_LENGTH") - 1)
        call = rows[point_key] - pixel
        calibration.append(f"- {label}: setup {call:.3f} instructions/call, pixels {pixel:.3f} instructions/write")
        for frame in frames:
            nodes, edges, pixels = counts[frame]
            total, projection = rows[f"{prefix}{frame}_{total_key}"], rows[f"{prefix}{frame}_{reject_key}"]
            delta = total - baseline[f"{prefix}{frame}_{total_key}"]
            setup, raster = call * edges, pixel * pixels
            gap = total - projection - setup - raster
            lines.append(f"| {label} | {frame} | {nodes:,} | {edges:,} | {pixels:,} | {projection:,.0f} | {setup:,.0f} | {raster:,.0f} | {total:,.0f} | {delta:+,.0f} | {delta / baseline[f'{prefix}{frame}_{total_key}']:+.1%} | {gap:+,.0f} |")
    destination.write_text("\n".join(lines) + "\n\n" + "\n".join(calibration) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--frame", type=int, action="append", default=[])
    parser.add_argument("--counts", action="store_true", help="requires an initialized host test build")
    parser.add_argument("--icount", action="store_true")
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--main-log", type=pathlib.Path)
    parser.add_argument("--variant", action="append", default=[])
    args = parser.parse_args()
    output = args.out.resolve()
    originals = {path: path.read_bytes() for path in (TIMELINE, HEADER)}
    labels = ["default"] + [item.split(":", 1)[0] for item in args.variant]
    if args.report:
        if not args.main_log:
            parser.error("--report requires --main-log")
        cost_table(output, labels, args.main_log, args.report)
        return
    if not (args.frame or args.counts or args.icount):
        parser.error("choose --frame, --counts, --icount or --report")
    try:
        if args.frame:
            render(output / "default", args.frame)
        measure(output / "default", args.counts, args.icount)
        for variant in args.variant:
            label, overrides = variant.split(":", 1)
            if pathlib.Path(label).name != label or label in (".", "..", "default"):
                parser.error("variant label must be a directory name other than default")
            changed = json.loads(originals[TIMELINE])
            changed["timing"].update(json.loads(overrides))
            TIMELINE.write_text(json.dumps(changed, indent=4) + "\n", encoding="utf-8")
            generated = subprocess.check_output(
                [sys.executable, str(LAUNCHER / "tools/gen/gen_boot_anim_timeline.py"),
                 str(TIMELINE), str(TIMELINE.with_name("boot_anim_motion.glb"))])
            HEADER.write_bytes(generated)
            directory = output / label
            measure(directory, args.counts, args.icount)
            if not args.frame:
                continue
            render(directory, args.frame)
            command = [sys.executable, str(LAUNCHER / "tools/render/render_compare.py"),
                       "--out", str(directory / "compare.png"), "--crops", str(DIFFERENCE_CROPS),
                       "--summary", str(directory / "summary.txt"),
                       "--label-a", "default", "--label-b", label]
            for frame in args.frame:
                name = str(frame) + ".bmp"
                command += ["--row", str(frame), str(output / "default" / name), str(directory / name)]
            run(*command)
    finally:
        for path, content in originals.items():
            path.write_bytes(content)


if __name__ == "__main__":
    main()
