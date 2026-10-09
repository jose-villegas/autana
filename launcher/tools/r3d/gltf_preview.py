#!/usr/bin/env python3
"""Render any skinned glTF on the CPU: a looping GIF of one animation, or a
sheet of the bind pose from front, side, top and three-quarter views.
render() also draws colours lit elsewhere, one per vertex, interpolated.

    python tools/r3d/gltf_preview.py ASSET.glb --gif walk --out walk.gif
    python tools/r3d/gltf_preview.py ASSET.glb --sheet --out bind.png

Run from launcher/. Needs Pillow. Flat-shaded, z-buffered, with each triangle's vertical shadow
on a checkered ground at y = 0, so a sliding or sinking foot shows against
the checks. The camera frames the bind pose once and never moves, so an
animation's motion is not hidden by re-framing.
"""

import argparse
import math
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from r3d import gltf_skin  # noqa: E402

LIGHT = (0.45, 0.80, 0.40)
AMBIENT = 0.38
BACKGROUND = (206, 214, 222)
GROUND = ((188, 196, 176), (172, 181, 160))
SHADOW = (128, 134, 122)
SUPERSAMPLE = 2
VIEWS = {"front": (0.0, 8.0), "side": (90.0, 5.0), "top": (0.0, 88.0), "three-quarter": (38.0, 18.0)}


def normalize(v):
    length = math.sqrt(sum(x * x for x in v)) or 1.0
    return tuple(x / length for x in v)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def linear_to_srgb(c):
    c = min(max(c, 0.0), 1.0)
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1.0 / 2.4) - 0.055


class Camera:
    """A perspective orbit camera framing a bounding sphere."""

    def __init__(self, centre, radius, yaw_deg, pitch_deg, width, height):
        yaw, pitch = math.radians(yaw_deg), math.radians(pitch_deg)
        distance = radius * 4.0
        self.eye = (
            centre[0] + distance * math.cos(pitch) * math.sin(yaw),
            centre[1] + distance * math.sin(pitch),
            centre[2] + distance * math.cos(pitch) * math.cos(yaw),
        )
        self.forward = normalize(tuple(c - e for c, e in zip(centre, self.eye)))
        self.right = normalize(cross(self.forward, (0.0, 1.0, 0.0)))
        self.up = cross(self.right, self.forward)
        self.width, self.height = width, height
        half_angle = math.asin(radius / distance)
        self.focal = 0.5 * min(width, height) / math.tan(half_angle) * 0.96

    def project(self, p):
        d = tuple(a - b for a, b in zip(p, self.eye))
        depth = sum(a * b for a, b in zip(d, self.forward))
        x = sum(a * b for a, b in zip(d, self.right)) / depth
        y = sum(a * b for a, b in zip(d, self.up)) / depth
        return (self.width / 2.0 + x * self.focal, self.height / 2.0 - y * self.focal, depth)


def bounds(positions):
    lo = [min(p[k] for p in positions) for k in range(3)]
    hi = [max(p[k] for p in positions) for k in range(3)]
    centre = tuple((a + b) / 2.0 for a, b in zip(lo, hi))
    radius = 0.5 * math.sqrt(sum((b - a) ** 2 for a, b in zip(lo, hi)))
    return centre, radius


def draw_ground(image, camera, positions, triangles, extent):
    draw = ImageDraw.Draw(image)
    step = 0.1
    cells = int(extent / step)
    for i in range(-cells, cells):
        for j in range(-cells, cells):
            corners = [(i * step, j * step), ((i + 1) * step, j * step),
                       ((i + 1) * step, (j + 1) * step), (i * step, (j + 1) * step)]
            points = [camera.project((x, 0.0, z))[:2] for x, z in corners]
            draw.polygon(points, fill=GROUND[(i + j) % 2])
    for a, b, c in triangles:
        points = [camera.project((positions[v][0], 0.0, positions[v][2]))[:2] for v in (a, b, c)]
        draw.polygon(points, fill=SHADOW)


def flat_shading(positions, colors):
    """Corner colours for rasterize(): each triangle one colour, its vertex
    colours' mean lit by LIGHT through the face normal, as sRGB."""
    light = normalize(LIGHT)

    def corners(a, b, c):
        pos = [positions[v] for v in (a, b, c)]
        normal = normalize(cross(tuple(q - p for p, q in zip(pos[0], pos[1])),
                                 tuple(q - p for p, q in zip(pos[0], pos[2]))))
        shade = AMBIENT + (1.0 - AMBIENT) * max(0.0, sum(x * y for x, y in zip(normal, light)))
        base = [sum(colors[v][k] for v in (a, b, c)) / 3.0 for k in range(3)]
        color = tuple(int(round(255 * linear_to_srgb(ch * shade))) for ch in base)
        return color, color, color

    return corners


def rasterize(image, camera, positions, triangles, corners):
    """Z-buffers the front faces; corners(a, b, c) gives a triangle's three
    8-bit corner colours, interpolated across it."""
    width, height = image.size
    pixels = image.load()
    depth = [math.inf] * (width * height)
    projected = [camera.project(p) for p in positions]
    for a, b, c in triangles:
        pa, pb, pc = projected[a], projected[b], projected[c]
        if (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pc[0] - pa[0]) * (pb[1] - pa[1]) >= 0.0:
            continue
        fill_triangle(pixels, depth, width, height, (pa, pb, pc), corners(a, b, c))


