"""The case as a 3D figure: translucent walls, screen, IMU and both axis sets, from board/board_geometry.h.

Device axes: x right, y up the screen, z out of the glass; millimetres; origin at the glass centre.
A pose is a 3x3 matrix whose columns are the device axes in the figure's world axes; world z is up.
DeviceAnimation is the input figures' one animation: the posed device with its direction vectors on the left,
scalar signals over time on the right, a time cursor in sync.
"""
from pathlib import Path

import numpy as np
from matplotlib.lines import Line2D
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

from math_figures import run_probe
from docs.plot_style import FAINT, GIF_FPS, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, figure, save_gif

PROBE = Path(__file__).with_name("device_model_probe.c")
CORNER_SEGMENTS = 8
BODY_ALPHA = 0.22
# The PCB is a faint reference behind the IMU, inside the translucent case.
PCB_ALPHA = 0.4
GLASS_ALPHA = 0.5
GLASS = MUTED
BODY = FAINT
IMU = INK
CHIP_AXES = SLOTS[0]
SCREEN_AXES = SLOTS[2]
EDGE_WIDTH = 0.2
# Axis arrow length as a share of the case height.
AXIS_SHARE = 0.32
ARROW_RATIO = 0.15
LABEL_PAST = 1.12
WORLD_DOWN = np.array([0.0, 0.0, -1.0])
VIEW_ELEVATION = 22
VIEW_AZIMUTH = -60
ANIMATION_DPI = 80
# Palette size: the shaded device takes many entries, and every line and arrow colour must keep its own.
ANIMATION_COLORS = 128
ANIMATION_SIZE = (10, 5.4)
# Vector unit length and scene margin as shares of the case height.
VECTOR_SHARE = 0.5
MARGIN_SHARE = 0.05
# The cube projection leaves room around its inscribed sphere without this zoom.
SCENE_ZOOM = 1.6
# A sphere clears every case pose; vectors start this share of the case height beyond it.
BESIDE_SHARE = 0.1
# Figure shares: the scene on the left, the signal plots stacked on the right, the legend under both.
SCENE = (0, 0.08, 0.62, 0.84)
PLOTS = (0.68, 0.2, 0.3, 0.64)
PLOT_GAP = 0.1
TITLE_Y = 0.94
LEGEND_Y = 0.02
LEGEND_COLUMNS = 6


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
        self.height = geometry["case_height"]
        self.depth = geometry["case_depth"]
        self.width = geometry["case_width"]
        self.radius = np.linalg.norm((self.width, self.height, self.depth)) / 2
        self.imu_centre = np.array(geometry["imu_centre"])
        self.chip_axes = np.array(geometry["chip_axes"], dtype=float)
        # The code's screen axes x right, y down, z out: minus where imu_axes_screen() sends the chip axes,
        # because the chip reads specific force (up) and the code calls that vector down.
        self.screen_axes = -np.array(geometry["mapped_axes"], dtype=float)
        outline = rounded_outline(self.width, self.height, geometry["case_corner"])
        front = np.c_[outline, np.zeros(len(outline))]
        back = np.c_[outline, np.full(len(outline), -self.depth)]
        screen = rounded_outline(geometry["screen_width"], geometry["screen_height"], 0)
        self.glass = np.c_[screen, np.zeros(len(screen))]
        self.case_outlines = (front, back)
        # rounded_outline keeps the same point count for every radius, pairing bezel ring vertices.
        self.body = [np.array([front[i], front[i + 1], other[i + 1], other[i]])
                     for other in (back, self.glass) for i in range(len(outline) - 1)] + [back]
        corners = np.array([(1, 1), (-1, 1), (-1, -1), (1, -1)]) * geometry["back_side"] / 2
        square = np.vstack((corners, corners[:1]))
        radius = geometry["screw_radius"]
        circle = rounded_outline(2 * radius, 2 * radius, radius)
        self.back_details = [np.c_[line, np.full(len(line), -self.depth)]
                             for line in [square] + [circle + corner for corner in corners]]
        pcb = rounded_outline(geometry["width"], geometry["height"], geometry["corner"])
        self.pcb = np.c_[pcb, np.full(len(pcb), -geometry["depth"])]
        self.imu = box_faces(self.imu_centre, self.chip_axes.T @ np.array(geometry["imu_size"]))

    def to_device(self, screen_vector):
        """A vector in the code's screen axes as a device vector."""
        return np.asarray(screen_vector) @ self.screen_axes

    def draw(self, axis, pose, chip_axes=True, screen_axes=True):
        """Draw the posed device into a 3D axis made with computed_zorder=False, so arrows stay over the faces."""
        pose = np.asarray(pose)
        axis.add_collection3d(Poly3DCollection([face @ pose.T for face in self.body], facecolor=BODY,
                                               edgecolor="none", alpha=BODY_ALPHA))
        axis.add_collection3d(Poly3DCollection([self.glass @ pose.T], facecolor=GLASS, edgecolor=GLASS,
                                               alpha=GLASS_ALPHA))
        for line in (*self.case_outlines, *self.back_details):
            axis.plot(*(line @ pose.T).T, color=MUTED, linewidth=EDGE_WIDTH)
        axis.plot(*(self.pcb @ pose.T).T, color=MUTED, linewidth=EDGE_WIDTH, alpha=PCB_ALPHA)
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


