#!/usr/bin/env python3
"""What a scene's thin curves add, seen on its camera path: the same locked bakes drawn with the view's curves
off and on (the scene viewer's --curves). launcher/tools/render/render_doc_images.sh runs it.

    python launcher/tools/render/curves_figures.py --scene SCENE.scene.toml --object NAME [--object NAME ...]
        --camera NAME --out PNG --work DIR

Every --object is drawn. It writes --out: the frame
where the curves change the most pixels, cut to the WINDOW holding most of them, without curves above with
curves, enlarged without smoothing.
"""

import os
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image
from depth_figures import busiest, scene_parser
from render_compare import still_sheet

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figures import ASSET_DIR_ENV, save_png  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402
from render.check_avi import read_video  # noqa: E402

FRAME_MS = 1000  # one frame a second along the camera path
FRAMES = 30  # the path's first half minute
WINDOW = (96, 64)  # the cut, in rendered pixels
ZOOM = 4  # the cut's enlargement
CHANGE = 24  # summed RGB difference that counts a pixel as changed
SIDES = ("without curves", "with curves")


def host_and_assets(scene, work):
    """The scene viewer's host, built for `scene`, and the asset folder its build packed."""
    out = subprocess.run(["sh", str(ROOT / "launcher/tools/render/scene_viewer.sh"), str(scene), "--build-only",
                          "-o", str(work / "build")], check=True, capture_output=True, text=True, cwd=ROOT)
    binary = pathlib.Path(out.stdout.strip().splitlines()[-1].removeprefix("built "))
    return binary, binary.parent / "assets"


def frames(host, assets, args, side, curves):
    video = args.work / f"{side}.avi"
    objects = [flag for name in args.object for flag in ("--object", name)]
    with open(args.work / f"{side}.log", "w") as log:
        subprocess.run([str(host), "--scene", scene_id(args.scene), *objects, "--camera", args.camera, "--curves", curves,
                        "--frames", str(FRAMES), "--dt", str(FRAME_MS), "--video", str(video),
                        "-o", str(args.work / f"{side}.bmp")],
                       check=True, stdout=log, stderr=subprocess.STDOUT, env={**os.environ, ASSET_DIR_ENV: str(assets)})
    return [frame.astype(np.int16) for frame in read_video(video)[1]]


def main(argv=None):
    args = scene_parser(__doc__).parse_args(argv)
    args.work.mkdir(parents=True, exist_ok=True)
    host, assets = host_and_assets(args.scene, args.work)
    clips = {side: frames(host, assets, args, f"curves-{state}", state) for side, state in zip(SIDES, ("off", "on"))}
    changed = [(np.abs(a - b).sum(2) > CHANGE) for a, b in zip(*clips.values())]
    k = max(range(len(changed)), key=lambda i: int(changed[i].sum()))
    x, y = busiest(changed[k].astype(int), WINDOW)
    width, height = WINDOW
    cut = {side: Image.fromarray(clip[k][y:y + height, x:x + width].astype(np.uint8))
           .resize((width * ZOOM, height * ZOOM), Image.NEAREST) for side, clip in clips.items()}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    save_png(still_sheet([[(f"{side}, {k * FRAME_MS / 1000:g} s", cut[side]) for side in SIDES]]), args.out)


if __name__ == "__main__":
    main()
