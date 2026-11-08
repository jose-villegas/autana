"""What the geomorph blend buys at level switches, over a scene's camera path.

    python launcher/tools/r3d/lod/morph_table.py --script HOST_RENDER.sh \\
        --render LABEL "RENDERER ARGS" [--render ...] [--assets VARIANT=DIR ...] [--doc PAGE.md] [--work DIR]

Builds the scene's host renderer once (the script's --build-only), or takes
one --renderer already built, and runs
each --render path in three arms: the finest level (render.lod_coverage_permille
0), the pick's switches drawn as they are and each blend (render.lod_blend,
the values of render_lod_blend_t), once per --assets variant, a folder of packs the
renderer reads instead of its own (AUTANA_ASSET_DIR). Per path and arm it
reports the blocks popping beyond the finest level's own frame-to-frame
change and the mean difference from the finest level (coverage_table.py's
scores), the share of transformed vertices blended, and the vertices whose
targets' level was itself blending, with the most pixels that put them off
the surface they meet. With --doc it writes the table into the page's
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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from coverage_table import ROOT, build, scored  # noqa: E402
from generated_blocks import replace_block  # noqa: E402

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
BLENDED = (f"render.lod_blend={BLEND['morph']}",)
CENTI = 100.0
BLOCK = "lod-morph"


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
    morphed = sum(int(r["vertices_morphed"]) for r in rows)
    stacked = sum(int(r["vertices_stacked"]) for r in rows)
    frames = sum(int(r["vertices_stacked"]) > 0 for r in rows)
    most = max((int(r["stacked_centipixels"]) for r in rows), default=0) / CENTI
    share = (lambda n: 100.0 * n / total) if total else (lambda n: 0.0)
    return share(morphed), 100.0 * frames / max(len(rows), 1), share(stacked), most


def table(renderer, renders, variants, work):
    lines = ["| Path | Arm | Blocks popping | Mean difference /255 | Vertices blended | Frames stacked "
             "| Vertices stacked | Stacked, most px |",
             "|---|---|---|---|---|---|---|---|"]
    for label, args in renders:
        first = variants[0][1]
        finest = work / label / "finest"
        render(renderer, args, FINEST, first, finest)
        arms = [("switches", SWITCHES, first)] + [(f"blend, {name}", BLENDED, assets) for name, assets in variants]
        for arm, sets, assets in arms:
            out = work / label / arm.replace(", ", "-")
            rows = render(renderer, args, sets, assets, out)
            popping, difference = scored(finest / "path.avi", out / "path.avi")
            (out / "path.avi").unlink()
            blended, frames, stacked, most = blend_shares(rows)
            lines.append(f"| {label} | {arm} | {popping:.4f}% | {difference:.3f} | {blended:.1f}% | {frames:.1f}% "
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
    parser.add_argument("--doc", help=f"page whose `{BLOCK}` block takes the table")
    parser.add_argument("--work", help="keep the renders here, not in a temporary folder")
    options = parser.parse_args()
    variants = [tuple(item.split("=", 1)) for item in options.assets] or [("baked", None)]
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(options.work or temporary)
        renderer = options.renderer or build(options.script, work / "build")
        body = table(renderer, options.render, variants, work)
    print(body)
    if options.doc:
        replace_block(options.doc, BLOCK, "\n" + body + "\n")


if __name__ == "__main__":
    main()
