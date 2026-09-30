"""Lay renders of two revisions side by side and say how they differ.

    render_compare.py --out sheet.png [--summary summary.txt] [--clear RRGGBB]
        [--gain N] --row LABEL A.bmp B.bmp [--row ...]

One row per render: A | B | a greyscale heatmap of the absolute per-pixel
difference, scaled by --gain so a small colour shift shows. With --clear (the
colour the scene clears to) a pixel that is clear on one side and drawn on the
other is red in the heatmap and counted as a hole on the side that left it
clear; a silhouette that moved counts too, so read the count beside the sheet.
--clear is matched as given and as a 16-bit framebuffer holds it once
expanded to 24 bits. render_compare.sh drives this.

Needs Pillow and numpy.
"""

import argparse
from dataclasses import dataclass

import numpy as np
from PIL import Image

HOLE_RED = (255, 0, 0)


@dataclass
class Stats:
    changed: int
    total: int
    mean_abs: float
    holes_a: int
    holes_b: int


def parse_rgb(text):
    """(r, g, b) from RRGGBB hex."""
    if len(text) != 6:
        raise ValueError("colour must be RRGGBB, got %r" % text)
    return tuple(int(text[i : i + 2], 16) for i in (0, 2, 4))


def expand_565(rgb):
    """The 24-bit colour a 16-bit framebuffer shows for rgb."""
    r, g, b = rgb
    r5, g6, b5 = r >> 3, g >> 2, b >> 3
    return (r5 << 3 | r5 >> 2, g6 << 2 | g6 >> 4, b5 << 3 | b5 >> 2)


def _pixels(picture):
    return np.asarray(picture.convert("RGB")).astype(np.int32)


def _is_clear(pixels, clear):
    hit = np.zeros(pixels.shape[:2], dtype=bool)
    if clear is not None:
        for colour in {clear, expand_565(clear)}:
            hit |= (pixels == colour).all(axis=2)
    return hit


def _holes(a, b, clear):
    clear_a, clear_b = _is_clear(a, clear), _is_clear(b, clear)
    return clear_a & ~clear_b, clear_b & ~clear_a


def _same_size(a, b):
    if a.size != b.size:
        raise ValueError("size %dx%d vs %dx%d" % (*a.size, *b.size))


def measure(a, b, clear):
    """Stats for two same-sized images; clear may be None."""
    _same_size(a, b)
    pa, pb = _pixels(a), _pixels(b)
    diff = np.abs(pa - pb)
    holes_a, holes_b = _holes(pa, pb, clear)
    return Stats(
        changed=int((diff.max(axis=2) > 0).sum()),
        total=diff.shape[0] * diff.shape[1],
        mean_abs=float(diff.mean()),
        holes_a=int(holes_a.sum()),
        holes_b=int(holes_b.sum()),
    )


def heatmap(a, b, clear, gain):
    """Greyscale |a - b| times gain, holes in red."""
    _same_size(a, b)
    pa, pb = _pixels(a), _pixels(b)
    grey = np.clip(np.abs(pa - pb).max(axis=2) * gain, 0, 255).astype(np.uint8)
    heat = np.stack([grey] * 3, axis=2)
    holes_a, holes_b = _holes(pa, pb, clear)
    heat[holes_a | holes_b] = HOLE_RED
    return Image.fromarray(heat)


def sheet(rows, clear, gain):
    """One picture: each (label, a, b) row as A | B | heatmap, stacked."""
    strips = []
    for _label, a, b in rows:
        _same_size(a, b)
        strips.append(np.concatenate([_pixels(a), _pixels(b), np.asarray(heatmap(a, b, clear, gain)).astype(np.int32)], axis=1))
    return Image.fromarray(np.concatenate(strips, axis=0).astype(np.uint8))


def summary_line(label, stats):
    """One line of numbers for a render."""
    return "%s: changed %d/%d (%.2f%%), mean abs diff %.3f/255, holes A %d, holes B %d" % (
        label,
        stats.changed,
        stats.total,
        100.0 * stats.changed / stats.total,
        stats.mean_abs,
        stats.holes_a,
        stats.holes_b,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True)
    parser.add_argument("--summary")
    parser.add_argument("--clear", type=parse_rgb)
    parser.add_argument("--gain", type=int, default=8)
    parser.add_argument("--row", nargs=3, action="append", required=True, metavar=("LABEL", "A", "B"))
    args = parser.parse_args()

    rows = [(label, Image.open(a), Image.open(b)) for label, a, b in args.row]
    sheet(rows, args.clear, args.gain).save(args.out)
    lines = [summary_line(label, measure(a, b, args.clear)) for label, a, b in rows]
    print("\n".join(lines))
    if args.summary:
        with open(args.summary, "a") as handle:
            handle.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
