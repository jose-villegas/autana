"""Lay renders of two revisions side by side and say how they differ.

    render_compare.py --out sheet.png --label-a TEXT --label-b TEXT
        [--summary summary.txt] [--clear RRGGBB] --row LABEL A.bmp B.bmp [--row ...]
    render_compare.py --out video.mp4 --video A.avi B.avi --label-a TEXT
        --label-b TEXT [--csv frames.csv] [--fps N] [--clear RRGGBB]

Either form takes --crops N: a second picture, <out>.crops.png, of
the N places the two differ most, each cut with a margin, A above B and
enlarged ZOOM (4) times without smoothing. Places with holes come first,
then the strongest; changed pixels near each other are one place. A video
takes them from its two most different frames.

One row per render: A | B | a greyscale heatmap of the absolute per-pixel
difference, scaled by GAIN (8) so a small colour shift shows. Every panel of a
sheet, crops sheet, reference sheet or video sits under a LABEL_BAR naming it:
--label-a and --label-b for A and B ("difference xN", "dE76" for the
others), and a still without them is refused. With --reference-video,
--label-a names the render and the other panel is "reference". With --clear (the
colour the scene clears to) a pixel that is clear on one side and drawn on the
other is red in the heatmap and counted as a hole on the side that left it
clear; a silhouette that moved counts too, so read the count beside the sheet.
--clear is matched as given and as a 16-bit framebuffer holds it once
expanded to 24 bits. render_compare.sh drives this.

--video takes the two AVIs a renderer's own --video wrote over the same
frames and writes one H.264 .mp4 of A | B | heatmap per frame, each panel
labelled, plus with --csv one line of numbers per frame. The AVIs come from
the renderer's own encoder; ffmpeg only packs the composed frames, as
render_doc_images.sh does for its GIFs. --fps defaults to the AVIs' rate.

Needs Pillow and numpy, and ffmpeg for --video.
"""

import argparse
import pathlib
import shutil
import subprocess
from dataclasses import dataclass

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import check_avi

HOLE_RED = (255, 0, 0)
LABEL_BAR = 22
GAIN = 8
ZOOM = 4
CROP_FRAMES = 2
CSV_HEADER = "frame,time_ms,changed_pct,mean_abs,holes_a,holes_b"
# The colour space every ΔE here is measured in: gamma-2.2 RGB, linear sRGB
# primaries to XYZ, and the D65 white CIELAB is relative to.
GAMMA = 2.2
SRGB_TO_XYZ = ((0.4124564, 0.3575761, 0.1804375), (0.2126729, 0.7151522, 0.0721750), (0.0193339, 0.1191920, 0.9503041))
D65_WHITE = (0.95047, 1.0, 1.08883)


@dataclass
class ReferenceStats:
    """Perceptual error of a host render against an offline reference."""

    mean_delta_e: float
    p95_delta_e: float
    ssim_luma: float
    edge_delta_e: float = 0.0
    interior_delta_e: float = 0.0
    edge_share: float = 0.0


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
    """The 24-bit colour a 16-bit framebuffer shows for rgb: a tuple of three
    values, or an integer array with the channels last."""
    if isinstance(rgb, tuple):
        return tuple(int(value) for value in expand_565(np.array(rgb)))
    r5, g6, b5 = rgb[..., 0] >> 3, rgb[..., 1] >> 2, rgb[..., 2] >> 3
    return np.stack([r5 << 3 | r5 >> 2, g6 << 2 | g6 >> 4, b5 << 3 | b5 >> 2], axis=-1)


def _pixels(picture):
    return np.asarray(picture.convert("RGB")).astype(np.int32)


def _linear_rgb(picture):
    rgb = np.asarray(picture.convert("RGB"), dtype=float) / 255.0
    return rgb**GAMMA


def _lab(picture):
    """CIELAB (D65) from this project's gamma-encoded RGB images."""
    rgb = _linear_rgb(picture)
    xyz = rgb @ np.array(SRGB_TO_XYZ).T
    scaled = xyz / np.array(D65_WHITE)
    delta = 6 / 29
    f = np.where(scaled > delta**3, np.cbrt(scaled), scaled / (3 * delta**2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], axis=2)


def delta_e76(a, b):
    """Per-pixel CIE76 ΔE for two equal-sized RGB pictures."""
    _same_size(a, b)
    return np.linalg.norm(_lab(a) - _lab(b), axis=2)


