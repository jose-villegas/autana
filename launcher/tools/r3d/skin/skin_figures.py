"""The skinning maths figure of docs/render/animation/Skeleton-and-Skin.md: a two-joint limb's weights along its length,
the limb bent by linear blend skinning beside a single joint per vertex, and how thick each keeps the hinge as
the bend grows. The numbers come from skin_figures_probe.c, which runs r3d_skin_palette() and
r3d_skin_clusters(); the weights and positions are quantized by skin_asset.py.

    python launcher/tools/r3d/skin/skin_figures.py --cc CC --out PNG

launcher/tools/render/render_doc_images.sh runs it.
"""

import argparse
import math
import pathlib
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "math"))
from math_figures import run_probe  # noqa: E402
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, style_axes, subplots, save  # noqa: E402
from r3d import skin_asset  # noqa: E402

PROBE = pathlib.Path(__file__).with_name("skin_figures_probe.c")
PROBE_SOURCES = (PROBE, TOOLS.parent / "main/render/skin/r3d_skinning.c",
                 TOOLS.parent / "main/render/skin/r3d_skin_light.c")
# The limb, in model units: two joints, the second at the hinge, a strip of COLUMNS vertex pairs along x.
HINGE = 1.0
LENGTH = 2.0 * HINGE
HALF_THICKNESS = 0.2
COLUMNS = 41
# Weights ramp from the first joint to the second across this width centred on the hinge.
BLEND_WIDTH = 0.6
# Ticks per model unit: the limb's length must fit int16 ticks.
POSITION_SCALE = 8192
SHOWN_ANGLE = 90
ANGLE_STEP = 10
HALF_TURN = 180
PANEL = (3.2, 3.0)  # inches per panel
BLEND, RIGID = SLOTS[0], SLOTS[1]


def limb():
    """(x, y, source weights of joints 0 and 1) per vertex: the hinge column's top vertex first, the one traced."""
    columns = [LENGTH * c / (COLUMNS - 1) for c in range(COLUMNS)]
    hinge_column = min(range(COLUMNS), key=lambda c: abs(columns[c] - HINGE))
    order = [hinge_column] + [c for c in range(COLUMNS) if c != hinge_column]
    out = []
    for row in (HALF_THICKNESS, -HALF_THICKNESS):
        for c in order:
            t = min(max((columns[c] - (HINGE - BLEND_WIDTH / 2)) / BLEND_WIDTH, 0.0), 1.0)
            out.append((columns[c], row, (1.0 - t, t)))
    # Top row first then bottom, each starting at the hinge: the traced vertex is 0, its partner COLUMNS.
    return out


def records(vertices, rigid):
    """Each vertex's joints and weight bytes through quantize_weights, or all of it on its heaviest joint."""
    out = []
    for _, _, weights in vertices:
        joints, quantized = skin_asset.quantize_weights([0, 1], list(weights), skin_asset.INFLUENCES[0])
        bytes_by_joint = {j: w for j, w in zip(joints, quantized) if w}
        if rigid:
            heaviest = max(sorted(bytes_by_joint), key=lambda j: bytes_by_joint[j])
            bytes_by_joint = {heaviest: skin_asset.WEIGHT_SUM}
        out.append([bytes_by_joint.get(0, 0), bytes_by_joint.get(1, 0)])
    return out


def run(cc, vertices, weights, angles):
    with tempfile.TemporaryDirectory() as work:
        path = pathlib.Path(work) / "limb.txt"
        lines = [f"{POSITION_SCALE} {HINGE} {len(angles)} " + " ".join(map(str, angles)), str(len(vertices))]
        for (x, y, _), (w0, w1) in zip(vertices, weights):
            tx, ty, _ = skin_asset.quantize_position((x, y, 0.0), POSITION_SCALE)
            lines.append(f"{tx} {ty} 0 1 {w0} {w1}")
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return run_probe(cc, PROBE_SOURCES, path)


