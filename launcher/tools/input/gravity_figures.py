"""Plot gravity readers, motion rejection and gyro bias learning; --cc CC --out DIR."""
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
ACCELERATION = SLOTS[6]
LINEAR = SLOTS[2]
PLOT_HEADROOM = 1.15
FALL_ALPHA = 0.3


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


def scenario_animation(model, times, titles):
    """Two zero-based signal plots sharing the animation's time cursors."""
    animation = DeviceAnimation(model, plots=2)
    for plot, title in zip(animation.plots, titles):
        plot.set_xlim(0, times[-1])
        style_axes(plot, title)
    animation.plots[-1].set_xlabel("Time (s)", fontsize=LABEL_SIZE)
    return animation


def reader_figure(data, model, output):
    """The reader's full down and glass-plane vectors, suppressed during free fall."""
    frames = data["reader"]
    times = np.array([frame["t"] for frame in frames])
    strength = np.array([frame["strength"] for frame in frames])
    magnitude = np.array([np.linalg.norm(np.array(frame["linear"]) + frame["down"]) for frame in frames])
    animation = scenario_animation(model, times, ("In-plane strength", "|linear + down| (g)"))
    for plot, values, threshold, label in zip(animation.plots, (strength, magnitude),
                                              (data["min_direction"], data["free_fall_g"]),
                                              ("MIN_DIRECTION", "FREE_FALL")):
        plot.plot(times, values, color=ESTIMATE)
        plot.axhline(threshold, color=MUTED, linestyle=":", label=label)
        plot.axvspan(*data["fall_span"], color=FAINT, alpha=FALL_ALPHA)
        plot.set_ylim(0, max(1, values.max()) * PLOT_HEADROOM)
        plot.legend(fontsize=LABEL_SIZE, loc="lower left", frameon=False)
    animation.legend([Line2D([], [], color=ESTIMATE, label="input_gravity()"),
                      Line2D([], [], color=LINEAR, label="In-plane gravity")] + legend_handles()[:1])

    def draw(index):
        frame = frames[index]
        pose = np.array(frame["pose"])
        vectors = []
        if not frame["free_fall"]:
            down = np.array(frame["down"])
            plane = down.copy()
            plane[2] = 0
            vectors = [(animation.beside, pose @ model.to_device(down) * animation.unit,
                        ESTIMATE, ESTIMATE_WIDTH),
                       (np.zeros(3), pose @ model.to_device(plane) * animation.unit, LINEAR, ESTIMATE_WIDTH)]
        swing = VIEW_SWING * np.sin(2 * np.pi * index / len(frames))
        state = "    free fall" if frame["free_fall"] else ""
        animation.frame(pose, vectors, frame["t"],
                        f"t = {frame['t']:4.1f} s    strength {frame['strength']:.2f}{state}",
                        azimuth=VIEW_AZIMUTH + swing, screen_axes=False)

    animation.save(output, len(frames), draw)


def angle_from_down(vectors, truth):
    """Angular separation in degrees, with both vector magnitudes removed."""
    cosine = np.sum(vectors * truth, axis=1) / (np.linalg.norm(vectors, axis=1) * np.linalg.norm(truth, axis=1))
    return np.degrees(np.arccos(np.clip(cosine, -1, 1)))


def motion_figure(data, model, output):
    """True down, raw acceleration and the gyro-led estimate while the device is carried."""
    frames = data["motion"]
    times = np.array([frame["t"] for frame in frames])
    truth = np.array([frame["true_down"] for frame in frames])
    acceleration = np.array([frame["acceleration"] for frame in frames])
    estimate = np.array([model.to_device(frame["down"]) for frame in frames])
    raw_angle = angle_from_down(acceleration, truth)
    estimate_angle = angle_from_down(estimate, truth)
    deviation = abs(np.linalg.norm(acceleration, axis=1) - 1)
    animation = scenario_animation(model, times, ("Magnitude deviation from 1 g (g)", "Angle from true down (deg)"))
    top, bottom = animation.plots
    top.plot(times, deviation, color=ACCELERATION)
    for key, label in (("full_g", "FULL"), ("none_g", "NONE")):
        top.axhline(data[key], color=MUTED, linestyle=":" if key == "full_g" else "--", label=label)
    top.set_ylim(0, max(data["none_g"], deviation.max()) * PLOT_HEADROOM)
    top.legend(fontsize=LABEL_SIZE, loc="center right", frameon=False)
    bottom.plot(times, raw_angle, color=ACCELERATION)
    bottom.plot(times, estimate_angle, color=ESTIMATE)
    bottom.axhline(data["reject_deg"], color=MUTED, linestyle=":", label="REJECT")
    bottom.set_ylim(0, max(data["reject_deg"], raw_angle.max(), estimate_angle.max()) * PLOT_HEADROOM)
    bottom.legend(fontsize=LABEL_SIZE, loc="upper right", frameon=False)
    animation.legend([Line2D([], [], color=TRUE_DOWN, label="True down"),
                      Line2D([], [], color=ACCELERATION, label="Raw acceleration"),
                      Line2D([], [], color=ESTIMATE, label="Filter estimate"),
                      Line2D([], [], color=LINEAR, label="Linear acceleration")] + legend_handles()[:1])

    def draw(index):
        frame = frames[index]
        pose = np.array(frame["pose"])
        down = pose @ truth[index] * animation.unit
        raw = pose @ acceleration[index] * animation.unit
        filtered = pose @ estimate[index] * animation.unit
        swing = VIEW_SWING * np.sin(2 * np.pi * index / len(frames))
        animation.frame(pose, [(animation.beside, filtered, ESTIMATE, ESTIMATE_WIDTH),
                               (animation.beside, down, TRUE_DOWN, None),
                               (animation.beside, raw, ACCELERATION, None),
                               (animation.beside + down, raw - down, LINEAR, None)], frame["t"],
                        f"t = {frame['t']:4.1f} s    acceleration error {raw_angle[index]:4.1f} deg    "
                        f"estimate error {estimate_angle[index]:3.1f} deg",
                        azimuth=VIEW_AZIMUTH + swing, screen_axes=False)

    animation.save(output, len(frames), draw)


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
    model = DeviceModel()
    bias_figure(data, model, output / "gravity-bias.gif")
    reader_figure(data, model, output / "gravity-reader.gif")
    motion_figure(data, model, output / "gravity-motion.gif")


if __name__ == "__main__":
    main()
