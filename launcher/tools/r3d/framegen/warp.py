"""Frame generation by depth reprojection: forward-warp one rendered raster
(RGB565 colour plus r3d inverse depth) from the camera pose it was drawn at
to a later pose, and fill what the warp leaves empty.

The raster is the render size, stretched to the panel on the way out, so a
pixel's ray is the one poses.camera_rays casts through the same place of
the panel. A pixel's view depth is near_z / iz, with iz its stored depth
over R3D_DEPTH_NEAREST; R3D_DEPTH_EMPTY marks the sky, which warps as a
direction (rotation only) and loses to anything drawn.
"""
import pathlib
import struct
import sys

import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(TOOLS), str(TOOLS / "device")]
import gfx_color  # noqa: E402
from build.c_header import read_define  # noqa: E402
from r3d.poses import camera_basis  # noqa: E402

SPAN_HEADER = TOOLS.parent / "main" / "render" / "r3d_span.h"
R3D_DEPTH_EMPTY = read_define(SPAN_HEADER, "R3D_DEPTH_EMPTY")
# UINT16_MAX in the header: the largest a depth word holds.
R3D_DEPTH_NEAREST = int(np.iinfo(np.uint16).max)

# A native RGB565 word's six green bits, and five red or blue.
RGB565_GREEN_MASK = 0x3F
RGB565_RED_BLUE_MASK = 0x1F

# The dump's header (render_lab_render_host.c --dump): width and height as int32.
DUMP_HEADER = struct.Struct("<ii")


def read_dump(path):
    """(colour, depth): (h, w) uint16 arrays of one --dump file, the colour
    native RGB565 (gfx_color_t is stored in panel order)."""
    data = pathlib.Path(path).read_bytes()
    width, height = DUMP_HEADER.unpack_from(data)
    pixels = width * height
    words = np.frombuffer(data, dtype="<u2", offset=DUMP_HEADER.size, count=2 * pixels)
    return gfx_color.swap(words[:pixels]).reshape(height, width), words[pixels:].reshape(height, width)


def rgb_of(colour):
    """(h, w, 3) uint8 of native RGB565 colour, as the framebuffer expands it."""
    return np.stack(gfx_color.expand(colour >> 11, (colour >> 5) & RGB565_GREEN_MASK, colour & RGB565_RED_BLUE_MASK),
                    -1).astype(np.uint8)


class Lens:
    """The panel's pinhole (panel_w x panel_h, lens = tan of half the short
    side's field of view) seen through a raster of width x height."""

    def __init__(self, panel_w, panel_h, lens, near, width, height):
        # As poses.camera_rays spreads its rays.
        short = min(panel_w, panel_h)
        self.half_x, self.half_y = lens * panel_w / short, lens * panel_h / short
        self.near = near
        self.width, self.height = width, height

    def rays(self):
        """(sx, sy): the pixel centres as camera-space slopes, (h, w) each."""
        x = (np.arange(self.width) + 0.5) * 2 / self.width - 1
        y = 1 - (np.arange(self.height) + 0.5) * 2 / self.height
        return np.meshgrid(x * self.half_x, y * self.half_y)


def warp(colour, depth, lens, pose_from, pose_to):
    """Forward-warp one raster to `pose_to`: (colour, depth, written), each
    pixel splatted to the nearest one with the nearest point winning;
    `written` is False on holes. A pose is (eye xyz, forward xyz)."""
    sx, sy = lens.rays()
    drawn = depth != R3D_DEPTH_EMPTY
    r0, u0, f0 = camera_basis(pose_from[3:])
    r1, u1, f1 = camera_basis(pose_to[3:])
    direction = f0 + sx[..., None] * r0 + sy[..., None] * u0
    # A drawn pixel moves with the eye; the sky only turns.
    z = lens.near * R3D_DEPTH_NEAREST / np.maximum(depth, 1)
    offset = np.asarray(pose_from[:3]) - np.asarray(pose_to[:3])
    point = np.where(drawn[..., None], direction * z[..., None] + offset, direction)
    cz = point @ f1
    ix = np.floor(((point @ r1) / cz / lens.half_x + 1) * lens.width / 2).astype(np.int64)
    iy = np.floor((1 - (point @ u1) / cz / lens.half_y) * lens.height / 2).astype(np.int64)
    keep = (cz > 0) & (ix >= 0) & (ix < lens.width) & (iy >= 0) & (iy < lens.height)
    # Nearest first, the sky last; each target takes its first splat.
    order = np.argsort(np.where(drawn, cz, np.inf)[keep], kind="stable")
    target, first = np.unique((iy[keep] * lens.width + ix[keep])[order], return_index=True)
    chosen = order[first]
    source_cz, source_drawn = cz[keep][chosen], drawn[keep][chosen]
    out_colour = np.zeros(depth.size, dtype=np.uint16)
    out_depth = np.full(depth.size, R3D_DEPTH_EMPTY, dtype=np.uint16)
    written = np.zeros(depth.size, dtype=bool)
    out_colour[target] = colour[keep][chosen]
    near_iz = lens.near * R3D_DEPTH_NEAREST / np.where(source_drawn, source_cz, 1)
    out_depth[target] = np.where(source_drawn, np.clip(near_iz, 1, R3D_DEPTH_NEAREST), R3D_DEPTH_EMPTY)
    written[target] = True
    shape = depth.shape
    return out_colour.reshape(shape), out_depth.reshape(shape), written.reshape(shape)


def fill_row(colour, depth, written):
    """Each hole takes the farther of its nearest written neighbours left and
    right on its row (a disocclusion uncovers background), the one written
    neighbour at a row end; a row with none keeps its holes."""
    height, width = colour.shape
    index = np.broadcast_to(np.arange(width), (height, width))
    left = np.maximum.accumulate(np.where(written, index, -1), axis=1)
    right = np.minimum.accumulate(np.where(written, index, width)[:, ::-1], axis=1)[:, ::-1]
    rows = np.arange(height)[:, None]
    has_left, has_right = left >= 0, right < width
    left, right = np.clip(left, 0, width - 1), np.clip(right, 0, width - 1)
    # Farther is the smaller inverse depth; empty (sky) is the farthest.
    ld = np.where(has_left, depth[rows, left], R3D_DEPTH_NEAREST)
    rd = np.where(has_right, depth[rows, right], R3D_DEPTH_NEAREST)
    pick_left = has_left & (~has_right | (ld <= rd))
    fill = np.where(pick_left, colour[rows, left], colour[rows, right])
    return np.where(written | ~(has_left | has_right), colour, fill)


def fill_previous(colour, written, previous):
    """Each hole takes the rendered frame's colour at the same pixel."""
    return np.where(written, colour, previous)


def pinholes(written):
    """Holes with a written pixel on both sides along their row or column:
    the cracks a one-pixel splat leaves where the warp stretches."""
    pad = np.pad(written, 1, constant_values=False)
    across = pad[1:-1, :-2] & pad[1:-1, 2:]
    down = pad[:-2, 1:-1] & pad[2:, 1:-1]
    return ~written & (across | down)
