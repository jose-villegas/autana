"""What the geomorph blend buys at level switches, over a scene's camera path.

    python launcher/tools/r3d/lod/morph_table.py --script HOST_RENDER.sh \\
        --render LABEL "RENDERER ARGS" [--render ...] [--assets VARIANT=DIR ...] [--morph MESH ...] [--ramp PX ...]
        [--doc PAGE.md] [--work DIR]

Builds the scene's host renderer once (the script's --build-only), or takes
one --renderer already built, and runs
each --render path in its arms: the finest level (render.lod_coverage_permille
0), the pick's switches drawn as they are, and blended (render.lod_blend) once
per --assets variant, a folder of packs the renderer reads instead of its own
(AUTANA_ASSET_DIR): meshes whose packs carry morph targets morph, the rest
dither. --morph adds a variant, morph, with those pack entries given morph
targets from their locked bakes (morph_packs.py). Each blend runs once per --ramp (render.lod_blend_centipixels), or at
its setting. Per path and arm it
reports the blocks popping beyond the finest level's own frame-to-frame
change and the mean difference from the finest level (coverage_table.py's
scores), the share of transformed vertices blended, and the vertices whose
targets' level was itself blending, with the most pixels that put them off
the surface they meet. Beside the raw scores it gives each over luma averaged
on the screen door's 4x4 tile, where a dither averages to its blend: the mean
difference, and each arm's peak change in the square and frames where the
switches pop most, taken only next to a frame whose coarse clusters drawn
change: a pop anywhere else is no level switch's. With --doc it writes the table into the page's
generated block.
"""
import argparse
import csv
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

import re

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "render"))
from check_avi import read_video  # noqa: E402
from coverage_table import ROOT, build, scored  # noqa: E402
from generated_blocks import replace_block  # noqa: E402
from morph_packs import morph_packs  # noqa: E402
from render_compare import _box_mean  # noqa: E402
from render_temporal import TEMPORAL_BLOCK_SIDE  # noqa: E402

SETTINGS_HEADER = ROOT / "launcher/main/render/settings/render_settings.h"
BLEND_PREFIX = "RENDER_LOD_BLEND_"


def blends():
    """render_lod_blend_t's values by lower-case name, read from its header."""
    body = re.search(r"typedef enum \{(.*?)\} render_lod_blend_t;", SETTINGS_HEADER.read_text(), re.S).group(1)
    names = re.findall(BLEND_PREFIX + r"(\w+)", body)
    return {name.lower(): value for value, name in enumerate(names)}


BLEND = blends()
SWITCHES = (f"render.lod_blend={BLEND['switch']}",)
FINEST = ("render.lod_coverage_permille=0",) + SWITCHES
CENTI = 100.0


def blended(name, ramp):
    """The settings of blend `name` over a ramp of `ramp` pixels, or its setting's when None."""
    ramped = () if ramp is None else (f"render.lod_blend_centipixels={round(ramp * CENTI)}",)
    return (f"render.lod_blend={BLEND[name]}",) + ramped
BLOCK = "lod-morph"
POP_SQUARE = 64  # pixels a side of the square a pop is measured over
POP_AROUND = 15  # frames either side of the switches' worst pop over which each arm's own peak there is taken
SWITCH_NEAR = 1  # frames from a change in the coarse clusters drawn within which a pop counts as a switch's
RAW, AVERAGED = 1, TEMPORAL_BLOCK_SIDE  # luma per pixel, or over the screen door's tile, so a dither averages out


def luma(frame, side):
    values = np.asarray(frame, dtype=float).mean(axis=2)
    return values if side == 1 else _box_mean(values, side)


def square_changes(finest, arm, side):
    """(mean difference from the finest, map) per frame from the second: the map holds, per POP_SQUARE square, the
    mean of the arm's frame-to-frame change beyond the finest's, on luma averaged over `side` pixels a side."""
    previous = None
    for a, b in zip(finest, arm, strict=True):
        la, lb = luma(a, side), luma(b, side)
        if previous is not None:
            extra = np.abs((lb - previous[1]) - (la - previous[0]))
            yield float(np.abs(la - lb).mean()), _box_mean(extra, POP_SQUARE - side + 1)
        previous = (la, lb)


def switch_frames(rows):
    """The frames whose coarse clusters drawn differ from the frame before's: where a level switch can show."""
    return [f for f in range(1, len(rows)) if rows[f]["clusters_coarse"] != rows[f - 1]["clusters_coarse"]]


def worst_pop(finest, arm, side, switches=None):
    """(change /255, frame, top, left) of the square and frame where `arm` pops most beyond the finest; with
    `switches`, only within SWITCH_NEAR frames of one of them, as a pop elsewhere is no level switch's."""
    best = (-1.0, 0, 0, 0)
    for t, (_, squares) in enumerate(square_changes(finest, arm, side), start=1):
        if not near_switch(t, switches):
            continue
        top, left = np.unravel_index(int(np.argmax(squares)), squares.shape)
        if squares[top, left] > best[0]:
            best = (float(squares[top, left]), t, int(top), int(left))
    return best


def frames(path):
    return read_video(path)[1]


def near_switch(frame, switches):
    return switches is None or any(abs(frame - f) <= SWITCH_NEAR for f in switches)