def pose_from_down(down, yaw=0.0):
    """The pose that turns device vector `down` onto world down by the smallest rotation (Rodrigues), then `yaw`
    radians about world down's axis: a down vector cannot tell that turn, so the caller picks it."""
    down = np.asarray(down, dtype=float) / np.linalg.norm(down)
    axis = np.cross(down, WORLD_DOWN)
    sine, cosine = np.linalg.norm(axis), down @ WORLD_DOWN
    if sine < 1e-9:
        tilt = np.eye(3) if cosine > 0 else np.diag((1.0, -1.0, -1.0))
    else:
        k = axis / sine
        cross = np.array(((0, -k[2], k[1]), (k[2], 0, -k[0]), (-k[1], k[0], 0)))
        tilt = np.eye(3) + sine * cross + (1 - cosine) * cross @ cross
    turn = np.array(((np.cos(yaw), -np.sin(yaw), 0), (np.sin(yaw), np.cos(yaw), 0), (0, 0, 1)))
    return turn @ tilt


def facing_yaw(down, azimuth=VIEW_AZIMUTH):
    """The yaw for pose_from_down that turns the glass towards a camera at `azimuth` degrees."""
    normal = pose_from_down(down) @ np.array([0.0, 0.0, 1.0])
    return np.radians(azimuth) - np.arctan2(normal[1], normal[0])


class DeviceAnimation:
    """The posed device with direction vectors on the left and scalar signal plots on the right, in sync.

    Fill and style each of `plots` (top first) before saving; `frame` redraws the scene and moves every plot's
    time cursor. Vectors are (start, vector, colour, width) in world axes, millimetres.
    """

    def __init__(self, model, plots=1):
        self.model = model
        self.fig = figure(ANIMATION_SIZE)
        self.fig.set_layout_engine(None)
        # Arrows draw over the translucent device in call order, not depth order, so their colours stay true.
        self.scene = self.fig.add_axes(SCENE, projection="3d", computed_zorder=False)
        left, bottom, width, height = PLOTS
        each = (height - PLOT_GAP * (plots - 1)) / plots
        self.plots = [self.fig.add_axes((left, bottom + (plots - 1 - row) * (each + PLOT_GAP), width, each))
                      for row in range(plots)]
        self.cursors = [plot.axvline(0, color=INK, linewidth=1) for plot in self.plots]
        self.title = self.fig.text(0.5, TITLE_Y, "", ha="center", fontsize=TITLE_SIZE, color=INK)
        self.unit = VECTOR_SHARE * model.height
        away = model.radius + BESIDE_SHARE * model.height
        self.extent = away + self.unit + MARGIN_SHARE * model.height
        right = np.radians(VIEW_AZIMUTH + 90)
        right = away * np.array([np.cos(right), np.sin(right), 0.0])
        # World down is drawn on the right, centred on the device's height; the device's own motion on the left.
        self.beside = right + (0, 0, self.unit / 2)
        self.left = -right

    def legend(self, handles):
        self.fig.legend(handles=handles, loc="lower center", bbox_to_anchor=(0.5, LEGEND_Y),
                        ncol=min(len(handles), LEGEND_COLUMNS), fontsize=LABEL_SIZE, frameon=False)

    def frame(self, pose, vectors, time, title, azimuth=VIEW_AZIMUTH, **axes):
        """Draw one frame: the device in `pose` (draw's axis-set switches in `axes`), then the vectors over it."""
        scene, extent = self.scene, self.extent
        scene.cla()
        scene.set_axis_off()
        for setter in (scene.set_xlim, scene.set_ylim, scene.set_zlim):
            setter(-extent, extent)
        scene.set_box_aspect((1, 1, 1), zoom=SCENE_ZOOM)
        scene.view_init(elev=VIEW_ELEVATION, azim=azimuth)
        self.model.draw(scene, pose, **axes)
        for start, vector, colour, width in vectors:
            scene.quiver(*start, *vector, color=colour, arrow_length_ratio=ARROW_RATIO, linewidth=width)
        self.title.set_text(title)
        for cursor in self.cursors:
            cursor.set_xdata([time] * 2)

    def save(self, output, frames, draw_frame, fps=GIF_FPS):
        self.fig.set_dpi(ANIMATION_DPI)
        save_gif(self.fig, draw_frame, frames, output, fps, colors=ANIMATION_COLORS)
