"""Plot gravity correction trust, and the device rocking while the filter learns a gyro bias; --cc CC --out DIR."""
import numpy as np
from matplotlib.lines import Line2D
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

from input_figures import probe_data
from docs.plot_style import (FAINT, GIF_FPS, INK, LABEL_SIZE, MUTED, ORBIT_GIF_COLORS, SLOTS, TITLE_SIZE, figure,
                             save, save_gif, style_axes, subplots)

ANIMATION_DPI = 80
FIGURE_SIZE = (10, 5.4)
VIEW_ELEVATION = 22
# The camera swings this far either side of its start over one loop, so the last frame meets the first.
VIEW_AZIMUTH = -60
VIEW_SWING = 35
CORNER_SEGMENTS = 8
DEVICE_ALPHA = 0.22
GLASS_ALPHA = 0.5
GLASS = MUTED
BODY = FAINT
IMU = INK
CHIP_AXES = SLOTS[0]
SCREEN_AXES = SLOTS[2]
TRUE_DOWN = INK
ESTIMATE = SLOTS[1]
ESTIMATE_WIDTH = 3
# Arrow lengths and the scene's half extent, as shares of the device height.
AXIS_SHARE = 0.32
DOWN_SHARE = 0.5
EXTENT_SHARE = 0.55
ARROW_RATIO = 0.15
LABEL_PAST = 1.12
# Figure shares: the scene on the left, the bias error plot on the right, the legend under both.
SCENE = (0, 0.08, 0.62, 0.84)
PLOT = (0.68, 0.2, 0.3, 0.64)
TITLE_Y = 0.94
LEGEND_Y = 0.02
LEGEND_COLUMNS = 4
# The down vectors start beside the device: this share of the device height past its right edge.
DOWN_CLEARANCE_SHARE = 0.1


def rounded_outline(width, height, radius):
    """Plan-view outline of the device, counter-clockwise from the right edge."""
    corners = ((1, 1, 0), (-1, 1, 1), (-1, -1, 2), (1, -1, 3))
    points = []
    for sx, sy, quarter in corners:
        centre = np.array([sx * (width / 2 - radius), sy * (height / 2 - radius)])
        for step in range(CORNER_SEGMENTS + 1):
            angle = np.pi / 2 * (quarter + step / CORNER_SEGMENTS)
            points.append(centre + radius * np.array([np.cos(angle), np.sin(angle)]))
    return np.array(points)


def box_faces(centre, size):
    half = np.asarray(size) / 2
    corners = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]) * half + centre
    quads = ((0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5))
    return [corners[list(q)] for q in quads]


def bias_errors(data, screen):
    """In-plane and along-gravity bias error per frame, in deg/s, in device axes."""
    plane, along = [], []
    for frame in data["frames"]:
        error = (np.array(frame["bias"]) - data["bias_dps"]) @ screen
        down = np.array(frame["true_down"])
        along.append(abs(error @ down))
        plane.append(np.linalg.norm(error - (error @ down) * down))
    return np.array(plane), np.array(along)


def linearised(data, start, times):
    """Bias error of the linearised loop from `start` with no initial tilt error: both poles of s^2 + s/TAU + KI."""
    fast, slow = sorted(np.roots([1, 1 / data["tau_s"], data["ki"]]).real)
    return start * (fast * np.exp(slow * times) - slow * np.exp(fast * times)) / (fast - slow)


