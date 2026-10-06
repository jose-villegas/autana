"""Draw skin_light_bench's sheet frame: the mesh lit by every variant, and
beside each its error against the reference.

    python tools/r3d/skin_light/skin_light_sheet.py ASSET.glb SHEET.bin OUT.png

Run from launcher/. Needs Pillow. SHEET.bin is the bench's native RGB565
colour of every vertex, one run per lit variant in the bench's order; the
pose is skin_light_data.py's sheet frame.
"""

import pathlib
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from r3d import gltf_preview, gltf_skin  # noqa: E402
from r3d.skin_light import skin_light_data  # noqa: E402
from device.gfx_color import expand  # noqa: E402

# skin_light_bench.c's VARIANTS after the skin-only row, as the sheet labels them.
LABELS = (
    "Reference", "Direct, float", "Direct, int8",
    "8x8 nearest", "16x16 nearest", "32x32 nearest",
    "8x8 bilinear", "16x16 bilinear", "32x32 bilinear",
)
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


def sheet(asset, colours):
    clip, time = skin_light_data.sheet_frame(asset)
    positions, _ = asset.skin(asset.sample(clip, time))
    centre, radius = gltf_preview.bounds(positions)
    camera = gltf_preview.Camera(centre, radius * 0.62, *VIEW, TILE[0] * gltf_preview.SUPERSAMPLE,
                                 TILE[1] * gltf_preview.SUPERSAMPLE)
    reference = colours[0]
    rows = (len(LABELS) + COLUMNS - 1) // COLUMNS
    out = Image.new("RGB", (TILE[0] * COLUMNS * 2, TILE[1] * rows + LEGEND), (255, 255, 255))
    for index, (name, lit) in enumerate(zip(LABELS, colours)):
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
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    asset = gltf_skin.SkinnedAsset(*gltf_skin.load_glb(sys.argv[1]))
    raw = pathlib.Path(sys.argv[2]).read_bytes()
    count = len(asset.positions)
    values = struct.unpack(f"<{len(raw) // 2}H", raw)
    if len(values) != count * len(LABELS):
        sys.exit(f"{sys.argv[2]}: expected {len(LABELS)} runs of {count} colours")
    colours = [[unpack(v) for v in values[i * count:(i + 1) * count]] for i in range(len(LABELS))]
    sheet(asset, colours).save(sys.argv[3], optimize=True)


if __name__ == "__main__":
    main()
