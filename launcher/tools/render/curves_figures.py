#!/usr/bin/env python3
"""What a renderer's thin curves add, seen on a scene's camera path: the same locked bake drawn without its
curve entries and with them. launcher/tools/render/render_doc_images.sh runs it.

    python launcher/tools/render/curves_figures.py --scene SCENE.scene.toml --object NAME [--object NAME ...]
        --curves NAME --camera NAME --out PNG --work DIR

--curves names the renderer whose bake carries the curves; every --object is drawn. It writes --out: the frame
where the curves change the most pixels, cut to the WINDOW holding most of them, without curves above with
curves, enlarged without smoothing.
"""

import argparse
import os
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image
from depth_figures import busiest
from render_compare import still_sheet

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figures import ASSET_DIR_ENV, save_png  # noqa: E402
from r3d import mesh_bundle  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402
from r3d.thin import curve_format  # noqa: E402
from render.check_avi import read_video  # noqa: E402

FRAME_MS = 1000  # one frame a second along the camera path
FRAMES = 30  # the path's first half minute
WINDOW = (96, 64)  # the cut, in rendered pixels
ZOOM = 4  # the cut's enlargement
CHANGE = 24  # summed RGB difference that counts a pixel as changed
SIDES = ("without curves", "with curves")


def locked_bundle(name):
    """The locked bake file of output `name`, fetched into the bake cache."""
    out = subprocess.run([sys.executable, str(ROOT / "launcher/tools/bake/bake.py"), "path", name], check=True,
                         capture_output=True, text=True, cwd=ROOT)
    return pathlib.Path(out.stdout.strip().splitlines()[-1])


def without_curves(bundle, out):
    entries, copies = mesh_bundle.decode(bundle.read_bytes())
    kept = [entry for entry in entries if entry[1] != curve_format.TYPE]
    if len(kept) == len(entries):
        raise SystemExit(f"{bundle} has no curve entry: its import does not turn geometry.curves on")
    out.write_bytes(mesh_bundle.encode(kept, copies))
    return out


def host_and_assets(scene, work):
    """The scene viewer's host, built for `scene`, and the asset folder its build packed."""
    out = subprocess.run(["sh", str(ROOT / "launcher/tools/render/scene_viewer.sh"), str(scene), "--build-only",
                          "-o", str(work / "build")], check=True, capture_output=True, text=True, cwd=ROOT)
    binary = pathlib.Path(out.stdout.strip().splitlines()[-1].removeprefix("built "))
    return binary, binary.parent / "assets"


def frames(host, assets, args, side):
    video = args.work / f"{side}.avi"
    objects = [flag for name in args.object for flag in ("--object", name)]
    with open(args.work / f"{side}.log", "w") as log:
        subprocess.run([str(host), "--scene", scene_id(args.scene), *objects, "--camera", args.camera,
                        "--frames", str(FRAMES), "--dt", str(FRAME_MS), "--video", str(video),
                        "-o", str(args.work / f"{side}.bmp")],
                       check=True, stdout=log, stderr=subprocess.STDOUT, env={**os.environ, ASSET_DIR_ENV: str(assets)})
    return [frame.astype(np.int16) for frame in read_video(video)[1]]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scene", required=True, type=pathlib.Path)
    parser.add_argument("--object", required=True, action="append", help="a renderer to draw; repeat for more")
    parser.add_argument("--curves", required=True, help="the drawn renderer whose bake carries curves")
    parser.add_argument("--camera", required=True)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    args.work.mkdir(parents=True, exist_ok=True)
    host, assets = host_and_assets(args.scene, args.work)
    name = f"{scene_id(args.scene)}.{args.curves}"
    bare = without_curves(locked_bundle(f"{name}{mesh_bundle.SUFFIX}"), args.work / f"bare{mesh_bundle.SUFFIX}")
    subprocess.run([sys.executable, str(ROOT / "launcher/tools/r3d/build_pack.py"), "-o", str(args.work / "bare"),
                    str(args.scene), "--replace", f"{name}={bare}"], check=True, cwd=ROOT,
                   stdout=subprocess.DEVNULL)
    clips = {SIDES[0]: frames(host, args.work / "bare", args, "bare"), SIDES[1]: frames(host, assets, args, "curves")}
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
