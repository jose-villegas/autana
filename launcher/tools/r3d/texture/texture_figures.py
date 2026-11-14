"""A scene's textured look against its source: LTEX.md's sheet of one path frame, drawn plain and with its textures.

    python launcher/tools/r3d/texture/texture_figures.py --scene SCENE.scene.toml --object NAME --reference BAKE
        --frame N --dt MS --title TEXT --image NAME.png [--divides 1,0,16,8 --divide-image NAME.png] --out DIR

Renders frame N of the object's path on host (scene_viewer.sh) plain and with the object's textures (--textured),
takes the same frame from the reference bake BAKE (a .reference.tar.xz, bake.py path), scaled to the render's size,
and lays the three out with render_compare.py: the source, the plain draw, the textured draw and each one's error
against the source.

--divides draws the textured frame at each texture.divide_px value, the first the truth (1, with affine_bend 0, so
every triangle divides). --divide-image writes the squares of the frame's lowest third where the second value differs
most from it: a row per square, a column per value, each headed with its CIE76 dE against the truth over that third.
"""
import argparse
import pathlib
import subprocess
import sys
import tarfile
import tempfile

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[4]
VIEWER = ROOT / "launcher/tools/render/scene_viewer.sh"
COMPARE = ROOT / "launcher/tools/render/render_compare.py"
BAKE = ROOT / "launcher/tools/bake/bake.py"
sys.path.insert(0, str(COMPARE.parent))
import render_compare  # noqa: E402
# The reference archive's portrait frames, one PNG per path frame.
REFERENCE_FRAME = "reference/{frame:04d}.png"
# The draws compared: whether textured, and its label.
DRAWS = ((False, "vertex colours"), (True, "textured"))
# Squares render_compare.py zooms on.
CROPS = 3
# The quarter turn the frame is drawn at: portrait, as the reference.
PORTRAIT = 0
# The share of the frame's height, from the bottom, the divide sheet measures: the floor nearest the camera.
NEAR_SHARE = 1 / 3
# The tunables the divide sheet sets: the divide spacing, and the bend under which a triangle steps affine, 0 for
# the truth so every triangle divides.
DIVIDE_TUNABLE = "render.texture_divide_px"
AFFINE_TUNABLE = "render.texture_bend_steps"
TRUTH = 1
# Squares the divide sheet zooms on.
DIVIDE_CROPS = 4
# The divide sheet's square side in pixels.
DIVIDE_CELL = 240


def run(command):
    done = subprocess.run([str(part) for part in command], cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        raise SystemExit(f"{' '.join(str(part) for part in command)} failed:\n{done.stderr[-2000:]}")
    return done.stdout


def draw(args, value, work, divide=None):
    """Frame args.frame of the object's path, textured when `value`, dividing every `divide` pixels when given, as
    a PNG."""
    name = f"draw{value}" if divide is None else f"divide{divide}"
    bmp = work / f"{name}.bmp"
    tune = [] if divide is None else ["--set", f"{DIVIDE_TUNABLE}={divide}"]
    if divide == TRUTH:
        tune += ["--set", f"{AFFINE_TUNABLE}=0"]
    run(["sh", VIEWER, args.scene, "--object", args.object, "--quarter", PORTRAIT, "--frames", args.frame + 1,
         "--dt", args.dt, *(["--textured", args.object] if value else []), *tune, "-o", bmp])
    png = work / f"{name}.png"
    Image.open(bmp).convert("RGB").save(png)
    return png


def reference(args, size, work):
    """The reference bake's frame args.frame, scaled to `size`."""
    archive = run([sys.executable, BAKE, "path", args.reference]).strip().splitlines()[-1]
    with tarfile.open(archive) as tar:
        member = tar.extractfile(REFERENCE_FRAME.format(frame=args.frame))
        if member is None:
            raise SystemExit(f"{args.reference} has no {REFERENCE_FRAME.format(frame=args.frame)}")
        image = Image.open(member).convert("RGB")
        image.load()
    png = work / "reference.png"
    image.resize(size, Image.NEAREST).save(png)
    return png


def divide_label(divide):
    return "affine" if divide == 0 else "true" if divide == TRUTH else f"N{divide}"


def divide_sheet(args, work):
    """The squares of the near floor where the second --divides value differs most from the first: a row per
    square, a column per value, each column headed with its dE against the truth over the near floor."""
    divides = [int(value) for value in args.divides.split(",")]
    pictures = [Image.open(draw(args, True, work, divide)).convert("RGB") for divide in divides]
    truth = pictures[0]
    top = round(truth.height * (1 - NEAR_SHARE))
    labels = []
    for divide, picture in zip(divides, pictures):
        error = render_compare.delta_e76(picture, truth)[top:]
        labels.append(f"{divide_label(divide)}: dE {error.mean():.2f}, p95 {np.percentile(error, 95):.1f}")
        print(labels[-1])
    lower = truth.copy()
    lower.paste(pictures[1].crop((0, top, truth.width, truth.height)), (0, top))
    clusters = render_compare.find_clusters(truth, lower, None, DIVIDE_CROPS)
    rows = []
    for cluster in clusters:
        box = (cluster.x0, cluster.y0, cluster.x1, cluster.y1)
        scale = DIVIDE_CELL / max(cluster.x1 - cluster.x0, cluster.y1 - cluster.y0)
        size = (round((cluster.x1 - cluster.x0) * scale), round((cluster.y1 - cluster.y0) * scale))
        crops = [picture.crop(box).resize(size, Image.NEAREST) for picture in pictures]
        rows.append(list(zip(labels, crops)))
    sheet = render_compare.still_sheet(rows)
    sheet.save(args.out / args.divide_image, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--scene", required=True)
    parser.add_argument("--object", required=True)
    parser.add_argument("--reference", required=True, help="the reference bake's output name")
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--dt", type=int, required=True, help="milliseconds a path frame")
    parser.add_argument("--title", required=True)
    parser.add_argument("--image", required=True, help="the sheet's file name")
    parser.add_argument("--divides", help="texture.divide_px values, comma-separated, the truth first")
    parser.add_argument("--divide-image", help="the divide sheet's file name")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    if bool(args.divides) != bool(args.divide_image):
        parser.error("--divides and --divide-image go together")
    with tempfile.TemporaryDirectory() as folder:
        work = pathlib.Path(folder)
        plain, textured = (draw(args, value, work) for value, _ in DRAWS)
        source = reference(args, Image.open(plain).size, work)
        args.out.mkdir(parents=True, exist_ok=True)
        run([sys.executable, COMPARE, "--out", args.out / args.image, "--reference-stills", args.title, source,
             plain, textured, "--label-a", DRAWS[0][1], "--label-b", DRAWS[1][1], "--crops", CROPS])
        if args.divides:
            divide_sheet(args, work)


if __name__ == "__main__":
    main()
