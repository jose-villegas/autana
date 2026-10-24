"""Plot gravity correction trust and stationary bias learning; --cc CC --out DIR."""
from input_figures import probe_data
from docs.plot_style import SLOTS, save, style_axes, subplots


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
    fig, axis = subplots(1, 1, (7, 3))
    axis.plot(*zip(*data["bias"]), color=SLOTS[0], label="Learned bias")
    axis.axhline(data["offset_dps"], color=SLOTS[1], linestyle="--", label="Applied gyro offset")
    axis.axvline(data["settle_s"], color=SLOTS[2], linestyle=":", label="Settling target")
    axis.set(xlabel="Time (s)", ylabel="Gyro bias (deg/s)", ylim=(0, data["offset_dps"] * 1.15),
             xlim=(0, data["bias"][-1][0]))
    axis.legend(loc="lower right")
    style_axes(axis, "Stationary device, full acceleration trust")
    save(fig, output / "gravity-bias.png")


if __name__ == "__main__":
    main()
