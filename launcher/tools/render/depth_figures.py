#!/usr/bin/env python3
"""Depth precision before and after the per-frame depth fit, seen on a scene's
camera path: the same consecutive frames drawn with the depth encoding scaled
by the clip plane's near_z (render.depth_fit=0) and by the fitted scale
(render.depth_fit=1). launcher/tools/render/render_doc_images.sh runs it.

    python launcher/tools/render/depth_figures.py --scene SCENE.scene.toml --object NAME [--object NAME ...]
        --camera NAME --at-ms MS --out PNG --work DIR

It writes --out: a row per setting of FRAMES consecutive frames from --at-ms,
each cut to the window where the fit removes the most flips (a pixel that
changes for one frame and back: surfaces fighting), enlarged without
smoothing.
"""

import argparse
import pathlib
import subprocess

import numpy as np
from PIL import Image
from render_compare import still_sheet

ROOT = pathlib.Path(__file__).resolve().parents[3]
VIEWER = ROOT / "launcher/tools/render/scene_viewer.sh"
FRAME_MS = 16  # one host frame, as the viewer steps by default
FRAMES = 6  # consecutive frames per row: enough to see a fight come and go
WINDOW = (160, 96)  # the cut, in rendered pixels: wide enough to read the place
ZOOM = 2  # the cut's enlargement
CHANGE = 24  # summed RGB difference that counts a pixel as changed
SETTINGS = (("depth scaled by near_z", 0), ("depth scaled by the fit", 1))


def render(args, frame, fit):
    """The picture after `frame` frames, as RGB."""
    out = args.work / f"fit{fit}-{frame}.bmp"
    objects = [flag for name in args.object for flag in ("--object", name)]
    command = ["sh", str(VIEWER), str(args.scene), *objects, "--camera", args.camera,
               "--frames", str(frame), "--dt", str(FRAME_MS), "--set", f"render.depth_fit={fit}", "-o", str(out)]
    with open(args.work / f"fit{fit}-{frame}.log", "w") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    return np.asarray(Image.open(out).convert("RGB")).astype(np.int16)


def flips(frames):
    """Per pixel, how often a frame differs from both neighbours while they agree."""
    count = np.zeros(frames[0].shape[:2], int)
    for before, now, after in zip(frames, frames[1:], frames[2:]):
        apart = lambda a, b: np.abs(a - b).sum(2)
        count += (apart(now, before) > CHANGE) & (apart(now, after) > CHANGE) & (apart(before, after) <= CHANGE // 2)
    return count


def busiest(count):
    """The top-left corner of the WINDOW holding the most flips."""
    width, height = WINDOW
    total = count.cumsum(0).cumsum(1)
    total = np.pad(total, ((1, 0), (1, 0)))
    sums = total[height:, width:] - total[:-height, width:] - total[height:, :-width] + total[:-height, :-width]
    y, x = np.unravel_index(np.argmax(sums), sums.shape)
    return int(x), int(y)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scene", required=True, type=pathlib.Path)
    parser.add_argument("--object", required=True, action="append", help="a renderer to draw; repeat for more")
    parser.add_argument("--camera", required=True)
    parser.add_argument("--at-ms", required=True, type=int, help="the first frame's time along the camera path")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    args.work.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    first = args.at_ms // FRAME_MS
    rows = {fit: [render(args, first + k, fit) for k in range(FRAMES)] for _, fit in SETTINGS}
    removed = np.maximum(flips(rows[SETTINGS[0][1]]) - flips(rows[SETTINGS[1][1]]), 0)
    x, y = busiest(removed)
    width, height = WINDOW
    sheet = []
    for label, fit in SETTINGS:
        cuts = [Image.fromarray(frame[y:y + height, x:x + width].astype(np.uint8)) for frame in rows[fit]]
        sheet.append([(f"{label}, {(first + k) * FRAME_MS / 1000:.3f} s",
                       cut.resize((width * ZOOM, height * ZOOM), Image.NEAREST)) for k, cut in enumerate(cuts)])
    still_sheet(sheet).save(args.out, optimize=True)


if __name__ == "__main__":
    main()
