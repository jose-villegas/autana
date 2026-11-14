"""Draws the home screen's tile skins: rounded, half see-through, corners antialiased.

Usage: python home_tile_art.py OUT_DIR    writes OUT_DIR/home_tile.png and OUT_DIR/home_tile_pressed.png
"""
import pathlib
import sys
import tomllib

from PIL import Image, ImageDraw

SIDE = 48                       # sprite side, px
# Corner radius, px: the nine-slice border home_tile.image.toml gives the importer.
RADIUS = tomllib.loads((pathlib.Path(__file__).with_name("home_tile.image.toml")).read_text())["border"][0]
SUPERSAMPLE = 8                 # coverage drawn this many times larger, then reduced
FACE, FACE_ALPHA = (75, 75, 75), 128            # shell button face, half see-through
RIM, RIM_ALPHA = (171, 171, 171), 200           # face lit by the bezel highlight, a 1 px edge
PRESSED, PRESSED_ALPHA = (171, 171, 171), 208   # the held face
PRESSED_RIM_ALPHA = 255
RIM_WIDTH = 1


def mask(inset):
    big = Image.new("L", (SIDE * SUPERSAMPLE, SIDE * SUPERSAMPLE), 0)
    draw = ImageDraw.Draw(big)
    i = inset * SUPERSAMPLE
    far = (SIDE * SUPERSAMPLE) - 1 - i
    draw.rounded_rectangle((i, i, far, far), (RADIUS - inset) * SUPERSAMPLE, fill=255)
    return big.resize((SIDE, SIDE), Image.LANCZOS)


def tile(face, face_alpha, rim, rim_alpha):
    """The face inside a rim between the outer outline and one RIM_WIDTH in."""
    outer, inner = mask(0), mask(RIM_WIDTH)
    img = Image.new("RGBA", (SIDE, SIDE))
    px = img.load()
    for y in range(SIDE):
        for x in range(SIDE):
            o, n = outer.getpixel((x, y)) / 255, inner.getpixel((x, y)) / 255
            a_face, a_rim = face_alpha * n, rim_alpha * (o - n)
            a = a_face + a_rim
            c = tuple(round((f * a_face + r * a_rim) / a) if a else 0 for f, r in zip(face, rim))
            px[x, y] = (*c, round(a))
    return img


def main():
    out = sys.argv[1]
    tile(FACE, FACE_ALPHA, RIM, RIM_ALPHA).save(f"{out}/home_tile.png")
    tile(PRESSED, PRESSED_ALPHA, RIM, PRESSED_RIM_ALPHA).save(f"{out}/home_tile_pressed.png")


if __name__ == "__main__":
    main()
