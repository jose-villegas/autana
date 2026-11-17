#!/usr/bin/env python3
"""A perf_compare table of a capture's PERF TARGET rows: each row's measured microseconds.

Usage: perf_target_table.py CAPTURE TABLE, as perf_compare's table command
`python3 launcher/tools/perf/perf_target_table.py @CAPTURE@ @TABLE@` for a
suite that reports only PERF TARGET lines.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/device"))
from device_report import parse_perf_targets, parse_unpegged_targets, read_capture_text  # noqa: E402


def render(text):
    """The markdown table of `text`'s PERF TARGET rows, pegged or not, the last of each name kept."""
    targets = parse_perf_targets(text)[0] + parse_unpegged_targets(text)
    rows = {target["name"]: target["measured"] for target in targets}
    if not rows:
        raise ValueError("capture has no PERF TARGET rows")
    return "\n".join(["| Name | Measured us |", "|---|---:|", *(f"| {name} | {us} |" for name, us in rows.items())]) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("capture", type=Path)
    parser.add_argument("table", type=Path)
    args = parser.parse_args(argv)
    args.table.write_text(render(read_capture_text(args.capture)), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
