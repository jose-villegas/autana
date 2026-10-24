"""The device as a 3D figure: rounded box, IMU and both axis sets, from board/board_geometry.h.

Device axes: x right, y up the screen, z out of the glass; millimetres; origin at the glass centre.
A pose is a 3x3 matrix whose columns are the device axes in the figure's world axes.
"""
from pathlib import Path

import numpy as np
from matplotlib.lines import Line2D
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

from math_figures import run_probe
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS

PROBE = Path(__file__).with_name("device_model_probe.c")
CORNER_SEGMENTS = 8
BODY_ALPHA = 0.22
GLASS_ALPHA = 0.5
GLASS = MUTED
BODY = FAINT
IMU = INK
CHIP_AXES = SLOTS[0]
SCREEN_AXES = SLOTS[2]
EDGE_WIDTH = 0.2
# Axis arrow length as a share of the device height.
AXIS_SHARE = 0.32
ARROW_RATIO = 0.15
LABEL_PAST = 1.12


def rounded_outline(width, height, radius):
    """Plan-view outline, counter-clockwise from the right edge, first point repeated last."""
    points = []
    for quarter, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        centre = np.array([sx * (width / 2 - radius), sy * (height / 2 - radius)])
        for step in range(CORNER_SEGMENTS + 1):
            angle = np.pi / 2 * (quarter + step / CORNER_SEGMENTS)
            points.append(centre + radius * np.array([np.cos(angle), np.sin(angle)]))
    return np.array(points + points[:1])


def box_faces(centre, size):
    half = np.abs(size) / 2
    corners = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]) * half + centre
    quads = ((0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5))
    return [corners[list(q)] for q in quads]


class DeviceModel:
    """The device's faces and axes in device axes, built once from the board geometry."""

    def __init__(self, cc):
        geometry = run_probe(cc, (PROBE,))
        self.height = geometry["height"]
        self.depth = geometry["depth"]
        self.width = geometry["width"]
        self.imu_centre = np.array(geometry["imu_centre"])
        self.chip_axes = np.array(geometry["chip_axes"], dtype=float)
        # The code's screen axes x right, y down, z out: minus where imu_axes_screen() sends the chip axes,
        # because the chip reads specific force (up) and the code calls that vector down.
        self.screen_axes = -np.array(geometry["mapped_axes"], dtype=float)
        outline = rounded_outline(self.width, self.height, geometry["corner"])
        self.glass = np.c_[outline, np.zeros(len(outline))]
        back = np.c_[outline, np.full(len(outline), -self.depth)]
        self.body = [np.array([self.glass[i], self.glass[i + 1], back[i + 1], back[i]])
                     for i in range(len(outline) - 1)] + [back]
        self.imu = box_faces(self.imu_centre, self.chip_axes.T @ np.array(geometry["imu_size"]))

    def to_device(self, screen_vector):
        """A vector in the code's screen axes as a device vector."""
        return np.asarray(screen_vector) @ self.screen_axes

    def draw(self, axis, pose, chip_axes=True, screen_axes=True):
        """Draw the posed device into a 3D axis made with computed_zorder=False, so arrows stay over the faces."""
        pose = np.asarray(pose)
        axis.add_collection3d(Poly3DCollection([face @ pose.T for face in self.body], facecolor=BODY,
                                               edgecolor=MUTED, linewidth=EDGE_WIDTH, alpha=BODY_ALPHA))
        axis.add_collection3d(Poly3DCollection([self.glass @ pose.T], facecolor=GLASS, edgecolor=GLASS,
                                               alpha=GLASS_ALPHA))
        axis.add_collection3d(Poly3DCollection([face @ pose.T for face in self.imu], facecolor=IMU, edgecolor=IMU))
        length = AXIS_SHARE * self.height
        sets = ((chip_axes, pose @ self.imu_centre, self.chip_axes, "XYZ", CHIP_AXES),
                (screen_axes, np.zeros(3), self.screen_axes, "xyz", SCREEN_AXES))
        for shown, origin, vectors, names, colour in sets:
            for vector, name in zip(vectors if shown else (), names):
                arrow = pose @ vector * length
                axis.quiver(*origin, *arrow, color=colour, arrow_length_ratio=ARROW_RATIO)
                axis.text(*(origin + arrow * LABEL_PAST), name, color=colour, fontsize=LABEL_SIZE, ha="center")


def legend_handles():
    """Legend entries for the two axis sets."""
    return [Line2D([], [], color=CHIP_AXES, label="Chip axes X Y Z"),
            Line2D([], [], color=SCREEN_AXES, label="Screen axes x y z")]