def fill_triangle(pixels, depth, width, height, points, corners):
    """`corners` is one RGB tuple per point, interpolated."""
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = points
    flat = corners[0] if corners[0] == corners[1] == corners[2] else None
    min_y = max(int(math.ceil(min(y0, y1, y2) - 0.5)), 0)
    max_y = min(int(math.floor(max(y0, y1, y2) - 0.5)), height - 1)
    min_x = max(int(math.ceil(min(x0, x1, x2) - 0.5)), 0)
    max_x = min(int(math.floor(max(x0, x1, x2) - 0.5)), width - 1)
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if area == 0.0:
        return
    edges = ((x1, y1, x2, y2), (x2, y2, x0, y0), (x0, y0, x1, y1))
    for py in range(min_y, max_y + 1):
        cy = py + 0.5
        lo, hi = min_x, max_x
        for ax, ay, bx, by in edges:
            # w(x) = (bx-ax)*(cy-ay) - (by-ay)*(x-ax) keeps area's sign inside.
            slope = -(by - ay)
            offset = (bx - ax) * (cy - ay) + (by - ay) * ax
            if area < 0.0:
                slope, offset = -slope, -offset
            if slope > 0.0:
                lo = max(lo, int(math.ceil(-offset / slope - 0.5)))
            elif slope < 0.0:
                hi = min(hi, int(math.floor(-offset / slope - 0.5)))
            elif offset < 0.0:
                lo, hi = 1, 0
        if lo > hi:
            continue
        row = py * width
        for px in range(lo, hi + 1):
            cx = px + 0.5
            w0 = ((x2 - x1) * (cy - y1) - (y2 - y1) * (cx - x1)) / area
            w1 = ((x0 - x2) * (cy - y2) - (y0 - y2) * (cx - x2)) / area
            z = w0 * z0 + w1 * z1 + (1.0 - w0 - w1) * z2
            if z < depth[row + px]:
                depth[row + px] = z
                if flat:
                    pixels[px, py] = flat
                else:
                    w2 = 1.0 - w0 - w1
                    pixels[px, py] = tuple(int(round(w0 * p + w1 * q + w2 * r)) for p, q, r in zip(*corners))


def render(asset, camera, positions, size, lit=None):
    """Flat-shaded by LIGHT, or with `lit`, per-vertex 8-bit RGB, interpolated."""
    width, height = size
    image = Image.new("RGB", (width * SUPERSAMPLE, height * SUPERSAMPLE), BACKGROUND)
    draw_ground(image, camera, positions, asset.triangles, 0.8)
    if lit is None:
        corners = flat_shading(positions, asset.colors or [(0.6, 0.6, 0.6)] * len(positions))
    else:
        def corners(a, b, c):
            return lit[a], lit[b], lit[c]
    rasterize(image, camera, positions, asset.triangles, corners)
    return image.resize(size, Image.LANCZOS)


def make_camera(asset, view, size):
    centre, radius = bounds(asset.positions)
    yaw, pitch = VIEWS[view]
    return Camera(centre, radius * 1.05, yaw, pitch,
                  size[0] * SUPERSAMPLE, size[1] * SUPERSAMPLE)


def animation_frames(asset, name, size, view, fps):
    camera = make_camera(asset, view, size)
    duration = asset.duration(name)
    count = max(1, int(round(duration * fps)))
    frames = []
    for i in range(count):
        positions, _ = asset.skin(asset.sample(name, duration * i / count))
        frames.append(render(asset, camera, positions, size))
    return frames


def bind_sheet(asset, size):
    tiles = []
    positions, _ = asset.skin(asset.sample(None, 0.0))
    for view in VIEWS:
        tile = render(asset, make_camera(asset, view, size), positions, size)
        ImageDraw.Draw(tile).text((6, 4), view, fill=(40, 40, 40), stroke_width=1, stroke_fill=(255, 255, 255))
        tiles.append(tile)
    sheet = Image.new("RGB", (size[0] * 2, size[1] * 2))
    for index, tile in enumerate(tiles):
        sheet.paste(tile, ((index % 2) * size[0], (index // 2) * size[1]))
    return sheet


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("asset")
    parser.add_argument("--out", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--gif", metavar="ANIMATION", help="render this animation as a looping GIF")
    mode.add_argument("--sheet", action="store_true", help="render the bind pose from four views")
    parser.add_argument("--view", default="three-quarter", choices=sorted(VIEWS))
    parser.add_argument("--size", default="368x448", help="WIDTHxHEIGHT of one frame or tile")
    parser.add_argument("--fps", type=int, default=30)
    args = parser.parse_args()
    size = tuple(int(v) for v in args.size.lower().split("x"))
    asset = gltf_skin.SkinnedAsset(*gltf_skin.load_asset(args.asset))
    if args.sheet:
        bind_sheet(asset, size).save(args.out)
        return
    frames = animation_frames(asset, args.gif, size, args.view, args.fps)
    frames[0].save(args.out, save_all=True, append_images=frames[1:],
                   duration=int(round(1000.0 / args.fps)), loop=0, disposal=2)


if __name__ == "__main__":
    main()