SSIM_WINDOW = 8


def _box_mean(values, size):
    """The mean over every size x size window of a 2-D array, by cumulative sums."""
    total = np.pad(values.cumsum(axis=0).cumsum(axis=1), ((1, 0), (1, 0)))
    window = total[size:, size:] - total[:-size, size:] - total[size:, :-size] + total[:-size, :-size]
    return window / (size * size)


def luma_ssim(a, b):
    """Structural similarity of the luma of two pictures: the mean over every
    8 x 8 window (the whole picture when it is smaller) of the usual SSIM
    map, on Rec.709 luma of the 0..1 encoded colours."""
    _same_size(a, b)
    weights = np.array([0.2126, 0.7152, 0.0722])
    la = np.asarray(a.convert("RGB"), dtype=float) / 255.0 @ weights
    lb = np.asarray(b.convert("RGB"), dtype=float) / 255.0 @ weights
    size = min(SSIM_WINDOW, *la.shape)
    ma, mb = _box_mean(la, size), _box_mean(lb, size)
    va, vb = _box_mean(la * la, size) - ma * ma, _box_mean(lb * lb, size) - mb * mb
    cov = _box_mean(la * lb, size) - ma * mb
    c1, c2 = 0.01**2, 0.03**2
    return float(np.mean((2 * ma * mb + c1) * (2 * cov + c2) / ((ma * ma + mb * mb + c1) * (va + vb + c2))))


EDGE_GRADIENT = 0.06
HEAT_FULL_SCALE = 50


def edge_mask(reference):
    """Pixels on or beside a sharp change of the reference's luma: a lit or
    shadowed boundary, or a silhouette. The step is in gamma-encoded luma per
    pixel, and the mask grows one pixel each way."""
    luma = np.asarray(reference.convert("L"), dtype=float) / 255.0
    if min(luma.shape) < 2:
        return np.zeros(luma.shape, dtype=bool)
    gy, gx = np.gradient(luma)
    edge = np.pad(np.hypot(gx, gy) > EDGE_GRADIENT, 1)
    grown = np.zeros(luma.shape, dtype=bool)
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            grown |= edge[dy : dy + luma.shape[0], dx : dx + luma.shape[1]]
    return grown


def reference_measure(render, reference):
    """Mean and p95 CIE76 ΔE, luma SSIM, and the ΔE on edge and interior
    pixels of the reference, for one reference pair."""
    error = delta_e76(render, reference)
    edge = edge_mask(reference)
    on_edge = float(error[edge].mean()) if edge.any() else 0.0
    inside = float(error[~edge].mean()) if (~edge).any() else 0.0
    share = float(error[edge].sum() / error.sum()) if error.sum() > 0 else 0.0
    return ReferenceStats(float(error.mean()), float(np.percentile(error, 95)), luma_ssim(render, reference), on_edge, inside,
                          share)


def edge_overlay(reference):
    """The reference dimmed, with its edge pixels (see edge_mask) in magenta."""
    pixels = np.asarray(reference.convert("RGB"), dtype=float) * 0.5
    pixels[edge_mask(reference)] = (255, 0, 255)
    return Image.fromarray(pixels.astype(np.uint8))


def heat_scale(width):
    """A strip of the reference heatmap's colours from 0 to HEAT_FULL_SCALE dE, with ticks and a caption."""
    strip = np.tile(np.arange(width) / (width - 1) * HEAT_FULL_SCALE, (14, 1))
    picture = Image.new("RGB", (width, 70), (24, 24, 24))
    picture.paste(reference_heatmap_from_error(strip), (0, 0))
    draw, font = ImageDraw.Draw(picture), _font()
    for tick in range(0, HEAT_FULL_SCALE + 1, 10):
        x = round(tick / HEAT_FULL_SCALE * (width - 1))
        draw.line([(x, 14), (x, 19)], fill=(230, 230, 230))
        outlined_text(draw, (min(x, width - 24), 20), str(tick), (230, 230, 230), font)
    outlined_text(draw, (0, 42), "CIE76 dE per pixel: black matches, red about 20, yellow 50 or more", (230, 230, 230), font)
    return picture


