"""Draw skin_light_bench's figures, each animated over the sheet's clip: the
mesh lit by every variant beside its error against the reference, the skinned
normal and its length, each lighting term alone, and the tables' cells.

    python tools/r3d/skin_light/skin_light_sheet.py ASSET.glb BENCH_DIR OUT_DIR [--sheet CLIP[:PHASE]]

Run from launcher/. Needs Pillow. BENCH_DIR holds the bench's output:
sheet.bin and terms.bin, the native RGB565 colour of every vertex at every
frame of the clip, one run per label in sheet.txt and terms.txt; normals.bin,
the skinned normal before renormalising; tables.bin, each table's cells.
--sheet must name the frame the data was written with. OUT_DIR receives
skin-light-sheet.gif, skin-light-normals.gif, skin-light-terms.gif and
skin-light-table.png.
"""

import argparse
import math
import pathlib
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from r3d import gltf_preview, gltf_skin  # noqa: E402
from r3d.skin_light import skin_light_data  # noqa: E402
from device.gfx_color import expand  # noqa: E402

COLUMNS = 3
TILE = (200, 160)
VIEW = (30.0, 14.0)
LEGEND = 26
LEGEND_WIDTH = 256
# Error colour ramp: black at 0, through red and yellow, white at ERROR_FULL
# 8-bit steps of the worst channel.
ERROR_FULL = 24
# The same ramp over how much blending shortened a unit normal: white at this
# share of its length lost.
SHORTENING_FULL = 0.10
# The sheet's labels of the two runs the normals figure compares; the reference is the renormalised float run.
DIRECT = "Direct, float"
RENORMALISED = "Reference"
# The table figure marks where each axis lands on the square.
AXES = {"+X": (1, 0, 0), "-X": (-1, 0, 0), "+Y": (0, 1, 0), "-Y": (0, -1, 0), "+Z": (0, 0, 1), "-Z": (0, 0, -1)}
# One table cell's side in the table figure, at the largest table.
CELL = 6
# The figures draw every STEPth frame of the bench's, which keeps the GIFs small.
STEP = 2
PAD = 8
CAPTION = 16


def heat(t):
    t = min(max(t, 0.0), 1.0)
    return (int(255 * min(1.0, 3 * t)), int(255 * min(1.0, max(0.0, 3 * t - 1))), int(255 * max(0.0, 3 * t - 2)))


def unpack(native):
    return expand((native >> 11) & 0x1F, (native >> 5) & 0x3F, native & 0x1F)


def direction_colour(n):
    """A direction's axes as RGB, -1 to 0 and +1 to 255."""
    length = math.sqrt(sum(c * c for c in n)) or 1.0
    return tuple(int(round(255 * (0.5 + 0.5 * c / length))) for c in n)


def label(tile, text):
    ImageDraw.Draw(tile).text((6, 4), text, fill=(20, 20, 20), stroke_width=2, stroke_fill=(255, 255, 255))
    return tile


def legend(image, top, text, end):
    draw = ImageDraw.Draw(image)
    draw.text((6, top + 7), text, fill=(20, 20, 20))
    left = int(draw.textlength(text)) + 16
    for i in range(LEGEND_WIDTH):
        draw.line([(left + i, top + 6), (left + i, top + 19)], fill=heat(i / (LEGEND_WIDTH - 1)))
    draw.text((left + LEGEND_WIDTH + 6, top + 7), end, fill=(20, 20, 20))


class Clip:
    """The sheet's clip: its frames' skinned positions and one fixed camera framing all of them."""

    def __init__(self, asset, spec):
        clip, _ = skin_light_data.sheet_frame(asset, spec)
        count = skin_light_data.frame_count(asset, clip)
        self.asset = asset
        self.positions = [asset.skin(asset.sample(clip, asset.duration(clip) * i / count))[0] for i in range(count)]
        centre, radius = gltf_preview.bounds([p for frame in self.positions for p in frame])
        self.camera = gltf_preview.Camera(centre, radius * 0.62, *VIEW, TILE[0] * gltf_preview.SUPERSAMPLE,
                                          TILE[1] * gltf_preview.SUPERSAMPLE)

    def tile(self, frame, colours, text):
        return self.tile_at(self.positions[frame], colours, text)

    def tile_at(self, positions, colours, text):
        return label(gltf_preview.render(self.asset, self.camera, positions, TILE, colours), text)

    def frames(self):
        return range(0, len(self.positions), STEP)

    def save(self, path, frames):
        frames[0].save(path, save_all=True, append_images=frames[1:],
                       duration=int(round(1000.0 * STEP / skin_light_data.FPS)), loop=0, optimize=True)


