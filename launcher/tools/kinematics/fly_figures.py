#!/usr/bin/env python3
"""Plot one scripted flight's path and speed for docs/kinematics/Fly.md; --cc CC --out DIR."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "math"))
from math_figures import PANEL_SIZE, ROOT, VIEW_AZIMUTH, VIEW_ELEVATION
from docs.figures import probe_into_folder
from docs.producer import Producer
from docs.plot_style import FAINT, INK, LABEL_SIZE, SLOTS, TITLE_SIZE, figure, save
import numpy as np

PROBE = pathlib.Path(__file__).with_name("fly_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/packages/kinematics/src/fly/fly_motion.c")
PRODUCER = Producer(area="kinematics", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("fly-path.png",))
ARROW_EVERY = 4  # samples between the forward arrows drawn on the path
ARROW_LENGTH = 1.6  # metres
PANEL_RATIOS = (3, 1)
LEGEND_ANCHOR = (0.5, -0.02)
SPEED_HEADROOM = 1.15


def path_figure(data, output):
    samples = data["samples"]
    times = np.array([row["time"] for row in samples])
    positions = np.array([row["position"] for row in samples])
    forwards = np.array([row["axes"][2] for row in samples])
    speeds = np.array([row["speed"] for row in samples])
    phases = np.array([row["phase"] for row in samples])
    fig = figure((PANEL_SIZE * 2, PANEL_SIZE * 2))
    grid = fig.add_gridspec(2, 1, height_ratios=PANEL_RATIOS)
    path = fig.add_subplot(grid[0], projection="3d")
    strip = fig.add_subplot(grid[1])
    start = np.vstack(([0.0, 0.0, 0.0], positions))
    for phase, name in enumerate(data["phases"]):
        picked = np.flatnonzero(phases == phase)
        run = start[picked[0]:picked[-1] + 2]
        colour = SLOTS[phase % len(SLOTS)]
        path.plot(run[:, 0], run[:, 1], run[:, 2], color=colour, linewidth=2, label=name)
        for index in picked[::ARROW_EVERY]:
            path.quiver(*positions[index], *(forwards[index] * ARROW_LENGTH), color=colour, linewidth=1)
        strip.plot(times[picked], speeds[picked], color=colour)
    path.scatter(0, 0, 0, color=INK)
    lows, highs = positions.min(axis=0), positions.max(axis=0)
    middle, half = (lows + highs) / 2, (highs - lows).max() / 2 + ARROW_LENGTH
    path.set(xlim=(middle[0] - half, middle[0] + half), ylim=(middle[1] - half, middle[1] + half),
             zlim=(middle[2] - half, middle[2] + half), xlabel="x (m)", ylabel="y, up (m)", zlabel="z (m)")
    path.set_box_aspect((1, 1, 1))
    path.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH, vertical_axis="y")
    path.set_title("A flight from the origin facing +z; arrows are the pose's forward", fontsize=TITLE_SIZE)
    path.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR, ncol=3, frameon=False)
    strip.axhline(speeds[0], color=FAINT, linestyle="--", linewidth=1)
    strip.set(xlabel="Time (s)", ylabel="Speed (m/s)", ylim=(0, speeds.max() * SPEED_HEADROOM))
    save(fig, output)


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    path_figure(data, out / "fly-path.png")
    end = data["samples"][-1]
    print(f"end: position={end['position']}, speed={end['speed']:.4g} m/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