def pop_at(finest, arm, side, at, switches):
    """(mean difference /255, the arm's peak change /255 in the square of `at` within POP_AROUND frames of it, at
    frames next to one of `switches`)."""
    _, t, top, left = at
    differences, peak = [], 0.0
    for frame, (difference, squares) in enumerate(square_changes(finest, arm, side), start=1):
        differences.append(difference)
        if abs(frame - t) <= POP_AROUND and near_switch(frame, switches):
            peak = max(peak, float(squares[top, left]))
    return float(np.mean(differences)), peak


def render(renderer, args, sets, assets, out):
    """The path's video, and its per-frame cull counts, in `out`."""
    out.mkdir(parents=True, exist_ok=True)
    common = shlex.split(args) + [item for value in sets for item in ("--set", value)]
    env = dict(os.environ, AUTANA_ASSET_DIR=str(assets)) if assets else None
    for extra in (["-o", str(out / "frame.bmp"), "--video", str(out / "path.avi")],
                  ["--view", "cull", "--counts", str(out / "counts.csv"), "-o", str(out / "cull.bmp")]):
        run = subprocess.run([renderer, *common, *extra], cwd=ROOT, capture_output=True, text=True, env=env)
        if run.returncode:
            raise SystemExit(f"{renderer} {' '.join(common + extra)} failed:\n{run.stderr[-2000:]}")
    with (out / "counts.csv").open(newline="") as handle:
        return list(csv.DictReader(handle))


def blend_shares(rows):
    """(blended vertices %, frames with a stacked blend %, stacked vertices %, most stacked px)."""
    total = sum(int(r["vertices_transformed"]) for r in rows)
    blended = sum(int(r["vertices_blended"]) for r in rows)
    stacked = sum(int(r["vertices_stacked"]) for r in rows)
    frames = sum(int(r["vertices_stacked"]) > 0 for r in rows)
    most = max((int(r["stacked_centipixels"]) for r in rows), default=0) / CENTI
    share = (lambda n: 100.0 * n / total) if total else (lambda n: 0.0)
    return share(blended), 100.0 * frames / max(len(rows), 1), share(stacked), most


def arms(variants, ramps):
    """(label, settings, assets) per arm: the switches, then each variant blended at each ramp."""
    out = [("switches", SWITCHES, variants[0][1])]
    for ramp in ramps:
        at = "" if ramp is None else f" {ramp:g} px"
        out += [(f"blend{at}" + (f", {name}" if len(variants) > 1 else ""), blended("on", ramp), assets)
                for name, assets in variants]
    return out


def table(renderer, renders, variants, ramps, work):
    lines = ["| Path | Arm | Blocks popping | Mean difference /255 | 4×4 | Worst switch pop /255 | 4×4 "
             "| Vertices blended | Frames stacked | Vertices stacked | Stacked, most px |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for label, args in renders:
        first = variants[0][1]
        finest = work / label / "finest"
        render(renderer, args, FINEST, first, finest)
        worst, switches = {}, None
        for arm, sets, assets in arms(variants, ramps):
            out = work / label / re.sub(r"[^\w.]+", "-", arm)
            rows = render(renderer, args, sets, assets, out)
            popping, difference = scored(finest / "path.avi", out / "path.avi")
            pops = []
            # The switches come first: the frames their pick switches at, and their worst pop there, are where
            # every arm is measured.
            switches = switch_frames(rows) if switches is None else switches
            for side in (RAW, AVERAGED):
                if side not in worst:
                    worst[side] = worst_pop(frames(finest / "path.avi"), frames(out / "path.avi"), side, switches)
                pops.append(pop_at(frames(finest / "path.avi"), frames(out / "path.avi"), side, worst[side],
                                   switches))
            (out / "path.avi").unlink()
            vertices, frames_stacked, stacked, most = blend_shares(rows)
            lines.append(f"| {label} | {arm} | {popping:.4f}% | {difference:.3f} | {pops[1][0]:.3f} "
                         f"| {pops[0][1]:.2f} | {pops[1][1]:.2f} | {vertices:.1f}% | {frames_stacked:.1f}% "
                         f"| {stacked:.2f}% | {most:.2f} |")
        (finest / "path.avi").unlink()
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    built = parser.add_mutually_exclusive_group(required=True)
    built.add_argument("--script", help="the scene's host-render script, from the checkout root")
    built.add_argument("--renderer", help="a host renderer that script already built")
    parser.add_argument("--render", nargs=2, action="append", required=True, metavar=("LABEL", "ARGS"))
    parser.add_argument("--assets", action="append", default=[], metavar="VARIANT=DIR",
                        help="packs to read instead of the build's own; the first also draws the finest and switches")
    parser.add_argument("--morph", action="append", default=[], metavar="MESH",
                        help="a pack entry id the morph variant gives morph targets")
    parser.add_argument("--ramp", type=float, action="append", default=[], metavar="PX",
                        help="each blend's ramp, in pixels; without one, the setting's")
    parser.add_argument("--doc", help=f"page whose `{BLOCK}` block takes the table")
    parser.add_argument("--work", help="keep the renders here, not in a temporary folder")
    options = parser.parse_args()
    variants = [tuple(item.split("=", 1)) for item in options.assets] or [("dither", None)]
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(options.work or temporary)
        if options.morph:
            variants.append(("morph", morph_packs(options.morph, work / "morph-packs")))
        renderer = options.renderer or build(options.script, work / "build")
        body = table(renderer, options.render, variants, options.ramp or [None], work)
    print(body)
    if options.doc:
        replace_block(options.doc, BLOCK, "\n" + body + "\n")


if __name__ == "__main__":
    main()
