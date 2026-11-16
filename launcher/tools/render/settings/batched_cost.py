#!/usr/bin/env python3
"""Write what batching saves on the board into docs/render/Render-Settings.md,
from the frame-profile captures in docs/render/data/batched-board.log: the
suite's frame_profile rows, each capture headed by a line
`=== render.batched=<0|1> run <n> ===`, the two settings interleaved run by run.

    python launcher/tools/render/settings/batched_cost.py [--check]

A row and quarter is the mean of its captures' means and p95s. Every scene and
quarter must appear under both settings, in the same number of runs.
"""
import collections
import pathlib
import re
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
for folder in ("launcher/tools", "launcher/tools/render", "launcher/tools/perf"):
    sys.path.insert(0, str(ROOT / folder))
from doc_tables import markdown  # noqa: E402
from frame_profile import PROFILE_RE  # noqa: E402
from generated_blocks import block_main  # noqa: E402

CAPTURE = ROOT / "docs/render/data/batched-board.log"
PAGE = ROOT / "docs/render/Render-Settings.md"
BLOCK = "render-settings-batched"
COMMAND = "python launcher/tools/render/settings/batched_cost.py --check"
MARK = re.compile(r"^=== render\.batched=([01]) run (\d+) ===$", re.M)
US_PER_MS = 1000


def means(text):
    """{(row, quarter): {batched: (mean us, p95 us)}} over every run of each setting."""
    found = collections.defaultdict(lambda: collections.defaultdict(list))
    runs = collections.defaultdict(set)
    parts = MARK.split(text)
    for batched, run, body in zip(parts[1::3], parts[2::3], parts[3::3]):
        runs[batched].add(run)
        for record in PROFILE_RE.finditer(body):
            found[(record["row"], record["quarter"])][batched].append((int(record["mean"]), int(record["p95"])))
    if set(runs) != {"0", "1"} or len(runs["0"]) != len(runs["1"]):
        raise ValueError(f"{CAPTURE.name}: needs the same number of runs with render.batched 0 and 1")
    if not found:
        raise ValueError(f"{CAPTURE.name}: no frame_profile rows")
    result = {}
    for key, settings in found.items():
        if set(settings) != {"0", "1"}:
            raise ValueError(f"{CAPTURE.name}: {' '.join(key)} lacks a setting")
        result[key] = {batched: tuple(statistics.mean(values) for values in zip(*rows))
                       for batched, rows in settings.items()}
    return result


def render(text):
    rows = []
    for (row, quarter), settings in sorted(means(text).items()):
        on, off = settings["1"], settings["0"]
        rows.append((row, quarter, *(f"{value / US_PER_MS:.1f}" for value in (on[0], off[0], off[0] - on[0], on[1], off[1]))))
    return markdown(["Scene", "Quarter", "Mean ms, on", "Mean ms, off", "Off - on", "p95 ms, on", "p95 ms, off"],
                    rows).rstrip()


def main(argv=None):
    return block_main(PAGE, BLOCK, COMMAND, lambda: render(CAPTURE.read_text(encoding="utf-8")),
                      __doc__.split("\n\n")[0], argv)


if __name__ == "__main__":
    sys.exit(main())
