"""Show the accelerometer's six rest windows and fit with scripted chip-axis error."""
import numpy as np
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

from input_figures import probe_data
from device_model import (VIEW_AZIMUTH, WORLD_DOWN, DeviceAnimation, DeviceModel, facing_yaw,
                          legend_handles, pose_from_down)
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, figure, save

AXES = "xyz"
DIRECTIONS = (1, -1)
FLAT_YAW_DEG = 15
ERROR_MAGNIFICATION = 4
TRUE_DOWN, MEASURED = INK, SLOTS[3]
ERROR_COLOUR = SLOTS[4]
TRUE_WIDTH, VECTOR_WIDTH = 5, 2.5
AXIS_COLOURS = (SLOTS[0], SLOTS[1], SLOTS[6])
SETTLING, MEASURING = "#c4d8f1", "#bae6d6"
PLOT_TITLES = ("Screen acceleration", "Offset per axis after its pose pair")
PLOT_LEGEND_ANCHOR = (0.5, 1.01)
TRACE_LEGEND_HANDLE_LENGTH = 1.15
TRACE_LEGEND_COLUMN_SPACING = 1.05
PLOT_HEADROOM = 1.25
SCRIPTED_LINE_ALPHA = 0.60
FITTED_LINE_WIDTH = 2.4
VECTOR_ORIGIN = (0, 0, 0)
FIT_SIZE = (9, 4.2)
FIT_COLUMN_WIDTHS = (3, 2)
NUMBER_LINE_MARGIN = 0.20
NUMBER_LINE_Y_LIMITS = (-0.30, 0.72)
FIT_MARKER_ZORDER = 3
MEAN_LABEL_GAP = -9  # points below the number line
SCALE_ARROW_Y = 0.35
SCALE_LABEL_Y = 0.44
OFFSET_LABEL_Y = -0.19
VALUE_HEADER_Y = 0.82
VALUE_COLUMNS = ((0.43, "fit", "Fitted"), (0.85, "scripted", "Scripted"))
VALUE_ROWS = ((0.54, "Offset", "offset_mg", "+.1f"), (0.27, "Scale", "scale_ppm", ",.0f"))
IDEAL_TICK_HALF_HEIGHT = 0.09
IDEAL_LABEL_GAP = 3  # points above the tick
ROW_LABEL_POSITION = (0.04, 0.5)  # axes coordinates


def pose_matrices(model):
    """Screen +x, -x, +y, -y, +z, -z in turn, with the glass visible."""
    for axis, direction in enumerate(model.screen_axes):
        for sign in DIRECTIONS:
            down = sign * direction
            yaw = facing_yaw(down, VIEW_AZIMUTH) if axis < AXES.index("z") else np.radians(FLAT_YAW_DEG)
            yield from pose_from_down(down, yaw).T.ravel()


def round_figure(data, model, output):
    frames = data["frames"]
    times = np.array([frame["t"] for frame in frames])
    poses = np.array([frame["pose"] for frame in frames])
    acceleration = np.array([frame["acceleration"] for frame in frames])
    measured = (poses @ model.to_device(acceleration)[..., None])[..., 0]
    error = ERROR_MAGNIFICATION * (measured - WORLD_DOWN)

    vector_units = max(np.linalg.norm(WORLD_DOWN), np.linalg.norm([measured, error], axis=-1).max())
    animation = DeviceAnimation(model, centred=True, vector_units=vector_units, plot_titles=PLOT_TITLES)
    animation.legend([Line2D([], [], color=TRUE_DOWN, lw=TRUE_WIDTH, label="True down (1 g)"),
                      Line2D([], [], color=MEASURED, label="Measured acceleration"),
                      Line2D([], [], color=ERROR_COLOUR, label=f"Error (measured − true) ×{ERROR_MAGNIFICATION}")]
                     + legend_handles(chip_axes=False))

    trace, offsets = animation.plots
    for pose in data["poses"]:
        trace.axvspan(pose["settling"], pose["measuring"], color=SETTLING, lw=0)
        trace.axvspan(pose["measuring"], pose["done"], color=MEASURING, lw=0)
    traces = [trace.plot(times, values, color=colour, label=axis)[0]
              for axis, colour, values in zip(AXES, AXIS_COLOURS, acceleration.T)]
    legend_style = dict(loc="lower center", bbox_to_anchor=PLOT_LEGEND_ANCHOR, fontsize=LABEL_SIZE, frameon=False)
    trace_handles = [*traces, Patch(facecolor=SETTLING, label="Settling"), Patch(facecolor=MEASURING, label="Measuring")]
    trace.legend(handles=trace_handles, ncol=len(trace_handles), handlelength=TRACE_LEGEND_HANDLE_LENGTH,
                 columnspacing=TRACE_LEGEND_COLUMN_SPACING, **legend_style)
    trace.set_ylabel("g", fontsize=LABEL_SIZE)
    trace.tick_params(labelbottom=False)

    steps = []
    pair_results = data["poses"][len(DIRECTIONS) - 1::len(DIRECTIONS)]
    for colour, pose, scripted in zip(AXIS_COLOURS, pair_results, data["scripted"]["offset_mg"]):
        offsets.axhline(scripted, color=colour, linestyle="--", alpha=SCRIPTED_LINE_ALPHA)
        line, = offsets.plot([], [], color=colour, linewidth=FITTED_LINE_WIDTH, marker=".", markevery=[0])
        steps.append((line, pose["done"]))
    offset_handles = [Line2D([], [], color=MUTED, label="Fitted"), Line2D([], [], color=MUTED, ls="--", label="Scripted")]
    offsets.legend(handles=offset_handles, ncol=len(offset_handles), **legend_style)
    lim = PLOT_HEADROOM * np.abs([data[source]["offset_mg"] for _, source, _ in VALUE_COLUMNS]).max()
    offsets.set_ylim(-lim, lim)
    offsets.set_ylabel("mg", fontsize=LABEL_SIZE)
    offsets.set_xlabel("Time (s)", fontsize=LABEL_SIZE)
    for axis in animation.plots:
        axis.set_xlim(0, times[-1])

    def draw(index):
        frame = frames[index]
        time = frame["t"]
        for (line, completed), fitted in zip(steps, frame["offset_mg"]):
            if fitted is None:
                line.set_data([], [])
            else:
                line.set_data([completed, time], [fitted, fitted])
        reading = "   ".join(f"{axis} {value:+.3f}" for axis, value in zip(AXES, acceleration[index]))
        pose = frame["pose_number"]
        animation.frame(poses[index], [(VECTOR_ORIGIN, WORLD_DOWN * animation.unit, TRUE_DOWN, TRUE_WIDTH),
                                       (VECTOR_ORIGIN, measured[index] * animation.unit, MEASURED, VECTOR_WIDTH),
                                       (VECTOR_ORIGIN, error[index] * animation.unit, ERROR_COLOUR, VECTOR_WIDTH)],
                        time, f"Pose {pose}/{len(data['poses'])}    •    {reading} g", chip_axes=False)

    fps = 1 / (times[1] - times[0])
    animation.save(output, len(frames), draw, fps=fps)


