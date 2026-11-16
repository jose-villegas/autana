"""The one matplotlib setup of the doc-figure generators: palette, sizes and save options.

Importing this module selects the headless backend. A bar chart added here must start its value axis at 0
and keep its category labels from overlapping (grouped bars or turned labels).
"""

import pathlib
import sys

import matplotlib
import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import figures  # noqa: E402

matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK, MUTED, FAINT = "#0b0b0b", "#52514e", "#c9c7c2"
# One colour per series or bracket, in a fixed order.
SLOTS = ("#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948")
DPI = 110
TITLE_SIZE = 10
LABEL_SIZE = 8


def figure(size, **options):
    """A figure of `size` inches with constrained layout."""
    return plt.figure(figsize=size, layout="constrained", **options)


def subplots(rows, columns, size, **options):
    return plt.subplots(rows, columns, figsize=size, layout="constrained", **options)


def save(fig, path, dpi=DPI):
    """Write the PNG without the version stamp, so a regeneration diffs clean, then free the figure."""
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=dpi, metadata={"Software": None})
    plt.close(fig)


BAR_HEIGHT = 0.6  # of each bar's row
END_LABEL_ROOM = 0.35  # share of the longest bar left free past its end for its label
END_LABEL_GAP = 4  # points between a bar's end and its label


def stacked_barh(axis, rows, colours, end_labels=None, extent=None):
    """Horizontal stacked bars, the first row on top. `rows` is [(label, [(segment, value)])]; each segment takes
    colours[segment] and names itself once for a legend. `end_labels` sit past each bar's end. The value axis starts
    at 0 and spans `extent`, by default the longest bar plus room for its label; the top and right spines are hidden."""
    named, ends = set(), []
    for index, (_, segments) in enumerate(rows):
        left = 0.0
        for segment, value in segments:
            axis.barh(index, value, left=left, color=colours[segment], height=BAR_HEIGHT, edgecolor="white",
                      label=None if segment in named else segment)
            named.add(segment)
            left += value
        ends.append(left)
    for index, (end, text) in enumerate(zip(ends, end_labels or ())):
        axis.annotate(text, (end, index), xytext=(END_LABEL_GAP, 0), textcoords="offset points", va="center",
                      fontsize=LABEL_SIZE, color=INK)
    axis.set_yticks(range(len(rows)), [label for label, _ in rows])
    axis.set_xlim(0, max(ends) * (1 + END_LABEL_ROOM) if extent is None else extent)
    axis.invert_yaxis()
    for side in ("top", "right"):
        axis.spines[side].set_visible(False)


def style_axes(axis, title, *, square=False):
    """A panel's title, ticks and spines in the palette; square keeps one unit the same length on both axes."""
    axis.set_title(title, fontsize=TITLE_SIZE, color=INK)
    if square:
        axis.set_aspect("equal")
    axis.tick_params(labelsize=LABEL_SIZE, colors=MUTED)
    for spine in axis.spines.values():
        spine.set_color(FAINT)


GIF_FRAMES = 60
GIF_FPS = 10
GIF_MILLISECONDS_PER_SECOND = 1000
WIRE_SAMPLES = 49
WIRE_STRIDE = 8
WIRE_ALPHA = 0.45
WIRE_WIDTH = 0.6


def save_gif(fig, draw_frame, frames, path, fps):
    """Draw `frames` frames of the figure on white and write them as a looping GIF on the global palette."""
    images = []
    try:
        fig.set_dpi(min(fig.dpi, DPI))
        fig.set_facecolor("white")
        for index in range(frames):
            draw_frame(index)
            fig.canvas.draw()
            images.append(np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy())
    finally:
        plt.close(fig)
    figures.save_gif(images, path, GIF_MILLISECONDS_PER_SECOND / fps)


DARK_BACKGROUND = "#0d1117"
DARK_INK = "#e6edf3"
DARK_MUTED = "#9da7b3"
DARK_FAINT = "#30363d"


def picture(stem, description, directory="../images"):
    from html import escape
    source = escape(f"{directory}/{stem}", quote=True)
    description = escape(description, quote=True)
    return (f'<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="{source}-dark.png">\n'
            f'  <source media="(prefers-color-scheme: light)" srcset="{source}.png">\n'
            f'  <img alt="{description}" src="{source}.png">\n</picture>\n\n')


