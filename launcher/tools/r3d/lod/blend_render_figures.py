"""The level switch the blend softens, on a scene's camera path: Levels-of-Detail.md's render figures.

    python launcher/tools/r3d/lod/blend_render_figures.py --script HOST_RENDER.sh --render "RENDERER ARGS"         --morph MESH [--morph ...] --out DIR

Renders the path at the finest level, with the pick's switches drawn as they are (lod.blend switch) and blended
(lod.blend on, which dithers a mesh without morph targets), and blended again with each --morph mesh given its
morph targets (morph_packs.py), then finds the window where a switch pops most: the
frame and the square whose frame-to-frame change differs most from the finest level's, on luma averaged over the
screen door's tile (morph_table.py's worst pop). lod-blend-pop.gif plays that square through the frames around
it, the finest level beside each arm; lod-blend-pop.png is the frame before and the frame of the pop, with each arm's difference from the finest level.
"""
import argparse
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parents[1] / "render"))
from check_avi import read_video  # noqa: E402
from coverage_table import ROOT, build  # noqa: E402
from docs.figures import save_gif, save_png  # noqa: E402
from docs.producer import Producer  # noqa: E402
from morph_packs import morph_packs  # noqa: E402
from morph_table import AVERAGED, FINEST, POP_SQUARE, SWITCHES, blended, worst_pop  # noqa: E402
from render_compare import captioned  # noqa: E402

PRODUCER = Producer(area="render", needs=("host", "bakes"),
                    run=("--script", "launcher/main/apps/render_lab/tools/render_lab_render_host.sh",
                         "--render", "--scene sponza-fitted-full --no-hud --frames 1101 --dt 33",
                         "--morph", "sponza.atrium_fitted_full", "--out", "{out}"),
                    outputs=("lod-blend-pop.gif", "lod-blend-pop.png"))

ARMS = (("finest", FINEST, False), ("switch", SWITCHES, False), ("dither", blended("on", None), False),
        ("morph", blended("on", None), True))  # (name, settings, reads the morph packs)
CROP = POP_SQUARE  # pixels a side of the square shown
ZOOM = 4  # each shown pixel's side in the figures
AROUND = 8  # frames shown before and after the pop
FRAME_MS = 100  # each GIF frame, slowed so the pop can be seen
DIFFERENCE_GAIN = 4  # the difference panels' brightness per level of difference


def render(renderer, args, sets, out, assets=None):
    """The path's video under `sets`, from the packs in `assets` when given."""
    command = [renderer, *shlex.split(args), *(item for value in sets for item in ("--set", value)),
               "-o", str(out / "frame.bmp"), "--video", str(out / "path.avi")]
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, AUTANA_ASSET_DIR=str(assets)) if assets else None
    run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=env)
    if run.returncode:
        raise SystemExit(f"{' '.join(command)} failed:\n{run.stderr[-2000:]}")
    return list(read_video(out / "path.avi")[1])


def crop(frame, top, left):
    square = np.asarray(frame)[top:top + CROP, left:left + CROP]
    return Image.fromarray(square.astype(np.uint8)).resize((CROP * ZOOM, CROP * ZOOM), Image.NEAREST)


def difference(frame, finest, top, left):
    gap = np.abs(np.asarray(frame, dtype=float) - np.asarray(finest, dtype=float)).mean(axis=2)
    grey = np.clip(gap * DIFFERENCE_GAIN, 0, 255).astype(np.uint8)
    return crop(np.repeat(grey[..., None], 3, axis=2), top, left)


def row(pictures):
    canvas = Image.new("RGB", (sum(p.width for p in pictures), max(p.height for p in pictures)))
    x = 0
    for picture in pictures:
        canvas.paste(picture, (x, 0))
        x += picture.width
    return canvas


def column(pictures):
    canvas = Image.new("RGB", (max(p.width for p in pictures), sum(p.height for p in pictures)))
    y = 0
    for picture in pictures:
        canvas.paste(picture, (0, y))
        y += picture.height
    return canvas


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--script", required=True)
    parser.add_argument("--render", required=True)
    parser.add_argument("--morph", action="append", required=True, metavar="MESH",
                        help="a pack entry id the morph arm gives morph targets")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as scratch:
        work = args.work or pathlib.Path(scratch)
        renderer = build(args.script, work)
        packs = morph_packs(args.morph, work / "morph-packs")
        videos = {name: render(renderer, args.render, sets, work / name, packs if morph else None)
                  for name, sets, morph in ARMS}
    finest, switch = videos["finest"], videos["switch"]
    _, t, top, left = worst_pop(finest, switch, AVERAGED)
    first, end = max(t - AROUND, 0), min(t + AROUND + 1, len(finest))
    frames = [row([captioned(crop(video[i], top, left), f"{name} {i}") for name, video in videos.items()])
              for i in range(first, end)]
    args.out.mkdir(parents=True, exist_ok=True)
    save_gif(frames, args.out / "lod-blend-pop.gif", FRAME_MS)
    sheet = []
    for name, video in videos.items():
        if video is finest:
            continue
        sheet.append(row([captioned(crop(video[t - 1], top, left), f"{name}, frame {t - 1}"),
                          captioned(crop(video[t], top, left), f"{name}, frame {t}"),
                          captioned(difference(video[t], finest[t], top, left),
                                    f"{name} - finest, x{DIFFERENCE_GAIN}")]))
    save_png(column(sheet), args.out / "lod-blend-pop.png")


if __name__ == "__main__":
    main()