def reference_sheet(items, label, tile=0.6):
    """Each (row title, render, reference) as reference | render | dE heatmap |
    edge pixels, shrunk by `tile`, each under its label (`label` names the
    render), with the heatmap's colour scale below."""
    rows = [(title, reference, render) for title, render, reference in items]

    def others(reference, render):
        return [("dE76", reference_heatmap(render, reference)), ("reference edges", edge_overlay(reference))]

    picture = _compose(list(zip(rows, _panel_rows(rows, None, GAIN, others))), "reference", label, tile)
    out = Image.new("RGB", (picture.width, picture.height + 70))
    out.paste(picture, (0, 0))
    out.paste(heat_scale(picture.width), (0, picture.height))
    return out


def reference_heatmap(render, reference):
    """A perceptual-error heatmap: black is zero, then red, then yellow at 50."""
    return reference_heatmap_from_error(delta_e76(render, reference))


def reference_heatmap_from_error(error):
    """The heatmap's colours for a per-pixel ΔE array."""
    error = np.clip(error, 0, HEAT_FULL_SCALE) / HEAT_FULL_SCALE
    red = np.clip(2 * error, 0, 1)
    green = np.clip(2 * error - 1, 0, 1)
    return Image.fromarray(np.round(np.stack([red, green, np.zeros_like(red)], axis=2) * 255).astype(np.uint8))


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


def heatmap(a, b, clear, gain=GAIN):
    """Greyscale |a - b| times gain, holes in red."""
    _same_size(a, b)
    pa, pb = _pixels(a), _pixels(b)
    grey = np.clip(np.abs(pa - pb).max(axis=2) * gain, 0, 255).astype(np.uint8)
    heat = np.stack([grey] * 3, axis=2)
    holes_a, holes_b = _holes(pa, pb, clear)
    heat[holes_a | holes_b] = HOLE_RED
    return Image.fromarray(heat)


def _panel_rows(rows, clear, gain, panels):
    """Per (title, a, b) row, [(label, picture)] for the panels after B: the
    heatmap unless panels(a, b) names others."""
    out = []
    for _title, a, b in rows:
        _same_size(a, b)
        out.append([("difference x%d" % gain, heatmap(a, b, clear, gain))] if panels is None else panels(a, b))
    return out


def _compose(rows, label_a, label_b, tile=1.0):
    """The panel rows as A | B | the rest, each panel under its label bar,
    stacked, black-padded to the widest. Pictures are shrunk by tile before
    they are labelled so the text stays full size."""
    if not label_a or not label_b:
        raise ValueError("a sheet needs a label for each of A and B")
    strips = []
    for (_title, a, b), after in rows:
        named = [(label_a, a), (label_b, b), *after]
        pictures = [captioned(_scaled(picture, tile), text) for text, picture in named]
        strips.append(np.concatenate([_pixels(picture) for picture in pictures], axis=1))
    widest = max(strip.shape[1] for strip in strips)
    padded = [np.pad(strip, ((0, 0), (0, widest - strip.shape[1]), (0, 0))) for strip in strips]
    return Image.fromarray(np.concatenate(padded, axis=0).astype(np.uint8))


def _scaled(picture, tile):
    if tile == 1.0:
        return picture
    return picture.resize((round(picture.width * tile), round(picture.height * tile)), Image.Resampling.BOX)


def sheet(rows, clear, label_a, label_b, gain=GAIN):
    """One picture: each (title, a, b) row as A | B | heatmap, each panel under
    a label bar, stacked, black-padded to the widest."""
    return _compose(list(zip(rows, _panel_rows(rows, clear, gain, None))), label_a, label_b)


@dataclass
class Cluster:
    """A group of changed pixels: its rect (x1, y1 exclusive), holes, size and strength."""

    x0: int
    y0: int
    x1: int
    y1: int
    holes: int
    size: int
    strength: int


def _grow(mask, radius):
    """mask with every set pixel widened by radius in each direction."""
    grown = mask
    for axis in (0, 1):
        widened = grown.copy()
        for step in range(1, radius + 1):
            lo, hi = [slice(None)] * 2, [slice(None)] * 2
            lo[axis], hi[axis] = slice(step, None), slice(None, -step)
            widened[tuple(lo)] |= grown[tuple(hi)]
            widened[tuple(hi)] |= grown[tuple(lo)]
        grown = widened
    return grown


