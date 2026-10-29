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

import numpy as np

import gfx_color

# render/r3d_span.h
R3D_DEPTH_EMPTY = 0
R3D_DEPTH_NEAREST = 0xFFFF

# The dump's header (render_lab_render_host.c --dump): width and height as int32.
DUMP_HEADER = struct.Struct("<ii")


def read_dump(path):
    """(colour, depth): (h, w) uint16 arrays of one --dump file, the colour
    RGB565 as a number (gfx_color_t is stored big-endian, for the panel)."""
    data = pathlib.Path(path).read_bytes()
    width, height = DUMP_HEADER.unpack_from(data)
    pixels = width * height
    words = np.frombuffer(data, dtype="<u2", offset=DUMP_HEADER.size, count=2 * pixels)
    colour = words[:pixels].byteswap().reshape(height, width)
    return colour, words[pixels:].reshape(height, width)


def rgb_of(colour):
    """(h, w, 3) uint8 of RGB565 colour, as the framebuffer expands it."""
    return np.stack(gfx_color.expand(colour >> 11, (colour >> 5) & 0x3F, colour & 0x1F), -1).astype(np.uint8)


def basis(forward):
    """(right, up, forward) as poses.camera_basis, world +y up."""
    forward = np.asarray(forward, dtype=float)
    forward = forward / np.linalg.norm(forward)
    right = np.cross(forward, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    return right, np.cross(right, forward), forward


class Lens:
    """The panel's pinhole (panel_w x panel_h, lens = tan of half the short
    side's field of view) seen through a raster of width x height."""

    def __init__(self, panel_w, panel_h, lens, near, width, height):
        short = min(panel_w, panel_h)
        self.half_x = lens * panel_w / short
        self.half_y = lens * panel_h / short
        self.near = near
        self.width, self.height = width, height

    def rays(self):
        """(sx, sy): the pixel centres as camera-space slopes, (h, w) each."""
        x = (np.arange(self.width) + 0.5) * 2 / self.width - 1
        y = 1 - (np.arange(self.height) + 0.5) * 2 / self.height
        return np.meshgrid(x * self.half_x, y * self.half_y)


def warp(colour, depth, lens, pose_from, pose_to):
    """Forward-warp one raster to `pose_to`: (colour, depth, written) with
    nearest-wins splats to the nearest pixel; `written` is False on holes.
    A pose is (eye xyz, forward xyz)."""
    sx, sy = lens.rays()
    drawn = depth != R3D_DEPTH_EMPTY
    z = np.where(drawn, lens.near * R3D_DEPTH_NEAREST / np.maximum(depth, 1), np.inf)
    r0, u0, f0 = basis(pose_from[3:])
    r1, u1, f1 = basis(pose_to[3:])
    direction = f0 + sx[..., None] * r0 + sy[..., None] * u0
    # A drawn pixel moves with the eye; the sky only turns.
    offset = np.asarray(pose_from[:3]) - np.asarray(pose_to[:3])
    point = np.where(drawn[..., None], direction * np.where(drawn, z, 0)[..., None] + offset, direction)
    cz = point @ f1
    px = ((point @ r1) / cz / lens.half_x + 1) * lens.width / 2
    py = (1 - (point @ u1) / cz / lens.half_y) * lens.height / 2
    ix, iy = np.floor(px).astype(np.int64), np.floor(py).astype(np.int64)
    keep = (cz > 0) & (ix >= 0) & (ix < lens.width) & (iy >= 0) & (iy < lens.height)
    # Sky as infinitely far, so splat farthest first and let nearer ones overwrite.
    order_z = np.where(drawn, cz, np.inf)[keep]
    order = np.argsort(-order_z, kind="stable")
    target = (iy[keep] * lens.width + ix[keep])[order]
    out_colour = np.zeros(depth.size, dtype=np.uint16)
    out_depth = np.zeros(depth.size, dtype=np.uint16)
    written = np.zeros(depth.size, dtype=bool)
    out_colour[target] = colour[keep][order]
    out_depth[target] = np.where(np.isinf(order_z[order]), R3D_DEPTH_EMPTY,
                                 np.clip(lens.near * R3D_DEPTH_NEAREST / np.where(np.isinf(order_z[order]), 1, order_z[order]),
                                         1, R3D_DEPTH_NEAREST)).astype(np.uint16)
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
    lc = np.where(has_left, colour[rows, np.clip(left, 0, width - 1)], 0)
    rc = np.where(has_right, colour[rows, np.clip(right, 0, width - 1)], 0)
    # Farther is the smaller inverse depth; empty (sky) is the farthest.
    ld = np.where(has_left, depth[rows, np.clip(left, 0, width - 1)], R3D_DEPTH_NEAREST)
    rd = np.where(has_right, depth[rows, np.clip(right, 0, width - 1)], R3D_DEPTH_NEAREST)
    pick_left = has_left & (~has_right | (ld <= rd))
    fill = np.where(pick_left, lc, rc)
    return np.where(written | ~(has_left | has_right), colour, fill)


def fill_previous(colour, written, previous):
    """Each hole takes the rendered frame's colour at the same pixel."""
    return np.where(written, colour, previous)


def pinholes(written):
    """Holes with a written pixel on both sides along their row or column:
    the cracks a one-pixel splat leaves where the warp stretches."""
    hole = ~written
    pad = np.pad(written, 1, constant_values=False)
    across = pad[1:-1, :-2] & pad[1:-1, 2:]
    down = pad[:-2, 1:-1] & pad[2:, 1:-1]
    return hole & (across | down)
