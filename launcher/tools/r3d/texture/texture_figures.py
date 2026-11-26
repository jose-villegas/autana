"""A scene's textured look against its source: LTEX.md's sheet of one path frame, drawn plain and with its textures.

    python launcher/tools/r3d/texture/texture_figures.py --scene SCENE.scene.toml --object NAME --reference BAKE
        --frame N --dt MS --title TEXT --image NAME.png [--divides 1,0,16,8 --divide-image NAME.png]
        [--far-frame N --far-image NAME.png] --out DIR

Renders frame N of the object's path on host (scene_viewer.sh) plain and with the object's textures (--textured),
takes the same pose from the reference bake BAKE (a .reference.tar.xz, bake.py path), scaled to the render's size,
and lays the three out with render_compare.py: the source, the plain draw, the textured draw and each one's error
against the source. Frame N is the pose at (N + 1) * MS, as the viewer steps the path before it draws; it must be
one of the fit's training poses, the only ones the bake holds.

--divides draws the textured frame at each texture.divide_px value, the first the truth (1, with affine_bend 0, so
every triangle divides). --divide-image writes the squares of the frame's lowest third where the second value differs
most from it: a row per square, a column per value, each headed with its CIE76 dE against the truth over that third.
Its draws read every level's texels (texture.far_level 0), so they differ by the divide alone.

--far-image writes path frame --far-frame textured with every level's texels and with the default texture.far_level,
whose far levels draw untextured, one colour a triangle, beside the source, as the first sheet, with its crops.
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
sys.path.insert(0, str(ROOT / "launcher/tools"))
import render_compare  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.reference_set import TRAINING, split_poses  # noqa: E402
# The reference archive's portrait training references, one PNG per training pose.
REFERENCE_FRAME = TRAINING[0][1] + "/{index:04d}.png"
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
# texture.affine_level 0: no level steps affine for its coarseness.
LEVEL_TUNABLE = "render.texture_affine_level"
TRUTH = 1
# The tunable of the first level drawn untextured, one colour a triangle; 0 textures every level.
FAR_TUNABLE = "render.texture_far_level"
EVERY_LEVEL = 0
# Squares the divide sheet zooms on.
DIVIDE_CROPS = 4
# The divide sheet's square side in pixels.
DIVIDE_CELL = 240


def run(command):
    done = subprocess.run([str(part) for part in command], cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        raise SystemExit(f"{' '.join(str(part) for part in command)} failed:\n{done.stderr[-2000:]}")
    return done.stdout


def draw(args, textured, work, name, frame=None, sets=()):
    """Path frame `frame` (args.frame when None) of the object, textured when `textured`, with the tunables `sets`
    ((name, value) pairs), as the PNG work/name.png."""
    bmp = work / f"{name}.bmp"
    tune = [part for tunable, value in sets for part in ("--set", f"{tunable}={value}")]
    frames = (args.frame if frame is None else frame) + 1
    run(["sh", VIEWER, args.scene, "--object", args.object, "--quarter", PORTRAIT, "--frames", frames,
         "--dt", args.dt, *(["--textured", args.object] if textured else []), *tune, "-o", bmp])
    png = work / f"{name}.png"
    Image.open(bmp).convert("RGB").save(png)
    return png


def divide_sets(divide):
    """The tunables of a divide sheet's draw: every `divide` pixels, every level's texels, and for the truth no bend
    and no level allowed to step affine."""
    truth = ((AFFINE_TUNABLE, 0), (LEVEL_TUNABLE, 0)) if divide == TRUTH else ()
    return ((DIVIDE_TUNABLE, divide), (FAR_TUNABLE, EVERY_LEVEL), *truth)


def reference_member(fit, frame, dt_ms):
    """The reference archive's picture of the pose the viewer draws as path frame `frame`, `dt_ms` apart: the
    viewer steps the path by dt_ms before each draw, so frame N is at (N + 1) * dt_ms."""
    time_ms = (frame + 1) * dt_ms
    training, _ = split_poses(fit, [index * fit.train_every_ms for index in range(time_ms // fit.train_every_ms + 1)])
    if time_ms not in training:
        raise SystemExit(f"frame {frame} is the pose at {time_ms} ms, not one of the fit's training poses "
                         f"(every {fit.train_every_ms} ms, those at multiples of {fit.held_out_every_ms} ms held out)")
    return REFERENCE_FRAME.format(index=training.index(time_ms))


def object_fit(scene, name):
    """The fit settings of the scene's object `name`."""
    for item in load_scene(scene).objects:
        if item.name == name:
            return item.component.fit
    raise SystemExit(f"{scene} has no object {name}")


