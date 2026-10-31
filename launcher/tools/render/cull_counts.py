"""The cull view's per-frame counts as curves: their names, read from
raster_cull_view.h's RASTER_CULL_FRAME_FIELDS, the fate colours from
raster_cull_view.c, and the series every counts chart draws."""

import pathlib
import re

from docs.plot_style import INK, MUTED, SLOTS, style_axes

ROOT = pathlib.Path(__file__).resolve().parents[3]
CULL_VIEW = ROOT / "launcher/main/render/view/raster_cull_view.c"
CULL_HEADER = ROOT / "launcher/main/render/view/raster_cull_view.h"
LINE_WIDTH = 1.4


def fields(source=CULL_HEADER):
    """Every count's name, in the order the view lists them."""
    text = pathlib.Path(source).read_text(encoding="utf-8")
    body = re.search(r"#define RASTER_CULL_FRAME_FIELDS\(X\)((?:.*\\\n)*.*)", text)[1]
    return re.findall(r"X\((\w+),", body)


def fates(source=CULL_VIEW):
    """(name, #rrggbb) per fate, in the view's order, read from its colour table."""
    text = pathlib.Path(source).read_text(encoding="utf-8")
    table = re.search(r"FATE_RGB\[RASTER_FATE_COUNT\] = \{(.*?)\};", text, re.S)[1]
    return [(name.lower().replace("_", " "), "#" + rgb.lower())
            for name, rgb in re.findall(r"\[RASTER_FATE_(\w+)\] = 0x([0-9A-Fa-f]{6})", table)]


def cluster_series():
    """(label, count, colour): the clusters tested and kept, then each kept fate in its view colour."""
    kept = [(name, f"clusters_{name.replace(' ', '_')}", colour) for name, colour in fates()[1:]]
    return [("tested", "clusters_tested", MUTED), ("kept", "clusters_kept", INK), *kept]


TRIANGLE_SERIES = [("submitted", "triangles_submitted", INK), ("set up", "triangles_set_up", SLOTS[0]),
                   ("cover pixels", "triangles_covering", SLOTS[2]),
                   ("facing away", "triangles_facing_away", SLOTS[1]),
                   ("miss every pixel centre", "triangles_missed", MUTED)]


def check_series():
    """Fails when a series names a count the view does not list."""
    names = set(fields())
    missing = [key for _, key, _ in (*cluster_series(), *TRIANGLE_SERIES) if key not in names]
    if missing:
        raise ValueError(f"{CULL_HEADER.name}: no count named {', '.join(missing)}")


def plot_counts(clusters, triangles, seconds, rows, label, legend=True):
    """The clusters' and the triangles' curves over `seconds`, one per series, from count rows."""
    check_series()
    for axis, series, what in ((clusters, cluster_series(), "clusters"), (triangles, TRIANGLE_SERIES, "triangles")):
        for name, key, colour in series:
            axis.plot(seconds, [row[key] for row in rows], label=name, color=colour, linewidth=LINE_WIDTH)
        style_axes(axis, f"{label}: {what} per frame")
        axis.set_ylim(bottom=0)
        if legend:
            axis.legend(frameon=False, fontsize="small")
