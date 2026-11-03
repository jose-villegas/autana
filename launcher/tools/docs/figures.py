"""What the doc-figure producers share: a C probe built against the engine's libraries, a scene viewer run,
and the one way to write a PNG or a GIF.

    python launcher/tools/docs/figures.py gif AVI GIF --fps 15 [--rate R] [--first N] [--end N] [--seconds S] [--quarter Q]
    python launcher/tools/docs/figures.py png PICTURE PNG [--quarter Q]
"""
import argparse
import fractions
import functools
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.packages import include_roots  # noqa: E402
from docs.probe_standard import PROBE_STANDARD  # noqa: E402
from docs.producer import DEPENDS_FOLDER  # noqa: E402

# Every probe prints its numbers through the library's own text formatting.
PROBE_SUPPORT = (ROOT / "launcher/packages/math/src/text/math_text.c",)


def build_probe(cc, sources, directory):
    """Build a probe against the library into directory; returns the program."""
    binary = pathlib.Path(directory) / "probe"
    flags = [f"-std={PROBE_STANDARD}", *(flag for root in include_roots() for flag in ("-I", str(root)))]
    sources = [str(source) for source in (*sources, *PROBE_SUPPORT)]
    if os.environ.get(DEPENDS_FOLDER):
        for source in sources:
            listing = tempfile.NamedTemporaryFile(suffix=".d", dir=os.environ[DEPENDS_FOLDER], delete=False).name
            subprocess.run([cc, *flags, "-MM", "-MF", listing, source], check=True)
    subprocess.run([cc, *flags, "-O1", *sources, "-lm", "-o", str(binary)], check=True)
    return binary


