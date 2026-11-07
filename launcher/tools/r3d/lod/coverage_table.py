"""What the level pick's coverage cap trades, over a scene's camera path.

    python launcher/tools/r3d/lod/coverage_table.py --script HOST_RENDER.sh \\
        --render LABEL "RENDERER ARGS" [--render ...] [--caps 0,100,...] [--doc PAGE.md] [--work DIR]

Builds the scene's host renderer once (the script's --build-only) and runs it
along each --render path once per cap, set by render.lod_coverage_permille; a cap of
0 keeps every group fine, the finest level, which every other cap is scored
against. Per path and cap it reports the blocks popping beyond the finest
level's own frame-to-frame change (render_temporal.py's measure), the mean
difference from the finest level, and the triangles submitted, mean and 95th
percentile, against the finest level's. With --doc it writes the table into
that page's `lod-coverage` block; otherwise it prints it.
"""
import argparse
import csv
import pathlib
import shlex
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
sys.path.insert(0, str(ROOT / "launcher/tools"))
from check_avi import read_video  # noqa: E402
from generated_blocks import replace_block  # noqa: E402
from r3d.lod.pick import PERMILLE, UNCAPPED  # noqa: E402
from render_temporal import temporal_change  # noqa: E402

TUNABLE = "render.lod_coverage_permille"
FINEST = 0
# Every cap the table lists.
CAPS = (UNCAPPED, 500, 250, 100)
PERCENTILE = 95
BLOCK = "lod-coverage"


def build(script, work):
    run = subprocess.run(["sh", script, "-o", str(work), "--build-only"], cwd=ROOT, capture_output=True, text=True)
    built = [line[len("built "):] for line in run.stdout.splitlines() if line.startswith("built ")]
    if run.returncode or not built:
        raise SystemExit(f"{script} --build-only failed:\n{run.stdout[-2000:]}{run.stderr[-2000:]}")
    return built[-1]


def render(renderer, args, cap, out):
    """The path's video and per-frame counts at `cap`, in `out`."""
    out.mkdir(parents=True, exist_ok=True)
    common = shlex.split(args) + ["--set", f"{TUNABLE}={cap}"]
    for extra in (["-o", str(out / "frame.bmp"), "--video", str(out / "path.avi")],
                  ["--view", "cull", "--counts", str(out / "counts.csv"), "-o", str(out / "cull.bmp")]):
        run = subprocess.run([renderer, *common, *extra], cwd=ROOT, capture_output=True, text=True)
        if run.returncode:
            raise SystemExit(f"{renderer} {' '.join(common + extra)} failed:\n{run.stderr[-2000:]}")
    with (out / "counts.csv").open(newline="") as handle:
        return [int(row["triangles_submitted"]) for row in csv.DictReader(handle)]


def scored(finest, arm):
    """(blocks popping %, mean difference /255) of `arm`'s video against `finest`'s."""
    fps_a, frames_a = read_video(finest)
    fps_b, frames_b = read_video(arm)
    if fps_a != fps_b:
        raise ValueError("videos need equal frame rates")
    popping, difference, previous = [], [], None
    for a, b in zip(frames_a, frames_b, strict=True):
        difference.append(float(np.abs(a.astype(float) - b.astype(float)).mean()))
        if previous is not None:
            popping.append(temporal_change(previous[0], a, previous[1], b)[1])
        previous = (a, b)
    return float(np.mean(popping)), float(np.mean(difference))


def summary(triangles):
    return float(np.mean(triangles)), float(np.percentile(triangles, PERCENTILE))


def cap_text(cap):
    return "none" if cap == UNCAPPED else f"{100 * cap / PERMILLE:g}%"


def table(renderer, renders, caps, work):
    lines = ["| Path | Cap | Blocks popping | Mean difference /255 | Triangles, mean | Triangles, p95 |",
             "|---|---|---|---|---|---|"]
    for label, args in renders:
        finest_dir = work / label / str(FINEST)
        mean0, p0 = summary(render(renderer, args, FINEST, finest_dir))
        for cap in caps:
            arm = work / label / str(cap)
            mean, p = summary(render(renderer, args, cap, arm))
            popping, difference = scored(finest_dir / "path.avi", arm / "path.avi")
            (arm / "path.avi").unlink()
            lines.append(f"| {label} | {cap_text(cap)} | {popping:.4f}% | {difference:.3f} "
                         f"| {100 * (mean / mean0 - 1):+.1f}% | {100 * (p / p0 - 1):+.1f}% |")
        (finest_dir / "path.avi").unlink()
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--script", required=True, help="the scene's host-render script, from the checkout root")
    parser.add_argument("--render", nargs=2, action="append", required=True, metavar=("LABEL", "ARGS"))
    parser.add_argument("--caps", default=",".join(map(str, CAPS)), help="caps in thousandths of the picture height")
    parser.add_argument("--doc", help=f"page whose `{BLOCK}` block takes the table")
    parser.add_argument("--work", help="keep the renders here, not in a temporary folder")
    options = parser.parse_args()
    caps = [int(cap) for cap in options.caps.split(",")]
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(options.work or temporary)
        body = table(build(options.script, work / "build"), options.render, caps, work)
    if options.doc:
        replace_block(options.doc, BLOCK, "\n" + body + "\n")
    else:
        print(body)


if __name__ == "__main__":
    main()