def reference(args, name, size, work):
    """The reference bake's picture `name`, scaled to `size`."""
    archive = run([sys.executable, BAKE, "path", args.reference]).strip().splitlines()[-1]
    with tarfile.open(archive) as tar:
        member = tar.extractfile(name)
        if member is None:
            raise SystemExit(f"{args.reference} has no {name}")
        image = Image.open(member).convert("RGB")
        image.load()
    png = work / "reference.png"
    image.resize(size, Image.NEAREST).save(png)
    return png


def divide_label(divide):
    return "affine" if divide == 0 else "true" if divide == TRUTH else f"N{divide}"


def square_box(cluster, size):
    """The square around `cluster`, its longer side, centred on it and moved inside a `size` picture, so every
    square of the sheet is one cell whatever the cluster's shape."""
    side = min(max(cluster.x1 - cluster.x0, cluster.y1 - cluster.y0), *size)
    x0 = min(max((cluster.x0 + cluster.x1 - side) // 2, 0), size[0] - side)
    y0 = min(max((cluster.y0 + cluster.y1 - side) // 2, 0), size[1] - side)
    return (x0, y0, x0 + side, y0 + side)


def divide_sheet(args, work):
    """The squares of the near floor where the second --divides value differs most from the first: a row per
    square, a column per value, each column headed with its dE against the truth over the near floor."""
    divides = [int(value) for value in args.divides.split(",")]
    pictures = [Image.open(draw(args, True, work, f"divide{divide}", sets=divide_sets(divide))).convert("RGB")
                for divide in divides]
    truth = pictures[0]
    top = round(truth.height * (1 - NEAR_SHARE))
    labels = []
    for divide, picture in zip(divides, pictures):
        error = render_compare.delta_e76(picture, truth)[top:]
        labels.append(f"{divide_label(divide)}: dE {error.mean():.2f}, p95 {np.percentile(error, 95):.1f}")
        print(labels[-1])
    lower = truth.copy()
    lower.paste(pictures[1].crop((0, top, truth.width, truth.height)), (0, top))
    clusters = render_compare.find_clusters(truth, lower, None, DIVIDE_CROPS,
                                           crop=lambda cluster: square_box(cluster, truth.size))
    rows = []
    for cluster in clusters:
        box = square_box(cluster, truth.size)
        crops = [picture.crop(box).resize((DIVIDE_CELL, DIVIDE_CELL), Image.NEAREST) for picture in pictures]
        rows.append(list(zip(labels, crops)))
    sheet = render_compare.still_sheet(rows)
    sheet.save(args.out / args.divide_image, optimize=True)


def far_sheet(args, fit, work):
    """Path frame args.far_frame textured with every level's texels and with the default far level, beside the
    source."""
    name = reference_member(fit, args.far_frame, args.dt)
    every = draw(args, True, work, "far_every", args.far_frame, ((FAR_TUNABLE, EVERY_LEVEL),))
    far = draw(args, True, work, "far_default", args.far_frame)
    source = reference(args, name, Image.open(every).size, work)
    run([sys.executable, COMPARE, "--out", args.out / args.far_image, "--reference-stills", args.far_title, source,
         every, far, "--label-a", "every level's texels", "--label-b", "far levels: one colour", "--crops", CROPS])


def main(argv=None):
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
    parser.add_argument("--far-frame", type=int, help="the path frame of the far-level sheet")
    parser.add_argument("--far-image", help="the far-level sheet's file name")
    parser.add_argument("--far-title", help="the far-level sheet's title")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    if bool(args.divides) != bool(args.divide_image):
        parser.error("--divides and --divide-image go together")
    if len({args.far_frame is None, args.far_image is None, args.far_title is None}) != 1:
        parser.error("--far-frame, --far-image and --far-title go together")
    fit = object_fit(args.scene, args.object)
    name = reference_member(fit, args.frame, args.dt)
    if args.far_frame is not None:
        reference_member(fit, args.far_frame, args.dt)
    with tempfile.TemporaryDirectory() as folder:
        work = pathlib.Path(folder)
        plain, textured = (draw(args, value, work, f"draw{value}") for value, _ in DRAWS)
        source = reference(args, name, Image.open(plain).size, work)
        args.out.mkdir(parents=True, exist_ok=True)
        run([sys.executable, COMPARE, "--out", args.out / args.image, "--reference-stills", args.title, source,
             plain, textured, "--label-a", DRAWS[0][1], "--label-b", DRAWS[1][1], "--crops", CROPS])
        if args.divides:
            divide_sheet(args, work)
        if args.far_image:
            far_sheet(args, fit, work)


if __name__ == "__main__":
    main()
