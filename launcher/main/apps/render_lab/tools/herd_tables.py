"""The herd suite's timing table for perf_compare: run_herd_perf_suite's step and frame means, by size and quarter.

    python launcher/main/apps/render_lab/tools/herd_tables.py --perf-table CAPTURE TABLE

A row is the mean, over every run in the capture, of one logged mean: `herd-N step`, `herd-N frame QUARTER`, and
for the fixed-tier test `herd-N ANIM step` and `herd-N ANIM frame QUARTER`.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define  # noqa: E402

PREFIX = read_define(pathlib.Path(__file__).resolve().parents[1] / "tests/suite_herd_perf.c", "HERD_PERF_PREFIX")
QUARTER = read_define(ROOT / "launcher/test/frame_profile.h", "FRAME_PROFILE_QUARTER")
FIELD = re.compile(r"(\w+)=(\S+)")
READINGS = (("step_mean_us", "step"), ("frame_mean_us", "frame"))


def perf_table(text):
    """Each logged step and frame mean, averaged over the capture's runs, in integer microseconds."""
    readings = {}
    for line in text.splitlines():
        if PREFIX not in line:
            continue
        fields = dict(FIELD.findall(line.split(PREFIX, 1)[1]))
        for field, part in READINGS:
            if field not in fields:
                continue
            name = " ".join(filter(None, (f"herd-{fields['n']}", fields.get("anim"), part,
                                          fields.get(QUARTER.removesuffix("=")))))
            readings.setdefault(name, []).append(int(fields[field]))
    if not readings:
        raise ValueError(f"no {PREFIX.strip()} timing lines")
    rows = [f"| {name} | {round(sum(values) / len(values))} |" for name, values in readings.items()]
    return "\n".join(["| Test | Mean us |", "|---|---:|", *rows]) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--perf-table", nargs=2, type=pathlib.Path, required=True, metavar=("CAPTURE", "TABLE"))
    args = parser.parse_args(argv)
    capture, table = args.perf_table
    try:
        table.write_text(perf_table(capture.read_text(encoding="utf-8")), encoding="utf-8")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
