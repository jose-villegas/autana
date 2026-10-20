"""The one matplotlib setup of the doc-figure generators: palette, sizes and save options.

Importing this module selects the headless backend. A bar chart added here must start its value axis at 0
and keep its category labels from overlapping (grouped bars or turned labels).
"""

import io
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from PIL import Image

INK, MUTED, FAINT = "#0b0b0b", "#52514e", "#c9c7c2"
# One colour per series or bracket, in a fixed order.
SLOTS = ("#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948")
DPI = 110
TITLE_SIZE = 10
LABEL_SIZE = 8
# An animated figure's palette: enough for the lines and their antialiasing, small enough to keep the GIF light.
GIF_COLOURS = 64


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


def style_axes(axis, title, *, square=False):
    """A panel's title, ticks and spines in the palette; square keeps one unit the same length on both axes."""
    axis.set_title(title, fontsize=TITLE_SIZE, color=INK)
    if square:
        axis.set_aspect("equal")
    axis.tick_params(labelsize=LABEL_SIZE, colors=MUTED)
    for spine in axis.spines.values():
        spine.set_color(FAINT)


def save_gif(figures, path, frame_ms, dpi=DPI):
    """Write each figure (any iterable, so a generator keeps one open at a time) as one frame of a looping GIF,
    frame_ms apart, freeing each as it goes."""
    frames = []
    for fig in figures:
        buffer = io.BytesIO()
        fig.savefig(buffer, dpi=dpi, format="png")
        plt.close(fig)
        frames.append(Image.open(buffer).convert("RGB").quantize(colors=GIF_COLOURS))
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=frame_ms, loop=0, optimize=True)