def bias_figure(data, output):
    device, imu = data["device"], data["imu"]
    # The documented screen axes as device vectors: the code's down is minus the chip's specific force.
    screen = -np.array(data["screen_axes"], dtype=float)
    chip = np.array(imu["axes"], dtype=float)
    outline = rounded_outline(device["width"], device["height"], device["corner"])
    glass = np.c_[outline, np.zeros(len(outline))]
    back = np.c_[outline, np.full(len(outline), -device["depth"])]
    sides = [np.array([glass[i], glass[i + 1], back[i + 1], back[i]]) for i in range(len(outline) - 1)]
    chip_box = box_faces(imu["centre"], chip.T @ np.array(imu["size"]))
    times = np.array([frame["t"] for frame in data["frames"]])
    plane, along = bias_errors(data, screen)
    predicted = linearised(data, plane[0], times)
    axis_length = AXIS_SHARE * device["height"]
    down_length = DOWN_SHARE * device["height"]
    extent = EXTENT_SHARE * device["height"]

    fig = figure(FIGURE_SIZE)
    fig.set_layout_engine(None)
    # Arrows draw over the translucent device in call order, not depth order, so their colours stay true.
    scene = fig.add_axes(SCENE, projection="3d", computed_zorder=False)
    inset = fig.add_axes(PLOT)
    title = fig.text(0.5, TITLE_Y, "", ha="center", fontsize=TITLE_SIZE, color=INK)

    inset.plot(times, predicted, color=MUTED, linestyle="--", label="Linearised loop, across")
    inset.plot(times, plane, color=ESTIMATE, label="Across gravity")
    inset.plot(times, along, color=SLOTS[3], label="Along gravity")
    inset.axvline(data["settle_s"], color=FAINT, linestyle=":")
    cursor = inset.axvline(0, color=INK, linewidth=1)
    inset.set(xlim=(0, times[-1]), ylim=(0, max(plane.max(), along.max()) * 1.1))
    inset.set_xlabel("Time (s)", fontsize=LABEL_SIZE)
    inset.set_ylabel("Bias error (deg/s)", fontsize=LABEL_SIZE)
    inset.legend(fontsize=LABEL_SIZE, loc="upper right", frameon=False)
    style_axes(inset, "Gyro bias error")
    entries = (("True down", TRUE_DOWN), ("Filter estimate", ESTIMATE), ("Chip axes X Y Z", CHIP_AXES),
               ("Screen axes x y z", SCREEN_AXES))
    fig.legend(handles=[Line2D([], [], color=colour, label=label) for label, colour in entries],
               loc="lower center", bbox_to_anchor=(0.5, LEGEND_Y), ncol=LEGEND_COLUMNS, fontsize=LABEL_SIZE,
               frameon=False)
    down_at = np.array([device["width"] / 2 + DOWN_CLEARANCE_SHARE * device["height"], 0, down_length / 2])

    def place(pose, points):
        return np.asarray(points) @ pose.T

    def labelled_arrow(origin, vector, colour, text=None):
        scene.quiver(*origin, *vector, color=colour, arrow_length_ratio=ARROW_RATIO)
        if text:
            scene.text(*(origin + vector * LABEL_PAST), text, color=colour, fontsize=LABEL_SIZE, ha="center")

    def draw(index):
        frame = data["frames"][index]
        pose = np.array(frame["pose"])
        scene.cla()
        scene.set_axis_off()
        for setter in (scene.set_xlim, scene.set_ylim, scene.set_zlim):
            setter(-extent, extent)
        scene.set_box_aspect((1, 1, 1))
        swing = VIEW_SWING * np.sin(2 * np.pi * index / len(data["frames"]))
        scene.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH + swing)
        body = [place(pose, face) for face in sides + [back]]
        scene.add_collection3d(Poly3DCollection(body, facecolor=BODY, edgecolor=MUTED, linewidth=0.2,
                                                alpha=DEVICE_ALPHA))
        scene.add_collection3d(Poly3DCollection([place(pose, glass)], facecolor=GLASS, edgecolor=GLASS,
                                                alpha=GLASS_ALPHA))
        scene.add_collection3d(Poly3DCollection([place(pose, face) for face in chip_box], facecolor=IMU,
                                                edgecolor=IMU))
        imu_at = place(pose, imu["centre"])
        for vector, name in zip(chip, "XYZ"):
            labelled_arrow(imu_at, pose @ vector * axis_length, CHIP_AXES, name)
        for vector, name in zip(screen, "xyz"):
            labelled_arrow(np.zeros(3), pose @ vector * axis_length, SCREEN_AXES, name)
        estimate = pose @ (np.array(frame["down"]) @ screen)
        truth = pose @ np.array(frame["true_down"])
        # The estimate underneath and wider, so true down stays visible once they agree.
        scene.quiver(*down_at, *estimate * down_length, color=ESTIMATE, arrow_length_ratio=ARROW_RATIO,
                     linewidth=ESTIMATE_WIDTH)
        scene.quiver(*down_at, *truth * down_length, color=TRUE_DOWN, arrow_length_ratio=ARROW_RATIO)
        tilt = np.degrees(np.arccos(np.clip(truth @ estimate / np.linalg.norm(estimate), -1, 1)))
        title.set_text(f"t = {frame['t']:4.1f} s    tilt error {tilt:3.1f} deg    "
                       f"bias error {np.hypot(plane[index], along[index]):4.1f} deg/s")
        cursor.set_xdata([frame["t"]] * 2)

    fig.set_dpi(ANIMATION_DPI)
    save_gif(fig, draw, len(data["frames"]), output, GIF_FPS, colors=ORBIT_GIF_COLORS)


def main():
    data, output = probe_data(__file__, "gravity.c", __doc__)
    fig, axis = subplots(1, 1, (7, 3))
    axis.plot(*zip(*data["trust"]), color=SLOTS[0])
    for key, label in (("full_g", "FULL"), ("none_g", "NONE")):
        axis.axvline(data[key], color=SLOTS[1], linestyle=":")
        axis.annotate(label, (data[key], 0.5), xytext=(5, 0), textcoords="offset points")
    axis.set(xlabel="Acceleration magnitude deviation from 1 g (g)", ylabel="Trust weight",
             ylim=(0, 1.1), xlim=(0, None))
    style_axes(axis, "Acceleration trust weight against magnitude deviation")
    save(fig, output / "gravity-trust.png")
    bias_figure(data, output / "gravity-bias.gif")


if __name__ == "__main__":
    main()
