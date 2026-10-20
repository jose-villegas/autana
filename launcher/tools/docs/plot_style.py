"""The one matplotlib setup of the doc-figure generators: palette, sizes, save options and the bar chart.

Importing this module selects the headless backend, so a generator imports it where it draws
(from docs.plot_style import ...) and the report scripts that only print tables never need matplotlib.
"""

import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK, MUTED, FAINT = "#0b0b0b", "#52514e", "#c9c7c2"
# One colour per series or bracket, in a fixed order.
SLOTS = ("#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948")
DPI = 110
TITLE_SIZE = 10
LABEL_SIZE = 8
BAR_GROUP_WIDTH = 0.8  # share of the space between category ticks the bars of one category fill
LABEL_GAP = 0.9  # a category label wider than this share of its tick spacing turns the labels
LABEL_ANGLE = 30  # degrees, the turned labels


def figure(size, **options):
    """A figure of `size` inches with constrained layout."""
    return plt.figure(figsize=size, layout="constrained", **options)


def subplots(rows, columns, size, **options):
    return plt.subplots(rows, columns, figsize=size, layout="constrained", **options)


def save(fig, path, dpi=DPI):
    """Write the PNG (or GIF frame) without the version stamp, so a regeneration diffs clean, then free it."""
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=dpi, metadata={"Software": None})
    plt.close(fig)


def bars(axis, categories, series, ylabel=None):
    """Grouped vertical bars: one group per category, one bar per series name in `series`.

    The value axis starts at 0, so a bar's height is its value. The category labels turn
    when the longest would run into its neighbour, so none overlap.
    """
    width = BAR_GROUP_WIDTH / len(series)
    for index, (name, values) in enumerate(series.items()):
        offsets = [position + (index - (len(series) - 1) / 2) * width for position in range(len(categories))]
        axis.bar(offsets, values, width, label=name, color=SLOTS[index % len(SLOTS)])
    axis.set_xticks(range(len(categories)), categories)
    axis.set_ylim(bottom=0)
    if ylabel:
        axis.set_ylabel(ylabel)
    axis.figure.canvas.draw()
    spacing = axis.transData.transform((1, 0))[0] - axis.transData.transform((0, 0))[0]
    widest = max(label.get_window_extent().width for label in axis.get_xticklabels())
    if widest > LABEL_GAP * spacing:
        axis.tick_params(axis="x", labelrotation=LABEL_ANGLE)
        for label in axis.get_xticklabels():
            label.set_horizontalalignment("right")
