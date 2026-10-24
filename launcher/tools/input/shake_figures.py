"""Plot the shake reader's response; --cc CC --out PNG."""
from input_figures import probe_data
from docs.plot_style import SLOTS, save, style_axes, subplots


def main():
    data, output = probe_data(__file__, "shake.c", __doc__)
    fig, axis = subplots(1, 1, (7, 3))
    axis.plot(*zip(*data["points"]), color=SLOTS[0])
    for name in ("start_mg", "full_mg"):
        mg = data[name]
        axis.axvline(mg, color=SLOTS[1], linestyle=":")
        axis.text(mg, 0.1, name.removesuffix("_mg").upper(), ha="right")
    axis.set(xlabel="Smoothed linear acceleration (mg)", ylabel="Shake level", ylim=(0, 1.1), xlim=(0, None))
    style_axes(axis, "Deliberate shake")
    save(fig, output)


if __name__ == "__main__":
    main()