def save_themes(fig, path, dpi=DPI):
    """Save both page themes; the unsuffixed image is the light fallback."""
    from matplotlib.text import Text
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=dpi, metadata={"Software": None})
    fig.set_facecolor(DARK_BACKGROUND)
    for axis in fig.axes:
        axis.set_facecolor(DARK_BACKGROUND)
        axis.tick_params(colors=DARK_MUTED)
        for spine in axis.spines.values():
            spine.set_color(DARK_FAINT)
        for line in (*axis.get_xgridlines(), *axis.get_ygridlines()):
            line.set_color(DARK_FAINT)
        legend = axis.get_legend()
        if legend:
            legend.get_frame().set(facecolor=DARK_BACKGROUND, edgecolor=DARK_FAINT)
    for text in fig.findobj(Text):
        text.set_color(DARK_INK)
    save(fig, path.with_stem(path.stem + "-dark"), dpi)


FIGURE_SIZE = (8, 4.5)
LEGEND_ANCHOR = (0.5, -0.18)
LEGEND_COLUMNS = 2


def save_plot(fig, axis, path, title):
    """A one-theme plot with its legend, styled and saved."""
    axis.legend()
    style_axes(axis, title)
    save(fig, path)


def save_themed_plot(fig, axis, path, title, *, legend_columns=LEGEND_COLUMNS):
    style_axes(axis, title)
    axis.grid(color=FAINT)
    axis.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR, ncol=legend_columns)
    save_themes(fig, path)


POINT_LABEL_GAP = 3  # points between a labelled dot and its label, the nearest ring tried
POINT_LABEL_RINGS = 4  # rings of candidate places, each POINT_LABEL_GAP further out
POINT_LABEL_MARKER = 3  # half the side, in pixels, of the box a labelled dot keeps free


def place_labels(axis, points, texts, color=INK):
    """Label each (x, y) data point with its text, in order, at the nearest of eight places per ring around it whose
    box overlaps no earlier label, no labelled dot and nothing outside the axes; a label past the first ring draws a
    leader line to its dot. Call after the axes' limits are final."""
    fig = axis.figure
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    frame = axis.get_window_extent(renderer)
    side = 2 * POINT_LABEL_MARKER
    taken = [matplotlib.transforms.Bbox.from_bounds(x - POINT_LABEL_MARKER, y - POINT_LABEL_MARKER, side, side)
             for x, y in axis.transData.transform(points)]
    places = [(ring, dx, dy) for ring in range(1, POINT_LABEL_RINGS + 1)
              for dx, dy in ((1, 1), (1, -1), (-1, 1), (-1, -1), (1, 0), (-1, 0), (0, 1), (0, -1))]

    def label(point, text, ring, dx, dy, arrow=None):
        return axis.annotate(text, point, xytext=(dx * ring * POINT_LABEL_GAP, dy * ring * POINT_LABEL_GAP),
                             textcoords="offset points", fontsize=LABEL_SIZE, color=color, arrowprops=arrow,
                             ha={1: "left", -1: "right", 0: "center"}[dx], va={1: "bottom", -1: "top", 0: "center"}[dy])

    for point, text in zip(points, texts):
        # No free place: the nearest one, overlapping.
        chosen = places[0]
        for place in places:
            trial = label(point, text, *place)
            box = trial.get_window_extent(renderer)
            trial.remove()
            if (frame.contains(box.x0, box.y0) and frame.contains(box.x1, box.y1)
                    and not any(box.overlaps(other) for other in taken)):
                chosen = place
                break
        arrow = dict(arrowstyle="-", color=MUTED, linewidth=0.5, shrinkA=0, shrinkB=2) if chosen[0] > 1 else None
        placed = label(point, text, *chosen, arrow)
        placed.set_in_layout(False)
        taken.append(placed.get_window_extent(renderer))


def wire_sphere(axis, center, radius, color=FAINT):
    yaw = np.linspace(0, 2 * np.pi, WIRE_SAMPLES)
    pitch = np.linspace(-np.pi / 2, np.pi / 2, WIRE_SAMPLES)
    x = center[0] + radius * np.outer(np.cos(yaw), np.cos(pitch))
    y = center[1] + radius * np.outer(np.ones_like(yaw), np.sin(pitch))
    z = center[2] + radius * np.outer(np.sin(yaw), np.cos(pitch))
    axis.plot_wireframe(x, y, z, rstride=WIRE_STRIDE, cstride=WIRE_STRIDE,
                        color=color, alpha=WIRE_ALPHA, linewidth=WIRE_WIDTH)
