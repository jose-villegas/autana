"""Draw skin_light_bench's sheet frame: the mesh lit by every variant, and
beside each its error against the reference.

    python tools/r3d/skin_light/skin_light_sheet.py ASSET.glb BENCH_DIR OUT.png [--sheet CLIP[:PHASE]]

Run from launcher/. Needs Pillow. BENCH_DIR holds the bench's sheet.bin, the
native RGB565 colour of every vertex, one run per lit variant with the
reference first, and sheet.txt, their labels; --sheet must name the frame the data was written
with.
"""

import argparse
import pathlib
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from r3d import gltf_preview, gltf_skin  # noqa: E402
from r3d.skin_light import skin_light_data  # noqa: E402
from device.gfx_color import expand  # noqa: E402

COLUMNS = 3
TILE = (300, 240)
VIEW = (30.0, 14.0)
LEGEND = 26
# Error colour ramp: black at 0, through red and yellow, white at ERROR_FULL
# 8-bit steps of the worst channel.
ERROR_FULL = 24


def heat(error):
    t = min(error / ERROR_FULL, 1.0)
    return (int(255 * min(1.0, 3 * t)), int(255 * min(1.0, max(0.0, 3 * t - 1))), int(255 * max(0.0, 3 * t - 2)))


def unpack(native):
    return expand((native >> 11) & 0x1F, (native >> 5) & 0x3F, native & 0x1F)


def label(tile, text):
    ImageDraw.Draw(tile).text((6, 4), text, fill=(20, 20, 20), stroke_width=2, stroke_fill=(255, 255, 255))
    return tile


def sheet(asset, labels, colours, spec):
    clip, time = skin_light_data.sheet_frame(asset, spec)
    positions, _ = asset.skin(asset.sample(clip, time))
    centre, radius = gltf_preview.bounds(positions)
    camera = gltf_preview.Camera(centre, radius * 0.62, *VIEW, TILE[0] * gltf_preview.SUPERSAMPLE,
                                 TILE[1] * gltf_preview.SUPERSAMPLE)
    reference = colours[0]
    rows = (len(labels) + COLUMNS - 1) // COLUMNS
    out = Image.new("RGB", (TILE[0] * COLUMNS * 2, TILE[1] * rows + LEGEND), (255, 255, 255))
    for index, (name, lit) in enumerate(zip(labels, colours)):
        x, y = (index % COLUMNS) * 2 * TILE[0], (index // COLUMNS) * TILE[1]
        out.paste(label(gltf_preview.render(asset, camera, positions, TILE, lit), name), (x, y))
        errors = [heat(max(abs(a - b) for a, b in zip(got, want))) for got, want in zip(lit, reference)]
        out.paste(label(gltf_preview.render(asset, camera, positions, TILE, errors), "error"), (x + TILE[0], y))
    legend(out, TILE[1] * rows)
    return out


def legend(image, top):
    draw = ImageDraw.Draw(image)
    draw.text((6, top + 7), "error, worst channel, 8-bit steps:  0", fill=(20, 20, 20))
    left, width = 236, 256
    for i in range(width):
        draw.line([(left + i, top + 6), (left + i, top + 19)], fill=heat(ERROR_FULL * i / (width - 1)))
    draw.text((left + width + 6, top + 7), f"{ERROR_FULL} or more", fill=(20, 20, 20))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("asset")
    parser.add_argument("bench", type=pathlib.Path)
    parser.add_argument("out")
    skin_light_data.sheet_argument(parser)
    args = parser.parse_args()
    asset = gltf_skin.SkinnedAsset(*gltf_skin.load_asset(args.asset))
    labels = (args.bench / "sheet.txt").read_text(encoding="utf-8").splitlines()
    raw = (args.bench / "sheet.bin").read_bytes()
    count = len(asset.positions)
    values = struct.unpack(f"<{len(raw) // 2}H", raw)
    if len(values) != count * len(labels):
        sys.exit(f"{args.bench}: expected {len(labels)} runs of {count} colours")
    colours = [[unpack(v) for v in values[i * count:(i + 1) * count]] for i in range(len(labels))]
    sheet(asset, labels, colours, args.sheet).save(args.out, optimize=True)


if __name__ == "__main__":
    main()