def grid(tiles, columns, legend_text=None, legend_end=""):
    rows = (len(tiles) + columns - 1) // columns
    out = Image.new("RGB", (TILE[0] * columns, TILE[1] * rows + (LEGEND if legend_text else 0)), (255, 255, 255))
    for index, tile in enumerate(tiles):
        out.paste(tile, ((index % columns) * TILE[0], (index // columns) * TILE[1]))
    if legend_text:
        legend(out, TILE[1] * rows, legend_text, legend_end)
    return out


def sheet(clip, labels, runs):
    """Every variant beside its error against the reference, the first run."""
    frames = []
    for k in clip.frames():
        tiles = []
        for name, run in zip(labels, runs):
            errors = [heat(max(abs(a - b) for a, b in zip(got, want)) / ERROR_FULL)
                      for got, want in zip(run[k], runs[0][k])]
            tiles += [clip.tile(k, run[k], name), clip.tile(k, errors, "error")]
        frames.append(grid(tiles, 2 * COLUMNS, "error, worst channel, 8-bit steps:  0", f"{ERROR_FULL} or more"))
    return frames


def normals(clip, blended, direct, renormalised):
    """The skinned normal's direction and how much blending shortened it, beside direct, renormalised and how
    much darker direct is, on the error ramp."""
    frames = []
    for k in clip.frames():
        frame = blended[k]
        lost = [heat((1.0 - math.sqrt(sum(c * c for c in n))) / SHORTENING_FULL) for n in frame]
        darker = [heat(max(b - a for a, b in zip(got, want)) / ERROR_FULL)
                  for got, want in zip(direct[k], renormalised[k])]
        tiles = [clip.tile(k, [direction_colour(n) for n in frame], "normal direction"),
                 clip.tile(k, lost, "length lost"),
                 clip.tile(k, direct[k], "direct"),
                 clip.tile(k, renormalised[k], "renormalised"),
                 clip.tile(k, darker, "direct darker by")]
        frames.append(grid(tiles, len(tiles), "length lost 0, darker 0",
                           f"length lost {SHORTENING_FULL:.0%}, darker {ERROR_FULL} 8-bit steps, or more"))
    return frames


def terms(clip, labels, runs):
    return [grid([clip.tile(k, run[k], name) for name, run in zip(labels, runs)], len(runs))
            for k in clip.frames()]


def tables(cells):
    """The largest table's cell directions, then each table's light, drawn the same size."""
    side = max(size for size, _ in cells) * CELL
    largest, table = cells[-1]
    panels = [("cell direction", largest, [direction_colour(d) for d, _ in table])]
    panels += [(f"{size}x{size} light", size, [rgb for _, rgb in table]) for size, table in cells]
    out = Image.new("RGB", (len(panels) * (side + PAD) + PAD, side + 2 * PAD + CAPTION), (255, 255, 255))
    draw = ImageDraw.Draw(out)
    for index, (text, size, colours) in enumerate(panels):
        panel = Image.new("RGB", (size, size))
        panel.putdata(colours)
        x = PAD + index * (side + PAD)
        out.paste(panel.resize((side, side), Image.NEAREST), (x, PAD + CAPTION))
        draw.text((x, PAD), text, fill=(20, 20, 20))
    # Each axis at the centre of the cell whose direction is nearest it, on the first panel.
    for name, axis in AXES.items():
        cell = max(range(len(table)), key=lambda c: sum(a * d for a, d in zip(axis, table[c][0])))
        centre = ((cell % largest + 0.5) * side / largest + PAD, (cell // largest + 0.5) * side / largest + PAD + CAPTION)
        draw.text(centre, name, fill=(0, 0, 0), anchor="mm", stroke_width=2, stroke_fill=(255, 255, 255))
    return out


def read_runs(bench, name, count, frames):
    labels = (bench / f"{name}.txt").read_text(encoding="utf-8").splitlines()
    raw = (bench / f"{name}.bin").read_bytes()
    values = struct.unpack(f"<{len(raw) // 2}H", raw)
    if len(values) != count * frames * len(labels):
        sys.exit(f"{bench}: expected {len(labels)} runs of {frames} frames of {count} colours in {name}.bin")
    colours = [unpack(v) for v in values]
    run = count * frames
    return labels, [[colours[r * run + f * count:r * run + (f + 1) * count] for f in range(frames)]
                    for r in range(len(labels))]


def read_normals(bench, count, frames):
    raw = (bench / "normals.bin").read_bytes()
    values = struct.unpack(f"<{len(raw) // 4}f", raw)
    if len(values) != 3 * count * frames:
        sys.exit(f"{bench}: expected {frames} frames of {count} normals in normals.bin")
    triples = [values[i:i + 3] for i in range(0, len(values), 3)]
    return [triples[f * count:(f + 1) * count] for f in range(frames)]


def read_tables(bench):
    """(size, [(direction, light rgb)] per cell) per table, smallest first."""
    raw = (bench / "tables.bin").read_bytes()
    cells, offset = [], 0
    while offset < len(raw):
        (size,) = struct.unpack_from("<I", raw, offset)
        offset += 4
        table = []
        for _ in range(size * size):
            table.append((struct.unpack_from("<3f", raw, offset), tuple(raw[offset + 12:offset + 15])))
            offset += 16
        cells.append((size, table))
    return cells


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("asset")
    parser.add_argument("bench", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    skin_light_data.sheet_argument(parser)
    args = parser.parse_args()
    asset = gltf_skin.SkinnedAsset(*gltf_skin.load_asset(args.asset))
    clip = Clip(asset, args.sheet)
    count, frames = len(asset.positions), len(clip.positions)
    labels, runs = read_runs(args.bench, "sheet", count, frames)
    clip.save(args.out / "skin-light-sheet.gif", sheet(clip, labels, runs))
    clip.save(args.out / "skin-light-normals.gif",
              normals(clip, read_normals(args.bench, count, frames), runs[labels.index(DIRECT)],
                      runs[labels.index(RENORMALISED)]))
    clip.save(args.out / "skin-light-terms.gif", terms(clip, *read_runs(args.bench, "terms", count, frames)))
    tables(read_tables(args.bench)).save(args.out / "skin-light-table.png", optimize=True)


if __name__ == "__main__":
    main()
