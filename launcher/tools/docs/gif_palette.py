"""Fit the global GIF palette to the doc GIFs and print it as figures.GIF_PALETTE's source.

    python launcher/tools/docs/gif_palette.py [GIF ...]   # default: every GIF under docs/

The theme colours (white, black and plot_style's ink, series slots and dark theme) are pinned; the
other entries are k-means centres in OKLab over a fixed number of colour samples per GIF, each
colour drawn with weight count**WEIGHT_POWER so large flat areas stay exact and rare hues still
count. Every centre is replaced by the sampled colour nearest it, so each entry is a real sRGB
colour. The fit is seeded, so the same GIFs give the same table. Changing the table changes every
doc GIF; run it only to choose a new palette, then paste its output over GIF_PALETTE.
"""
import pathlib
import sys

import numpy as np
from PIL import Image, ImageSequence

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from docs import plot_style  # noqa: E402
from docs.figures import GIF_COLOURS, ROOT, nearest, oklab, packed_colours, unpacked_colours  # noqa: E402

SAMPLES_PER_GIF = 20_000
WEIGHT_POWER = 0.75
ROUNDS = 30
SEED = 1
PER_LINE = 8


def pinned():
    """The theme colours, in a fixed order, each once."""
    names = ("#ffffff", "#000000", plot_style.INK, plot_style.MUTED, plot_style.FAINT, *plot_style.SLOTS,
             plot_style.DARK_BACKGROUND, plot_style.DARK_INK, plot_style.DARK_MUTED, plot_style.DARK_FAINT)
    return np.array(list(dict.fromkeys(tuple(int(name[at:at + 2], 16) for at in (1, 3, 5)) for name in names)),
                    np.uint8)


def samples(gifs, rng):
    """SAMPLES_PER_GIF colours of each GIF, drawn by count**WEIGHT_POWER."""
    drawn = []
    for gif in gifs:
        pixels = np.concatenate([np.asarray(frame.convert("RGB")).reshape(-1, 3)
                                 for frame in ImageSequence.Iterator(Image.open(gif))])
        unique, counts = np.unique(packed_colours(pixels), return_counts=True)
        weight = counts ** WEIGHT_POWER
        drawn.append(unpacked_colours(unique[rng.choice(len(unique), SAMPLES_PER_GIF, p=weight / weight.sum())]))
    return np.concatenate(drawn)


def fit(gifs):
    """GIF_COLOURS colours: the pinned theme, then centres fitted to the GIFs' colours."""
    rng = np.random.default_rng(SEED)
    colours = samples(gifs, rng)
    points, fixed = oklab(colours), oklab(pinned())
    free = GIF_COLOURS - len(fixed)
    centres = points[rng.choice(len(points), free, replace=False)]
    for _ in range(ROUNDS):
        label = nearest(points, np.concatenate([fixed, centres])) - len(fixed)
        for index in range(free):
            members = points[label == index]
            centres[index] = members.mean(0) if len(members) else points[rng.integers(len(points))]
    return np.concatenate([pinned(), colours[nearest(centres, points)]])


def source(palette):
    lines = ["GIF_PALETTE = ("]
    for start in range(0, len(palette), PER_LINE):
        lines.append("    " + " ".join(f'"#{r:02x}{g:02x}{b:02x}",' for r, g, b in palette[start:start + PER_LINE]))
    return "\n".join(lines + [")"])


def main(argv):
    gifs = [pathlib.Path(name) for name in argv] or sorted((ROOT / "docs").rglob("*.gif"))
    print(source(fit(gifs)))


if __name__ == "__main__":
    main(sys.argv[1:])