def _root(parent, node):
    while parent[node] != node:
        parent[node] = parent[parent[node]]
        node = parent[node]
    return node


def _row_runs(row):
    """[start, end) of each run of set pixels in one mask row."""
    edges = np.diff(np.concatenate(([0], row.astype(np.int8), [0])))
    return list(zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)))


def _components(mask):
    """8-connected labels of mask (0 is background), by joining runs row to row."""
    labels = np.zeros(mask.shape, dtype=np.int32)
    parent = [0]
    previous = []
    for y in range(mask.shape[0]):
        current = []
        above = 0
        for start, end in _row_runs(mask[y]):
            while above < len(previous) and previous[above][1] < start:
                above += 1
            touching, scan = [], above
            while scan < len(previous) and previous[scan][0] <= end:
                touching.append(previous[scan][2])
                scan += 1
            if touching:
                label = _root(parent, touching[0])
                for other in touching[1:]:
                    parent[_root(parent, other)] = label
            else:
                label = len(parent)
                parent.append(label)
            labels[y, start:end] = label
            current.append((start, end, label))
        previous = current
    roots = np.array([_root(parent, node) for node in range(len(parent))], dtype=np.int32)
    return roots[labels]


def _busiest_window(weight, wide, high):
    """Top-left (x, y) of the wide x high window of weight with the largest sum."""
    integral = np.pad(weight.cumsum(axis=0).cumsum(axis=1), ((1, 0), (1, 0)))
    sums = integral[high:, wide:] - integral[:-high, wide:] - integral[high:, :-wide] + integral[:-high, :-wide]
    y, x = np.unravel_index(int(sums.argmax()), sums.shape)
    return int(x), int(y)


def _rect(xs, ys, weight, margin, max_side):
    """(x0, y0, x1, y1) around the pixels xs, ys, plus margin, clamped to weight's image.

    A cluster wider or taller than max_side is cut to the max_side window of
    it that holds the most weight, so one picture-wide cluster still gives a
    crop that can be looked at.
    """
    height, width = weight.shape
    x0, x1, y0, y1 = int(xs.min()), int(xs.max()) + 1, int(ys.min()), int(ys.max()) + 1
    if x1 - x0 > max_side or y1 - y0 > max_side:
        wide, high = min(x1 - x0, max_side), min(y1 - y0, max_side)
        x0, y0 = _busiest_window(weight, wide, high)
        x1, y1 = x0 + wide, y0 + high
    return max(0, x0 - margin), max(0, y0 - margin), min(width, x1 + margin), min(height, y1 + margin)


def find_clusters(a, b, clear, count=4, margin=8, threshold=8, grow=3, max_side=64):
    """The count most telling clusters of changed pixels between a and b.

    A pixel changed if any channel moved by more than threshold, or it is a
    hole (clear on one side only). Changed pixels within grow of each other
    are one cluster. Clusters with holes come first, the most holes first,
    then the rest by total difference. Each rect is the changed pixels'
    bounding box plus margin, clamped to the image; a cluster larger than
    max_side is cut to its strongest max_side window.
    """
    _same_size(a, b)
    pa, pb = _pixels(a), _pixels(b)
    diff = np.abs(pa - pb).max(axis=2)
    holes_a, holes_b = _holes(pa, pb, clear)
    holes = holes_a | holes_b
    changed = (diff > threshold) | holes
    if not changed.any():
        return []
    labels = _components(_grow(changed, grow))
    ys, xs = np.nonzero(changed)
    owner = labels[ys, xs]
    clusters = []
    for label in np.unique(owner):
        pick = owner == label
        cy, cx = ys[pick], xs[pick]
        weight = np.zeros(diff.shape, dtype=np.int64)
        weight[cy, cx] = diff[cy, cx] + 255 * holes[cy, cx]
        x0, y0, x1, y1 = _rect(cx, cy, weight, margin, max_side)
        clusters.append(Cluster(x0, y0, x1, y1, int(holes[cy, cx].sum()), int(pick.sum()), int(diff[cy, cx].sum())))
    clusters.sort(key=lambda c: (c.holes, c.strength), reverse=True)
    return clusters[:count]


