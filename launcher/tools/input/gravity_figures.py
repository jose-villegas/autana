"""Plot gravity correction trust, and the device rocking while the filter learns a gyro bias; --cc CC --out DIR."""
import numpy as np
from matplotlib.lines import Line2D

from input_figures import probe_data
from device_model import VIEW_AZIMUTH, DeviceAnimation, DeviceModel, legend_handles
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, save, style_axes, subplots

# The camera swings this far either side of its start over one loop, so the last frame meets the first.
VIEW_SWING = 35
TRUE_DOWN = INK
ESTIMATE = SLOTS[1]
ESTIMATE_WIDTH = 3


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
    animation = DeviceAnimation(model)
    inset = animation.plots[0]
    inset.plot(times, predicted, color=MUTED, linestyle="--", label="Linearised loop, across")
    inset.plot(times, plane, color=ESTIMATE, label="Across gravity")
    inset.plot(times, along, color=SLOTS[3], label="Along gravity")
    inset.axvline(data["settle_s"], color=FAINT, linestyle=":")
    inset.set(xlim=(0, times[-1]), ylim=(0, max(plane.max(), along.max()) * 1.1))
    inset.set_xlabel("Time (s)", fontsize=LABEL_SIZE)
    inset.set_ylabel("Bias error (deg/s)", fontsize=LABEL_SIZE)
    inset.legend(fontsize=LABEL_SIZE, loc="upper right", frameon=False)
    style_axes(inset, "Gyro bias error")
    animation.legend([Line2D([], [], color=TRUE_DOWN, label="True down"),
                      Line2D([], [], color=ESTIMATE, label="Filter estimate")] + legend_handles())

    def draw(index):
        frame = data["frames"][index]
        pose = np.array(frame["pose"])
        estimate = pose @ model.to_device(frame["down"])
        truth = pose @ np.array(frame["true_down"])
        tilt = np.degrees(np.arccos(np.clip(truth @ estimate / np.linalg.norm(estimate), -1, 1)))
        swing = VIEW_SWING * np.sin(2 * np.pi * index / len(data["frames"]))
        # The estimate underneath and wider, so true down stays visible once they agree.
        animation.frame(pose, [(animation.beside, estimate * animation.unit, ESTIMATE, ESTIMATE_WIDTH),
                               (animation.beside, truth * animation.unit, TRUE_DOWN, None)], frame["t"],
                        f"t = {frame['t']:4.1f} s    tilt error {tilt:3.1f} deg    "
                        f"bias error {np.hypot(plane[index], along[index]):4.1f} deg/s",
                        azimuth=VIEW_AZIMUTH + swing)

    animation.save(output, len(data["frames"]), draw)


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
