#!/usr/bin/env python3
"""Turns a raw device self-test capture into a markdown report of
suite_cube_perf.c's frame-budget breakdown: one comparison table across every
run variant found, then each variant's own min/max/avg/median/p95 detail -
the same shape suite_cube_perf.c would write itself if this project had a
mounted filesystem to write to (it does not: no SPIFFS partition exists, and
the SD card is only mounted transiently by POST's own probe, not held open
for general use - see main/selftest/post.c). ESP_LOGI is the only persistent
output a DEVICE_BUILD suite has here, the same as suite_sand.c's own
frame-budget tests, so this generates the report on the host from a
captured serial log instead, mirroring
main/apps/sand/tools/report_performance.py's own reason for existing.

Each run's label states its own configuration - e.g.
"hud_on_partial_on_interlace_off" - rather than this script needing to know
what any particular name means, so a new variant added to the suite shows
up in the report with no changes needed here.

Usage:
    python main/apps/render_lab/tools/report_cube_perf.py <raw_capture.txt> <out.md>

Exit 2 means the capture has no run in it to report on.

Lives under render_lab because report_cube_perf.sh is its only caller -
same convention as sand's own tools/ folder: deleting the app takes its
tooling with it.
"""
import argparse
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools" / "perf"))
from phase_report import phase_stats, write_phase_report  # noqa: E402

# "I (11692) cube_perf: === CUBE PERF hud_on_partial_on_interlace_off (274 frames over 10s) ==="
HEADER_RE = re.compile(
    r"cube_perf:\s*===\s*CUBE PERF\s+(?P<label>\S+)\s+"
    r"\((?P<frames>\d+)\s+frames over\s+(?P<seconds>\d+)s\)\s*==="
)

# "I (...) cube_perf: Logic:   min=766us max=...us avg=766us med=772us p95=...us (2.1%)"
# The trailing parenthetical differs per phase (Total's is an fps triplet,
# the rest are a percentage of Total) and is not needed for the table - the
# five numbers before it are.
PHASE_RE = re.compile(
    r"cube_perf:\s*(?P<phase>Total|Logic|Raster|HUD|Present):\s*"
    r"min=(?P<min>\d+)us\s+max=(?P<max>\d+)us\s+avg=(?P<avg>\d+)us\s+"
    r"med=(?P<med>\d+)us\s+p95=(?P<p95>\d+)us"
)

PHASE_ORDER = ["Total", "Logic", "Raster", "HUD", "Present"]


def parse_capture(capture_path: str):
    with open(capture_path, "r", errors="replace") as f:
        lines = f.read().splitlines()

    runs = {}          # label -> {"frames": int, "seconds": int, "phases": {phase: {...}}}
    current_label = None

    for line in lines:
        hm = HEADER_RE.search(line)
        if hm:
            current_label = hm.group("label")
            runs[current_label] = {
                "frames": int(hm.group("frames")),
                "seconds": int(hm.group("seconds")),
                "phases": {},
            }
            continue

        pm = PHASE_RE.search(line)
        if pm and current_label is not None:
            runs[current_label]["phases"][pm.group("phase")] = phase_stats(pm)

    return runs


# A label the suite itself generates, e.g. "hud_on_partial_on_interlace_off" -
# parsed back into its three toggles for the configuration table below
# rather than making the reader decode the underscores themselves.
LABEL_RE = re.compile(
    r"hud_(?P<hud>on|off)_partial_(?P<partial>on|off)_interlace_(?P<interlace>on|off)"
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_path", help="Raw capture from scripts/device/device.py selftest")
    parser.add_argument("out_path", help="Markdown file to write")
    args = parser.parse_args()

    runs = parse_capture(args.capture_path)
    if not runs:
        # A header-and-timestamp report over an empty table reads like a
        # clean run of nothing. A report of nothing is not a report.
        print(f"{args.capture_path} has no CUBE PERF run headers in it - the "
              "suite never ran, or the device crashed before it reached one.",
              file=sys.stderr)
        return 2

    lines = []
    lines.append("# Cube App Performance Report")
    lines.append("")
    lines.append(f"Captured: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}")
    lines.append(f"Source: `{args.capture_path}`")
    lines.append("")

    labels = list(runs.keys())

    lines.append("## Configuration")
    lines.append("")
    lines.append("| Run | HUD | Partial updates | Interlace |")
    lines.append("|---|:---:|:---:|:---:|")
    for label in labels:
        lm = LABEL_RE.fullmatch(label)
        if lm:
            lines.append(f"| `{label}` | {lm.group('hud')} | "
                         f"{lm.group('partial')} | {lm.group('interlace')} |")
        else:
            # An older or hand-named label that does not follow the
            # hud_X_partial_X_interlace_X convention - still worth a
            # row, just without a decoded configuration to show.
            lines.append(f"| `{label}` | ? | ? | ? |")
    lines.append("")

    write_phase_report(args.out_path, lines, runs, labels, PHASE_ORDER,
                       lambda label, run: f"## `{label}` ({run['frames']} frames over {run['seconds']}s)")

    print(f"{len(runs)} run(s) -> {args.out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
