"""A scene's textured look against its source: LTEX.md's sheet of one path frame, drawn plain and with its textures.

    python launcher/tools/r3d/texture/texture_figures.py --scene SCENE.scene.toml --object NAME --reference BAKE
        --frame N --dt MS --title TEXT --image NAME.png --out DIR

Renders frame N of the object's path on host (scene_viewer.sh) with r3d_texture.draw at 0 and at 1, takes the same
frame from the reference bake BAKE (a .reference.tar.xz, bake.py path), scaled to the render's size, and lays the
three out with render_compare.py: the source, the plain draw, the textured draw and each one's error against the
source.
"""
import argparse
import pathlib
import subprocess
import sys
import tarfile
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[4]
VIEWER = ROOT / "launcher/tools/render/scene_viewer.sh"
COMPARE = ROOT / "launcher/tools/render/render_compare.py"
BAKE = ROOT / "launcher/tools/bake/bake.py"
# The reference archive's portrait frames, one PNG per path frame.
REFERENCE_FRAME = "reference/{frame:04d}.png"
# The draws compared: the knob's value and its label.
DRAWS = ((0, "vertex colours"), (1, "textured"))
# Squares render_compare.py zooms on.
CROPS = 3
# The quarter turn the frame is drawn at: portrait, as the reference.
PORTRAIT = 0


def run(command):
    done = subprocess.run([str(part) for part in command], cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        raise SystemExit(f"{' '.join(str(part) for part in command)} failed:\n{done.stderr[-2000:]}")
    return done.stdout


def draw(args, value, work):
    """Frame args.frame of the object's path, r3d_texture.draw at `value`, as a PNG."""
    bmp = work / f"draw{value}.bmp"
    run(["sh", VIEWER, args.scene, "--object", args.object, "--quarter", PORTRAIT, "--frames", args.frame + 1,
         "--dt", args.dt, "--set", f"r3d_texture.draw={value}", "-o", bmp])
    png = work / f"draw{value}.png"
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--scene", required=True)
    parser.add_argument("--object", required=True)
    parser.add_argument("--reference", required=True, help="the reference bake's output name")
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--dt", type=int, required=True, help="milliseconds a path frame")
    parser.add_argument("--title", required=True)
    parser.add_argument("--image", required=True, help="the sheet's file name")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        work = pathlib.Path(folder)
        plain, textured = (draw(args, value, work) for value, _ in DRAWS)
        source = reference(args, Image.open(plain).size, work)
        args.out.mkdir(parents=True, exist_ok=True)
        run([sys.executable, COMPARE, "--out", args.out / args.image, "--reference-stills", args.title, source,
             plain, textured, "--label-a", DRAWS[0][1], "--label-b", DRAWS[1][1], "--crops", CROPS])


if __name__ == "__main__":
    main()
