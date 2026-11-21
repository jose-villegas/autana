#!/usr/bin/env python3
"""Plot a plate turning about a hinge axis for docs/kinematics/Hinge.md; --cc CC --out DIR."""
import pathlib

from kinematics_figures import PANEL_SIZE, ROOT, VIEW_AZIMUTH, VIEW_ELEVATION
from docs.figures import probe_into_folder
from docs.producer import Producer
from docs.plot_style import FAINT, INK, LABEL_SIZE, SLOTS, figure, save
import numpy as np
from matplotlib.ticker import MaxNLocator

SOURCES = (pathlib.Path(__file__).with_name("hinge_figures_probe.c"),
           ROOT / "launcher/packages/kinematics/src/hinge/hinge.c")
PRODUCER = Producer(area="kinematics", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("hinge-sweep.png",))
AXIS_EXTENT = 0.8


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    plates = np.array(data["plates"])
    fig = figure((PANEL_SIZE * 2, PANEL_SIZE * 1.6))
    axis = fig.add_subplot(projection="3d")
    axis.plot([0, 0], [0, 0], [-AXIS_EXTENT, AXIS_EXTENT], color=INK, label="Hinge axis (+z)")
    tips = plates[:, 1, :]
    axis.plot(tips[:, 0], tips[:, 1], tips[:, 2], color=FAINT, linestyle="--", label="Far corner's arc")
    for index, name, colour in ((0, "min: −60°", SLOTS[0]),
                                (len(plates) // 2, "Rest: 0°", INK),
                                (len(plates) - 1, "max: +60°", SLOTS[1])):
        points = plates[index]
        axis.plot(points[:, 0], points[:, 1], points[:, 2], color=colour, label=name)
    axis.set(xlabel="x", ylabel="y, up", zlabel="z", xlim=(0, 1), ylim=(-1, 1),
             zlim=(-AXIS_EXTENT, AXIS_EXTENT))
    for coordinate in (axis.xaxis, axis.yaxis, axis.zaxis):
        coordinate.set_major_locator(MaxNLocator(nbins=3))
    axis.set_box_aspect((1, 2, 2 * AXIS_EXTENT))
    axis.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH, vertical_axis="y")
    axis.legend(fontsize=LABEL_SIZE, loc="upper left", frameon=False)
    save(fig, out / "hinge-sweep.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
