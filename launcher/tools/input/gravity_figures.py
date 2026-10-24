"""Plot gravity correction trust, and the device rocking while the filter learns a gyro bias; --cc CC --out DIR."""
import numpy as np
from matplotlib.lines import Line2D

from input_figures import probe_data
from device_model import ARROW_RATIO, DeviceModel, legend_handles
from docs.plot_style import (FAINT, GIF_FPS, INK, LABEL_SIZE, MUTED, ORBIT_GIF_COLORS, SLOTS, TITLE_SIZE, figure,
                             save, save_gif, style_axes, subplots)

ANIMATION_DPI = 80
FIGURE_SIZE = (10, 5.4)
VIEW_ELEVATION = 22
# The camera swings this far either side of its start over one loop, so the last frame meets the first.
VIEW_AZIMUTH = -60
VIEW_SWING = 35
TRUE_DOWN = INK
ESTIMATE = SLOTS[1]
ESTIMATE_WIDTH = 3
# The down arrows' length and the scene's half extent, as shares of the device height.
DOWN_SHARE = 0.5
EXTENT_SHARE = 0.55
# Figure shares: the scene on the left, the bias error plot on the right, the legend under both.
SCENE = (0, 0.08, 0.62, 0.84)
PLOT = (0.68, 0.2, 0.3, 0.64)
TITLE_Y = 0.94
LEGEND_Y = 0.02
LEGEND_COLUMNS = 4
# The down vectors start beside the device: this share of the device height past its right edge.
DOWN_CLEARANCE_SHARE = 0.1


def bias_errors(data, model):
    """In-plane and along-gravity bias error per frame, in deg/s, in device axes."""
    plane, along = [], []
    for frame in data["frames"]:
        error = model.to_device(np.array(frame["bias"]) - data["bias_dps"])
        down = np.array(frame["true_down"])
        along.append(abs(error @ down))
        plane.append(np.linalg.norm(error - (error @ down) * down))
    return np.array(plane), np.array(along)


def linearised(data, start, times):
    """Bias error of the linearised loop from `start` with no initial tilt error: both poles of s^2 + s/TAU + KI."""
    fast, slow = sorted(np.roots([1, 1 / data["tau_s"], data["ki"]]).real)
    return start * (fast * np.exp(slow * times) - slow * np.exp(fast * times)) / (fast - slow)


def bias_figure(data, model, output):
    times = np.array([frame["t"] for frame in data["frames"]])
    plane, along = bias_errors(data, model)
    predicted = linearised(data, plane[0], times)
    down_length = DOWN_SHARE * model.height
    extent = EXTENT_SHARE * model.height

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
    handles = [Line2D([], [], color=TRUE_DOWN, label="True down"),
               Line2D([], [], color=ESTIMATE, label="Filter estimate")] + legend_handles()
    fig.legend(handles=handles,
               loc="lower center", bbox_to_anchor=(0.5, LEGEND_Y), ncol=LEGEND_COLUMNS, fontsize=LABEL_SIZE,
               frameon=False)
    down_at = np.array([model.width / 2 + DOWN_CLEARANCE_SHARE * model.height, 0, down_length / 2])

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
        model.draw(scene, pose)
        estimate = pose @ model.to_device(frame["down"])
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
    data, output, cc = probe_data(__file__, "gravity.c", __doc__)
    fig, axis = subplots(1, 1, (7, 3))
    axis.plot(*zip(*data["trust"]), color=SLOTS[0])
    for key, label in (("full_g", "FULL"), ("none_g", "NONE")):
        axis.axvline(data[key], color=SLOTS[1], linestyle=":")
        axis.annotate(label, (data[key], 0.5), xytext=(5, 0), textcoords="offset points")
    axis.set(xlabel="Acceleration magnitude deviation from 1 g (g)", ylabel="Trust weight",
             ylim=(0, 1.1), xlim=(0, None))
    style_axes(axis, "Acceleration trust weight against magnitude deviation")
    save(fig, output / "gravity-trust.png")
    bias_figure(data, DeviceModel(cc), output / "gravity-bias.gif")


if __name__ == "__main__":
    main()
