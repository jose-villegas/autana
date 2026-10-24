#!/usr/bin/env python3
"""Plot a spring line's wave for docs/physics/Spring.md; --cc CC --out DIR."""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "math"))
from math_figures import PANEL_SIZE, PANEL_TITLE_ROOM, SHAPE_ALPHA, run_probe
from docs.plot_style import INK, LABEL_SIZE, SLOTS, TITLE_SIZE, save, save_gif, subplots, GIF_FRAMES, GIF_FPS
import numpy as np

PROBE = pathlib.Path(__file__).with_name("spring_figures_probe.c")
PANELS = 2
WAVE_MARGIN = 1.2
LEGEND_ANCHOR = (0.5, -0.22)
BUMP_MARKER_SIZE = 3
COLORBAR_SHRINK = 0.7
COLUMN_EDGE = 0.5
OFFSET_COLORMAP = "RdBu_r"
PANEL_LABEL_PADDING = 0.15
WAVE_COLOR_PERCENTILE = 98
WAVE_PUSH_TICKS = 5


def wave_figures(data, output):
    runs = data["samples"]
    offsets = np.array([row["offset"] for row in runs])
    columns = np.arange(offsets.shape[1])
    fig, axis = subplots(1, 1, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    line, = axis.plot(columns, offsets[0], color=SLOTS[0])
    shade = None
    axis.set(xlabel="Column (free ends)", ylabel="Offset (px)", ylim=(offsets.min() * WAVE_MARGIN, offsets.max() * WAVE_MARGIN))
    indices = np.linspace(0, len(runs) - 1, GIF_FRAMES).astype(int)

    def draw(frame):
        nonlocal shade
        index = indices[frame]
        row = runs[index]
        if shade is not None:
            shade.remove()
        shade = None if row["rest"] else axis.axvspan(row["lo"], row["hi"], color=SLOTS[2], alpha=SHAPE_ALPHA)
        line.set_ydata(offsets[index])
        axis.set_title(f"Tick {row['tick']} — {'at rest' if row['rest'] else 'active range shaded'}", fontsize=TITLE_SIZE)

    save_gif(fig, draw, GIF_FRAMES, output / "spring-line-wave.gif", GIF_FPS)
    fig, (heat, bump) = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    fig.get_layout_engine().set(w_pad=PANEL_LABEL_PADDING)
    bound = np.percentile(abs(offsets[WAVE_PUSH_TICKS:]), WAVE_COLOR_PERCENTILE)
    image = heat.imshow(offsets, aspect="auto", origin="upper", cmap=OFFSET_COLORMAP, vmin=-bound, vmax=bound,
                        extent=(-COLUMN_EDGE, offsets.shape[1] - COLUMN_EDGE, len(runs) - COLUMN_EDGE, -COLUMN_EDGE))
    active = [row for row in runs if not row["rest"]]
    for key, label, style in (("lo", "active_lo", "-"), ("hi", "active_hi (exclusive)", "--")):
        heat.plot([row[key] for row in active], [row["tick"] for row in active], color=INK,
                  linestyle=style, label=label)
    heat.set(xlabel="Column", ylabel=f"Tick ({data['tick_ms']} ms)", title="Offset and active range")
    heat.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR)
    fig.colorbar(image, ax=heat, shrink=COLORBAR_SHRINK, label="Offset (px, clipped)")
    bump.plot(columns, offsets[0], "o-", color=SLOTS[0], markersize=BUMP_MARKER_SIZE)
    bump.set(xlabel="Column", ylabel="Offset (px)", ylim=(0, None), title=r"Nudge: $(1-t^2)^2$")
    save(fig, output / "spring-line-spacetime.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    wave_figures(run_probe(args.cc, (PROBE,)), args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