def fit_figure(data, output):
    fig = figure(FIT_SIZE)
    fig.suptitle("Accelerometer fit from six pose means", color=INK)
    grid = fig.add_gridspec(len(AXES), len(FIT_COLUMN_WIDTHS), width_ratios=FIT_COLUMN_WIDTHS)
    means = np.array([pose["mean"] for pose in data["poses"]])
    extent = np.abs(means).max() + NUMBER_LINE_MARGIN
    for index, (axis, colour) in enumerate(zip(AXES, AXIS_COLOURS)):
        positive, negative = means[index * len(DIRECTIONS): (index + 1) * len(DIRECTIONS), index]
        midpoint = (positive + negative) / 2
        number_line = fig.add_subplot(grid[index, 0])
        number_line.set(xlim=(-extent, extent), ylim=NUMBER_LINE_Y_LIMITS)
        number_line.set_axis_off()
        number_line.axhline(0, color=FAINT)
        for ideal in DIRECTIONS:
            number_line.vlines(ideal, -IDEAL_TICK_HALF_HEIGHT, IDEAL_TICK_HALF_HEIGHT, color=MUTED)
            number_line.annotate(f"{ideal:+d} g", (ideal, IDEAL_TICK_HALF_HEIGHT), (0, IDEAL_LABEL_GAP),
                                 textcoords="offset points", ha="center", va="bottom", fontsize=LABEL_SIZE,
                                 color=MUTED)
        number_line.scatter([negative, positive], [0, 0], color=colour, zorder=FIT_MARKER_ZORDER)
        number_line.scatter(midpoint, 0, color=colour, marker="D", zorder=FIT_MARKER_ZORDER)
        for value in (negative, positive):
            number_line.annotate(f"{value:+.3f} g", (value, 0), (0, MEAN_LABEL_GAP), textcoords="offset points",
                                 ha="center", va="top", fontsize=LABEL_SIZE, color=INK)
        number_line.annotate("", (positive, SCALE_ARROW_Y), (midpoint, SCALE_ARROW_Y),
                             arrowprops={"arrowstyle": "<->", "color": colour, "shrinkA": 0, "shrinkB": 0})
        number_line.text((midpoint + positive) / 2, SCALE_LABEL_Y, "Scale (half-span)", ha="center",
                         fontsize=LABEL_SIZE, color=MUTED)
        number_line.text(midpoint, OFFSET_LABEL_Y, "Offset", ha="center", va="top", fontsize=LABEL_SIZE, color=MUTED)
        number_line.text(*ROW_LABEL_POSITION, axis, transform=number_line.transAxes, ha="left", va="center",
                         fontweight="bold", color=colour)

        values = fig.add_subplot(grid[index, 1])
        values.set_axis_off()
        if index == 0:
            for x, _, heading in VALUE_COLUMNS:
                values.text(x, VALUE_HEADER_Y, heading, ha="right", fontweight="bold", fontsize=LABEL_SIZE, color=INK)
        for y, label, field, format_spec in VALUE_ROWS:
            unit = field.split("_")[1]
            values.text(0, y, label, fontsize=LABEL_SIZE, color=MUTED)
            for x, source, _ in VALUE_COLUMNS:
                value = data[source][field][index]
                values.text(x, y, f"{value:{format_spec}} {unit}", ha="right", fontsize=LABEL_SIZE, color=INK)
    save(fig, output)


def main():
    model = DeviceModel()
    data, output, _ = probe_data(__file__, ("imu_rest.c", "imu_calib.c"), __doc__, *pose_matrices(model))
    round_figure(data, model, output / "motion-calibration-round.gif")
    fit_figure(data, output / "motion-calibration-fit.png")


if __name__ == "__main__":
    main()
