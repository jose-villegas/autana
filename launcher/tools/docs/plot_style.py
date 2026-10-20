"""The one matplotlib setup of the doc-figure generators: palette, sizes and save options.

Importing this module selects the headless backend. A bar chart added here must start its value axis at 0
and keep its category labels from overlapping (grouped bars or turned labels).
"""

import pathlib

import matplotlib
import numpy as np
from PIL import Image

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
    """Write the PNG (or GIF frame) without the version stamp, so a regeneration diffs clean, then free it."""
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=dpi, metadata={"Software": None})
    plt.close(fig)


GIF_COLORS = 32
GIF_MILLISECONDS_PER_SECOND = 1000


def save_gif(fig, draw_frame, frames, path, fps, colors=GIF_COLORS):
    """Use one adaptive palette for the whole loop to keep static chart colours stable."""
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    images = []
    try:
        fig.set_dpi(min(fig.dpi, DPI))
        fig.set_facecolor("white")
        for index in range(frames):
            draw_frame(index)
            fig.canvas.draw()
            images.append(Image.fromarray(np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy()))
        atlas = Image.new("RGB", (images[0].width, images[0].height * frames))
        for index, image in enumerate(images):
            atlas.paste(image, (0, index * image.height))
        palette = atlas.quantize(colors=colors, dither=Image.Dither.NONE)
        quantized = [image.quantize(palette=palette, dither=Image.Dither.NONE) for image in images]
        quantized[0].save(path, save_all=True, append_images=quantized[1:], loop=0,
                          duration=round(GIF_MILLISECONDS_PER_SECOND / fps), optimize=False,
                          palette=palette.getpalette()[:colors * 3])
    finally:
        plt.close(fig)


GIF_FRAMES = 60
GIF_FPS = 10
ORBIT_GIF_COLORS = 64
WIRE_SAMPLES = 49
WIRE_STRIDE = 8
WIRE_ALPHA = 0.45
WIRE_WIDTH = 0.6


def wire_sphere(axis, center, radius, color=FAINT):
    yaw = np.linspace(0, 2 * np.pi, WIRE_SAMPLES)
    pitch = np.linspace(-np.pi / 2, np.pi / 2, WIRE_SAMPLES)
    x = center[0] + radius * np.outer(np.cos(yaw), np.cos(pitch))
    y = center[1] + radius * np.outer(np.ones_like(yaw), np.sin(pitch))
    z = center[2] + radius * np.outer(np.sin(yaw), np.cos(pitch))
    axis.plot_wireframe(x, y, z, rstride=WIRE_STRIDE, cstride=WIRE_STRIDE,
                        color=color, alpha=WIRE_ALPHA, linewidth=WIRE_WIDTH)