def probe_json(binary, *arguments):
    """Run a built probe; its JSON is every number a figure shows."""
    out = subprocess.run([str(binary), *map(str, arguments)], check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def run_probe(cc, sources, *arguments):
    """Build and run a probe once."""
    with tempfile.TemporaryDirectory() as work:
        return probe_json(build_probe(cc, sources, work), *arguments)


def probe_into_folder(description, sources, *arguments):
    """A producer's --cc CC --out DIR: makes DIR and runs the probe; returns (its JSON, DIR)."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    return run_probe(args.cc, sources, *arguments), args.out


SCENE_VIEWER = ROOT / "launcher/tools/render/scene_viewer.sh"


def render_scene(scene, *flags, log):
    """Run the scene viewer on `scene` with `flags`; its stdout and stderr go to `log`, whose text is returned."""
    with open(log, "w") as handle:
        subprocess.run(["sh", str(SCENE_VIEWER), str(scene), *map(str, flags)], check=True, stdout=handle,
                       stderr=subprocess.STDOUT)
    return pathlib.Path(log).read_text(encoding="utf-8", errors="replace")


def save_png(picture, out):
    """Write `picture`, a Pillow image or a file Pillow opens (a BMP, say), as a PNG with no metadata, so an
    unchanged picture always gives the same bytes."""
    image = picture.copy() if isinstance(picture, Image.Image) else Image.open(picture)
    image.load()
    image.info = {}
    out = pathlib.Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    image.save(out, format="PNG", optimize=True)


# Every doc GIF is drawn from this one palette, so a colour has the same index in all of them. The theme
# colours come first; the rest were fitted to the doc GIFs by gif_palette.py, which prints this table.
GIF_COLOURS = 256
GIF_PALETTE = (
    "#ffffff", "#000000", "#0b0b0b", "#52514e", "#c9c7c2", "#2a78d6", "#eb6834", "#1baf7a",
    "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948", "#0d1117", "#e6edf3", "#9da7b3",
    "#30363d", "#c4d8f1", "#b3bca8", "#946984", "#b9a387", "#fdfdfc", "#292418", "#f3fde3",
    "#1c0401", "#7ba284", "#f9fbfc", "#ff00e7", "#c8c9ca", "#423c31", "#ac714b", "#443225",
    "#998c70", "#adaead", "#292c39", "#181a0c", "#031f4b", "#030303", "#101008", "#8cd4ff",
    "#c3c3c3", "#bae6d6", "#6a6a6a", "#101010", "#fef9f7", "#31799c", "#462b19", "#f9dde8",
    "#7d7b7b", "#312c21", "#453927", "#313431", "#f8e1b8", "#1d1f0d", "#312821", "#a5ae8c",
    "#464c57", "#112030", "#000800", "#110c08", "#d89360", "#636584", "#101829", "#0d1200",
    "#80867a", "#061e00", "#cc895a", "#1e5917", "#0b2302", "#605f5d", "#fefdfd", "#9cbace",
    "#21170c", "#8a8a89", "#7b6d5a", "#ececeb", "#83887f", "#fdfdfd", "#181c21", "#bcc4b0",
    "#181810", "#5b5548", "#404040", "#c27748", "#383426", "#fdfcfc", "#282828", "#292421",
    "#181818", "#9a6847", "#41422a", "#00dbff", "#89b1e2", "#310502", "#62412c", "#fb0025",
    "#dad9d8", "#212018", "#e5e3f2", "#714628", "#aab39e", "#e9e8e6", "#b0cbda", "#861310",
    "#1f490e", "#b977b1", "#f4c2d0", "#5496e0", "#390a03", "#080808", "#deff8c", "#b6b5b5",
    "#7b6984", "#39cb52", "#12356b", "#292e2d", "#5a5b5a", "#474139", "#244c6d", "#aed8b9",
    "#4c4635", "#cedce8", "#393831", "#15417a", "#73a2bd", "#061800", "#28290e", "#c2e5fc",
    "#171412", "#322217", "#222121", "#617070", "#f4d832", "#fe7b00", "#423831", "#390400",
    "#5403fb", "#1d170f", "#232110", "#290400", "#211c10", "#4b4b44", "#9cc3e7", "#01ffef",
    "#2c1704", "#322f29", "#a72508", "#d8bffd", "#905b3a", "#795034", "#5a86a5", "#847584",
    "#6f7885", "#484848", "#c6c7c6", "#393431", "#f59f9e", "#585041", "#080800", "#140000",
    "#393429", "#253b5f", "#828284", "#f4f4f4", "#ced6de", "#f5c48e", "#4a3421", "#efff00",
    "#000400", "#7b8684", "#636163", "#260b02", "#636664", "#cab190", "#70f6a0", "#181c18",
    "#18556b", "#112c07", "#4c5667", "#e5e5e5", "#345c43", "#2c1007", "#5d4d2b", "#0021ff",
    "#847456", "#ff008c", "#e78cff", "#f8f8f8", "#6b6355", "#8b00ff", "#394552", "#10140d",
    "#a79a82", "#ff5101", "#847d84", "#c300ff", "#e8e9e9", "#040000", "#0e151c", "#18140c",
    "#e7a66b", "#23483d", "#112c57", "#acb6a1", "#dfeaf8", "#d4d3cf", "#180c07", "#635b45",
    "#f0f0f0", "#737173", "#7b9284", "#959593", "#b04fd4", "#322818", "#292018", "#6f654d",
    "#edfebf", "#fce9e1", "#535351", "#4a8ead", "#f8ecc7", "#bcfebc", "#dbfced", "#a0a0a0",
    "#392a1d", "#b97d54", "#4b9531", "#221b17", "#080c08", "#480a03", "#737573", "#1a2606",
    "#cdcdcd", "#5e6068", "#fcfcfc", "#91816a", "#bdbebe", "#0061ff", "#3efd53", "#6e160e",
    "#c1fef2", "#5a0f04", "#393827", "#393021", "#3a393c", "#154a91", "#28baea", "#fe0900",
    "#4c4331", "#321c08", "#f1f1f1", "#3c537a", "#5a92ad", "#171407", "#e0e0df", "#5a3824",
)
NEAREST_CHUNK = 8192  # colours compared against a table at once


def packed_colours(pixels):
    """One integer per RGB pixel of a (..., 3) array."""
    pixels = np.asarray(pixels, np.int64)
    return pixels[..., 0] << 16 | pixels[..., 1] << 8 | pixels[..., 2]


def unpacked_colours(keys):
    """The (n, 3) uint8 colours of packed_colours keys."""
    return np.stack([keys >> 16, keys >> 8 & 255, keys & 255], -1).astype(np.uint8)


def oklab(colours):
    """sRGB colours, (n, 3) uint8, in OKLab, where distance follows how different two colours look."""
    linear = np.asarray(colours, np.float64) / 255
    linear = np.where(linear <= 0.04045, linear / 12.92, ((linear + 0.055) / 1.055) ** 2.4)
    cone = linear @ np.array([[0.4122214708, 0.2119034982, 0.0883024619],
                              [0.5363325363, 0.6806995451, 0.2817188376],
                              [0.0514459929, 0.1073970566, 0.6299787005]])
    return np.cbrt(cone) @ np.array([[0.2104542553, 1.9779984951, 0.0259040371],
                                      [0.7936177850, -2.4285922050, 0.7827717662],
                                      [-0.0040720468, 0.4505937099, -0.8086757660]])


def nearest(points, table):
    """For each point, the index of the nearest row of `table`."""
    return np.concatenate([((part[:, None, :] - table[None]) ** 2).sum(2).argmin(1)
                           for part in np.array_split(points, max(1, -(-len(points) // NEAREST_CHUNK)))])


@functools.cache
def gif_palette():
    """GIF_PALETTE as (n, 3) sRGB and as OKLab."""
    colours = np.array([[int(name[at:at + 2], 16) for at in (1, 3, 5)] for name in GIF_PALETTE], np.uint8)
    return colours, oklab(colours)


def save_gif(frames, out, frame_ms):
    """Write a looping GIF of `frames` (Pillow images or (h, w, 3) arrays of one size), each shown `frame_ms`.
    Every pixel takes the GIF_PALETTE colour nearest it in OKLab, without dithering."""
    pixels = np.stack([np.asarray(frame.convert("RGB") if isinstance(frame, Image.Image) else frame)
                       for frame in frames])
    colours, lab = gif_palette()
    keys = packed_colours(pixels)
    unique, inverse = np.unique(keys, return_inverse=True)
    planes = nearest(oklab(unpacked_colours(unique)), lab)[inverse].reshape(keys.shape).astype(np.uint8)
    images = []
    for plane in planes:
        image = Image.fromarray(plane, "P")
        image.putpalette(colours.tobytes())
        images.append(image)
    out = pathlib.Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    images[0].save(out, save_all=True, append_images=images[1:], loop=0, duration=round(frame_ms), optimize=False)


def clip(frames, rate, fps, *, first=0, end=None, seconds=None):
    """The frames a GIF at `fps` shows of `frames` taken at `rate`: frames `first` up to `end`, at most
    `seconds` of them, each GIF frame the source frame nearest its time."""
    frames = list(frames)[first:end]
    if seconds is not None:
        frames = frames[:max(1, int(seconds * rate + 0.5))]
    count = max(1, int(len(frames) * fps / rate + 0.5))
    return [frames[min(len(frames) - 1, int(index * rate / fps + 0.5))] for index in range(count)]


def gif_from_avi(avi, out, fps, *, rate=None, first=0, end=None, seconds=None, quarter=0):
    """A host render's AVI as a doc GIF at `fps`, turned `quarter` quarters counterclockwise. `rate` replaces the
    AVI's own frame rate, so rate == fps shows every frame once."""
    from render.check_avi import read_video
    source_rate, frames = read_video(avi)
    shown = clip(frames, rate or source_rate, fps, first=first, end=end, seconds=seconds)
    save_gif([np.rot90(frame, quarter) for frame in shown], out, 1000 / fps)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Write a doc GIF from a host render's AVI, or a doc PNG.")
    commands = parser.add_subparsers(dest="command", required=True)
    gif = commands.add_parser("gif", help="AVI to GIF on the global palette")
    gif.add_argument("avi", type=pathlib.Path)
    gif.add_argument("gif", type=pathlib.Path)
    gif.add_argument("--fps", type=fractions.Fraction, required=True, help="the GIF's frame rate, e.g. 15 or 25/2")
    gif.add_argument("--rate", type=fractions.Fraction, help="the AVI's frame rate instead of its own")
    gif.add_argument("--first", type=int, default=0, help="the first AVI frame shown")
    gif.add_argument("--end", type=int, help="the AVI frame after the last one shown")
    gif.add_argument("--seconds", type=float, help="show at most this much of the AVI")
    gif.add_argument("--quarter", type=int, default=0, help="quarter turns counterclockwise")
    png = commands.add_parser("png", help="any picture Pillow opens to a PNG with no metadata")
    png.add_argument("picture", type=pathlib.Path)
    png.add_argument("png", type=pathlib.Path)
    png.add_argument("--quarter", type=int, default=0, help="quarter turns counterclockwise")
    args = parser.parse_args(argv)
    if args.command == "png":
        save_png(Image.open(args.picture).rotate(90 * args.quarter, expand=True), args.png)
        return
    gif_from_avi(args.avi, args.gif, args.fps, rate=args.rate, first=args.first, end=args.end, seconds=args.seconds,
                 quarter=args.quarter)


if __name__ == "__main__":
    main()