def crop_sheet(entries, label_a, label_b, zoom=ZOOM):
    """One picture of zoomed crops: per entry a row, per cluster A above B.

    entries is [(title, a, b, clusters)]; every cluster is headed with its
    title and where it was cut, and each crop sits under a bar naming its
    side. A crop over 512 px is zoomed less so the sheet stays a size a
    viewer opens.
    """
    if not label_a or not label_b:
        raise ValueError("a crops sheet needs a label for each of A and B")
    font, cell_min, gap, bar = _font(), 150, 6, 36
    rows = []
    for title, a, b, clusters in entries:
        cells = []
        for cluster in clusters:
            box = (cluster.x0, cluster.y0, cluster.x1, cluster.y1)
            scale = max(1, min(zoom, 512 // max(cluster.x1 - cluster.x0, cluster.y1 - cluster.y0)))
            size = ((cluster.x1 - cluster.x0) * scale, (cluster.y1 - cluster.y0) * scale)
            cells.append((box, [p.convert("RGB").crop(box).resize(size, Image.NEAREST) for p in (a, b)]))
        if cells:
            rows.append((title, cells))
    heights = [bar + 2 * (LABEL_BAR + max(c[1][0].size[1] for c in cells)) + 2 * gap for _t, cells in rows]
    widths = [sum(max(c[1][0].size[0], cell_min) + gap for c in cells) for _t, cells in rows]
    canvas = Image.new("RGB", (max(widths or [cell_min]), max(sum(heights), 1)), (24, 24, 24))
    draw, top = ImageDraw.Draw(canvas), 0
    for (title, cells), height in zip(rows, heights):
        left = 0
        for box, (crop_a, crop_b) in cells:
            place = "x%d y%d  %dx%d" % (box[0], box[1], box[2] - box[0], box[3] - box[1])
            outlined_text(draw, (left, top), title, (255, 255, 255), font)
            outlined_text(draw, (left, top + 17), place, (200, 200, 200), font)
            width = max(crop_a.size[0], cell_min)
            below = captioned(crop_a, label_a, width)
            canvas.paste(below, (left, top + bar))
            canvas.paste(captioned(crop_b, label_b, width), (left, top + bar + below.size[1] + gap))
            left += max(crop_a.size[0], cell_min) + gap
        top += height
    return canvas


def crops_path(out):
    """Where the crops sheet for a sheet or video written to out goes."""
    return out.rsplit(".", 1)[0] + ".crops.png"


def read_video(path):
    """(fps, frames) of a render_video.c AVI; frames yields (h, w, 3) RGB arrays."""
    video = check_avi.read_avi(path)
    stride = (video.width * 3 + 3) // 4 * 4

    def frames():
        for body in video.frames:
            rows = np.frombuffer(body, dtype=np.uint8).reshape(video.height, stride)
            bgr = rows[::-1, : video.width * 3].reshape(video.height, video.width, 3)
            yield np.ascontiguousarray(bgr[:, :, ::-1])

    return video.fps, frames()


def _font():
    try:
        return ImageFont.load_default(size=14)
    except TypeError:
        return ImageFont.load_default()


def outlined_text(draw, xy, text, fill, font):
    """Draw text ringed in the opposite shade of its fill, so a label reads over any panel."""
    light = 0.299 * fill[0] + 0.587 * fill[1] + 0.114 * fill[2] >= 128
    draw.text(xy, text, fill=fill, font=font, stroke_width=1, stroke_fill=(0, 0, 0) if light else (255, 255, 255))


def captioned(picture, text, width=0):
    """picture under a LABEL_BAR holding text, on a canvas at least width wide.

    The default font has no Greek or multiplication sign, so labels are ASCII.
    """
    picture = picture.convert("RGB")
    canvas = Image.new("RGB", (max(picture.width, width), picture.height + LABEL_BAR), (0, 0, 0))
    canvas.paste(picture, (0, LABEL_BAR))
    outlined_text(ImageDraw.Draw(canvas), (6, 3), text, (255, 255, 255), _font())
    return canvas


def compose_frame(a, b, clear, gain, label_a, label_b, note=""):
    """A | B | heatmap under a bar naming each; the heatmap's label carries note."""
    _same_size(a, b)
    texts = ("A  " + label_a, "B  " + label_b, ("diff x%d  " % gain + note).strip())
    panels = (a, b, heatmap(a, b, clear, gain))
    return Image.fromarray(np.concatenate([_pixels(captioned(p, t)) for p, t in zip(panels, texts)], axis=1).astype(np.uint8))


def frame_line(index, dt_ms, stats):
    """One CSV row: frame, time, changed share, mean abs diff, holes per side."""
    return "%d,%d,%.4f,%.4f,%d,%d" % (
        index,
        round(index * dt_ms),
        100.0 * stats.changed / stats.total,
        stats.mean_abs,
        stats.holes_a,
        stats.holes_b,
    )


def video_summary(all_stats, dt_ms):
    """One line about a whole video: the worst frame and the averages."""
    shares = [100.0 * s.changed / s.total for s in all_stats]
    worst = max(range(len(shares)), key=shares.__getitem__)
    return (
        "video: %d frames, changed share mean %.2f%% max %.2f%% (frame %d, %.1f s), "
        "mean abs diff %.3f/255, holes A peak %d, holes B peak %d"
        % (
            len(all_stats),
            sum(shares) / len(shares),
            shares[worst],
            worst,
            worst * dt_ms / 1000.0,
            sum(s.mean_abs for s in all_stats) / len(all_stats),
            max(s.holes_a for s in all_stats),
            max(s.holes_b for s in all_stats),
        )
    )


def _ffmpeg_command(ffmpeg, size, fps, out):
    return (
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24"]
        + ["-s", "%dx%d" % size, "-r", "%g" % fps, "-i", "-"]
        + ["-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-c:v", "libx264", "-crf", "16", "-preset", "medium"]
        + ["-pix_fmt", "yuv420p", "-movflags", "+faststart", out]
    )


def worst_frames(all_stats, count):
    """Indices of the count frames that differ most: holes first, then changed pixels."""
    order = sorted(range(len(all_stats)), key=lambda i: (all_stats[i].holes_a + all_stats[i].holes_b, all_stats[i].changed), reverse=True)
    return sorted(order[:count])


def write_crops(entries, out, label_a, label_b, zoom=ZOOM):
    """Save the crops sheet of entries unless nothing differs; True if written."""
    if not any(clusters for _t, _a, _b, clusters in entries):
        print("no differences: no crops written")
        return False
    crop_sheet(entries, label_a, label_b, zoom).save(out)
    return True


def write_video_crops(path_a, path_b, indices, out, clear, count, zoom, dt_ms, label_a, label_b):
    """A crops sheet of the given frames of two AVIs, one row per frame."""
    wanted = set(indices)
    picked = {}
    for name, path in (("a", path_a), ("b", path_b)):
        for index, raw in enumerate(read_video(path)[1]):
            if index in wanted:
                picked[(name, index)] = Image.fromarray(raw)
    entries = []
    for index in indices:
        a, b = picked[("a", index)], picked[("b", index)]
        title = "frame %d (%.1f s)" % (index, index * dt_ms / 1000.0)
        entries.append((title, a, b, find_clusters(a, b, clear, count)))
    write_crops(entries, out, label_a, label_b, zoom)


def compare_videos(path_a, path_b, out, csv_path, clear, gain, label_a, label_b, fps=None, crops=0, zoom=ZOOM):
    """Write the side-by-side mp4, the per-frame CSV and, with crops, the crops sheet.

    Returns the summary line.
    """
    fps_a, frames_a = read_video(path_a)
    fps_b, frames_b = read_video(path_b)
    if fps_a != fps_b:
        raise ValueError("frame rate %g vs %g: render both with the same --dt" % (fps_a, fps_b))
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError("ffmpeg not found; --video needs it to pack the frames")
    dt_ms = 1000.0 / fps_a
    process, lines, all_stats = None, [CSV_HEADER], []
    try:
        for index, (raw_a, raw_b) in enumerate(zip(frames_a, frames_b)):
            a, b = Image.fromarray(raw_a), Image.fromarray(raw_b)
            stats = measure(a, b, clear)
            all_stats.append(stats)
            lines.append(frame_line(index, dt_ms, stats))
            note = "t %.1f s  changed %.1f%%" % (index * dt_ms / 1000.0, 100.0 * stats.changed / stats.total)
            picture = compose_frame(a, b, clear, gain, label_a, label_b, note)
            if process is None:
                command = _ffmpeg_command(ffmpeg, picture.size, fps or fps_a, out)
                process = subprocess.Popen(command, stdin=subprocess.PIPE)
            process.stdin.write(picture.tobytes())
    finally:
        if process is not None:
            process.stdin.close()
            if process.wait() != 0:
                raise RuntimeError("ffmpeg failed writing %s" % out)
    if not all_stats:
        raise ValueError("no frames to compare")
    if csv_path:
        with open(csv_path, "w") as handle:
            handle.write("\n".join(lines) + "\n")
    if crops:
        worst = worst_frames(all_stats, CROP_FRAMES)
        write_video_crops(path_a, path_b, worst, crops_path(out), clear, crops, zoom, dt_ms, label_a, label_b)
    return video_summary(all_stats, dt_ms)


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


def reference_line(label, stats):
    """One reference comparison line: CIE76 ΔE and windowed luma SSIM. In the
    frames-mean line, p95 is the mean of the frames' own 95th percentiles."""
    return "%s: mean DeltaE76 %.4f, p95 DeltaE76 %.4f, luma SSIM %.6f, edge DeltaE76 %.4f, interior DeltaE76 %.4f, edge share %.4f" % (
        label, stats.mean_delta_e, stats.p95_delta_e, stats.ssim_luma, stats.edge_delta_e, stats.interior_delta_e, stats.edge_share
    )


def reference_video(path, reference_dir, scale=None, heatmaps=None, keep=None, sink=None, first=0):
    """Score every AVI frame against ordered reference PNGs and return their
    mean. `scale` is the render's pixels per reference pixel, found from the
    first frame when None. `keep`, a dict of frame index to None, is filled
    with the (render, reference) pairs of those frames; `sink(fps, index,
    render, reference)` is called for every frame. The first `first` video
    frames are skipped, for references that cover only a later segment."""
    fps, frames = read_video(path)
    references = sorted(pathlib.Path(reference_dir).glob("*.png"))
    values = []
    for position, raw in enumerate(frames):
        index = position - first
        if index < 0:
            continue
        if index >= len(references):
            raise ValueError("more video frames than reference images")
        render = Image.fromarray(raw)
        reference = Image.open(references[index])
        if scale is None:
            scale = render.width // reference.width
        if scale != 1:
            reference = reference.resize((reference.width * scale, reference.height * scale), Image.Resampling.NEAREST)
        stats = reference_measure(render, reference)
        if sink is not None:
            sink(fps, index, render, reference)
        values.append(stats)
        if heatmaps is not None:
            captioned(reference_heatmap(render, reference), "dE76").save(heatmaps / ("%04d.png" % index))
        if keep is not None and index in keep:
            keep[index] = (render, reference)
    if len(values) != len(references):
        raise ValueError("more reference images than video frames")
    return values, ReferenceStats(
        sum(item.mean_delta_e for item in values) / len(values),
        sum(item.p95_delta_e for item in values) / len(values),
        sum(item.ssim_luma for item in values) / len(values),
        sum(item.edge_delta_e for item in values) / len(values),
        sum(item.interior_delta_e for item in values) / len(values),
        sum(item.edge_share for item in values) / len(values),
    )


class ReferenceVideoWriter:
    """Packs reference_sheet's one-frame sheets into an mp4 through ffmpeg."""

    def __init__(self, out, label, fps=None):
        self.out, self.label, self.fps, self.process = out, label, fps, None

    def add(self, fps, index, render, reference):
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg is None:
            raise RuntimeError("ffmpeg not found; --reference-mp4 needs it to pack the frames")
        picture = reference_sheet([("frame %d" % index, render, reference)], self.label, tile=1.0)
        if self.process is None:
            self.process = subprocess.Popen(_ffmpeg_command(ffmpeg, picture.size, self.fps or fps, self.out), stdin=subprocess.PIPE)
        self.process.stdin.write(picture.tobytes())

    def close(self):
        if self.process is not None:
            self.process.stdin.close()
            if self.process.wait() != 0:
                raise RuntimeError("ffmpeg failed writing %s" % self.out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True)
    parser.add_argument("--summary")
    parser.add_argument("--clear", type=parse_rgb)
    parser.add_argument("--row", nargs=3, action="append", metavar=("LABEL", "A", "B"))
    parser.add_argument("--reference-row", nargs=3, action="append", metavar=("LABEL", "RENDER", "REFERENCE"))
    parser.add_argument("--reference-video", nargs=2, metavar=("VIDEO", "REFERENCE_DIR"))
    parser.add_argument("--reference-scale", type=int, help="render pixels per reference pixel; default from the first frame")
    parser.add_argument("--reference-first", type=int, default=0, help="video frames to skip before the reference images begin")
    parser.add_argument("--reference-mp4", metavar="MP4", help="with --reference-video, the sheet of every frame as a video")
    parser.add_argument("--heatmap-dir")
    parser.add_argument("--reference-sheet", metavar="PNG", help="with --reference-video, one sheet of --sheet-frames")
    parser.add_argument("--sheet-frames", default="2,4", help="comma-separated video frame indices for --reference-sheet")
    parser.add_argument("--video", nargs=2, metavar=("A.avi", "B.avi"))
    parser.add_argument("--csv")
    parser.add_argument("--fps", type=float)
    parser.add_argument("--label-a", help="what A shows; required with --row, --video and the reference sheet or mp4")
    parser.add_argument("--label-b", help="what B shows; required with --row and --video")
    parser.add_argument("--crops", type=int, default=0)
    args = parser.parse_args()

    if (args.row or args.video) and not (args.label_a and args.label_b):
        parser.error("--label-a and --label-b are required: every panel is labelled")
    if (args.reference_sheet or args.reference_mp4) and not args.label_a:
        parser.error("--label-a, naming the render, is required with --reference-sheet or --reference-mp4")
    if args.video:
        line = compare_videos(*args.video, args.out, args.csv, args.clear, GAIN, args.label_a, args.label_b, args.fps, args.crops, ZOOM)
        print(line)
        if args.summary:
            with open(args.summary, "a") as handle:
                handle.write(line + "\n")
        return
    if not args.row and not args.reference_row and not args.reference_video:
        parser.error("--row, --reference-row or --video is required")

    if args.reference_row:
        heatmaps = None if args.heatmap_dir is None else pathlib.Path(args.heatmap_dir)
        if heatmaps is not None:
            heatmaps.mkdir(parents=True, exist_ok=True)
        lines = []
        for label, render_path, reference_path in args.reference_row:
            render, reference = Image.open(render_path), Image.open(reference_path)
            stats = reference_measure(render, reference)
            lines.append(reference_line(label, stats))
            if heatmaps is not None:
                captioned(reference_heatmap(render, reference), "dE76").save(heatmaps / (label + ".png"))
        print("\n".join(lines))
        if args.summary:
            with open(args.summary, "a") as handle:
                handle.write("\n".join(lines) + "\n")
        if not args.row:
            return

    if args.reference_video:
        if args.reference_scale is not None and args.reference_scale < 1:
            parser.error("--reference-scale must be positive")
        heatmaps = None if args.heatmap_dir is None else pathlib.Path(args.heatmap_dir)
        if heatmaps is not None:
            heatmaps.mkdir(parents=True, exist_ok=True)
        keep = {int(index): None for index in args.sheet_frames.split(",")} if args.reference_sheet else None
        video = ReferenceVideoWriter(args.reference_mp4, args.label_a, args.fps) if args.reference_mp4 else None
        try:
            frames, total = reference_video(*args.reference_video, args.reference_scale, heatmaps, keep,
                                            None if video is None else video.add, args.reference_first)
        finally:
            if video is not None:
                video.close()
        if keep is not None:
            sheet_rows = [("frame %d" % index, *pair) for index, pair in keep.items() if pair]
            reference_sheet(sheet_rows, args.label_a).save(args.reference_sheet, optimize=True)
        lines = [reference_line("frame %d" % index, stats) for index, stats in enumerate(frames)]
        lines.append(reference_line("frames mean", total))
        print("\n".join(lines))
        if args.summary:
            with open(args.summary, "a") as handle:
                handle.write("\n".join(lines) + "\n")
        if not args.row:
            return

    rows = [(label, Image.open(a), Image.open(b)) for label, a, b in args.row]
    sheet(rows, args.clear, args.label_a, args.label_b).save(args.out)
    if args.crops:
        entries = [(label, a, b, find_clusters(a, b, args.clear, args.crops)) for label, a, b in rows]
        write_crops(entries, crops_path(args.out), args.label_a, args.label_b, ZOOM)
    lines = [summary_line(label, measure(a, b, args.clear)) for label, a, b in rows]
    print("\n".join(lines))
    if args.summary:
        with open(args.summary, "a") as handle:
            handle.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