def outline(vertices, positions):
    """The strip's outline: the top row by bind x, then the bottom row back."""
    top = sorted(range(COLUMNS), key=lambda i: vertices[i][0])
    bottom = sorted(range(COLUMNS, 2 * COLUMNS), key=lambda i: -vertices[i][0])
    points = [positions[i] for i in top + bottom + top[:1]]
    return [p[0] for p in points], [p[1] for p in points]


def figure(blend, rigid, vertices, weights, output):
    fig, (weight_axis, bend_axis, sweep_axis) = subplots(1, 3, (3 * PANEL[0], PANEL[1]))

    style_axes(weight_axis, "weight bytes along the limb")
    xs = [x for x, _, _ in vertices[:COLUMNS]]
    order = sorted(range(COLUMNS), key=lambda i: xs[i])
    for joint, colour in ((0, BLEND), (1, RIGID)):
        weight_axis.plot([xs[i] for i in order], [weights[i][joint] for i in order], color=colour,
                         label=f"joint {joint}")
    weight_axis.axvline(HINGE, color=FAINT, linestyle="--", linewidth=1)
    weight_axis.set_ylim(0, skin_asset.WEIGHT_SUM * 1.05)
    weight_axis.set_xlabel("x along the limb", fontsize=LABEL_SIZE)
    weight_axis.legend(fontsize=LABEL_SIZE, loc="center right")

    shown = next(p for p in blend["poses"] if p["angle"] == SHOWN_ANGLE)
    shown_rigid = next(p for p in rigid["poses"] if p["angle"] == SHOWN_ANGLE)
    style_axes(bend_axis, f"bent {SHOWN_ANGLE} degrees at the hinge", square=True)
    bend_axis.plot(*outline(vertices, shown_rigid["positions"]), color=RIGID, linestyle="--", label="one joint per vertex")
    bend_axis.plot(*outline(vertices, shown["positions"]), color=BLEND, label="linear blend")
    turn = math.radians(SHOWN_ANGLE)
    hinge = shown["joints"][1]
    joints = shown["joints"] + [[hinge[0] + (LENGTH - HINGE) * math.cos(turn), hinge[1] + (LENGTH - HINGE) * math.sin(turn)]]
    bend_axis.plot([p[0] for p in joints], [p[1] for p in joints], color=INK, marker="o", markersize=4)
    # The traced vertex: carried rigidly by each joint, and their weighted mix.
    carried = shown["traced"]
    bend_axis.plot([c[0] for c in carried], [c[1] for c in carried], color=MUTED, linestyle=":", marker="x",
                   label="vertex carried by each joint")
    bend_axis.scatter(*shown["positions"][0], color=BLEND, zorder=3)
    bend_axis.legend(fontsize=LABEL_SIZE, loc="upper left")

    style_axes(sweep_axis, "hinge thickness as it bends")
    for data, colour, name in ((blend, BLEND, "linear blend"), (rigid, RIGID, "one joint per vertex")):
        thickness = [abs(complex(*p["positions"][0]) - complex(*p["positions"][COLUMNS])) / (2 * HALF_THICKNESS)
                     for p in data["poses"]]
        sweep_axis.plot([p["angle"] for p in data["poses"]], thickness, color=colour, label=name)
    sweep_axis.set_ylim(0, 1.1)
    sweep_axis.set_xlim(0, HALF_TURN)
    sweep_axis.set_xlabel("bend, degrees", fontsize=LABEL_SIZE)
    sweep_axis.set_ylabel("share of the bind thickness", fontsize=LABEL_SIZE)
    sweep_axis.legend(fontsize=LABEL_SIZE, loc="lower left")
    save(fig, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    vertices = limb()
    angles = list(range(0, HALF_TURN + 1, ANGLE_STEP))
    blended, rigid = records(vertices, False), records(vertices, True)
    figure(run(args.cc, vertices, blended, angles), run(args.cc, vertices, rigid, angles), vertices, blended,
           args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
