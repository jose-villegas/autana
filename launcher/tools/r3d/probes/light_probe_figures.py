"""The figures in docs/render/frame/Light-Probes.md, drawn from light_probe_figures_probe.c, which calls the
runtime's SH projection and convolution and its probe-grid sampler, and from a scene's baked SHPR grid.

    python launcher/tools/r3d/probes/light_probe_figures.py --cc CC --out DIR --scene SCENE.toml [--grid SHPR]

light-probe-sphere.png: one probe projected from a sky with a sun, then a white diffuse sphere lit by the
exact irradiance, by the probe's L1 evaluation and by its constant band alone, with a meridian below.
light-probe-blend.png: an example grid's probes over a plane, the blended constant band between and past them,
and the corner weights one point takes.
light-probe-<scene>.png: the scene's grid, a plan of each layer of probes coloured by the light an upward face
gets.
light-probe-<scene>-spheres.png: a white sphere at the brightest, median and darkest probe of the grid's second
layer, lit by the scene's flat ambient and by the grid.

--grid reads an SHPR file in place of the scene's locked bake (bake.py path <scene>.shpr).
"""
import argparse
import subprocess
import tempfile
import tomllib
import pathlib
import sys

import numpy as np
from matplotlib.patches import Rectangle

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from docs.figures import ROOT, build_probe, probe_json  # noqa: E402
from docs.plot_style import INK, LABEL_SIZE, MUTED, SLOTS, figure, save, style_axes, subplots  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402
from r3d.sh_probe_asset import SUFFIX  # noqa: E402

SOURCES = (HERE / "light_probe_figures_probe.c", ROOT / "launcher/packages/math/src/sh/sh1.c",
           ROOT / "launcher/main/render/probe/sh_probes.c", ROOT / "launcher/main/render/skin/r3d_skin_light.c")
BAKE = ROOT / "launcher/tools/bake/bake.py"
GAMMA = 1 / 2.2
BACKGROUND = 1.0  # white behind each sphere
EXACT, L1, FLAT = SLOTS[0], SLOTS[1], SLOTS[2]
SPHERE_SIZE = (9, 5.4)
SPHERE_HEIGHTS = (1.25, 1)
BLEND_SIZE = (7, 4.6)
PROBE_DOT = 120  # points squared
POINT_DOT = 40
LAYER_SIZE = (9, 3.4)
LAYER_COLUMNS = 4
SPHERES_SIZE = (9, 3.4)
UP = 0  # AXIS_NORMALS' order in the probe: +y, -y, +x, -x, +z, -z
FLOOR_LAYER = 1  # the second layer up: the first may sit at floor height


def encode(linear, white):
    """Linear light over `white` to display values: clipped, then gamma encoded."""
    return np.clip(np.asarray(linear) / white, 0, 1) ** GAMMA


def tone(linear, middle):
    """Linear light to display values for a scene with sun and shade: x / (1 + x) per channel, x the light over
    `middle`, so `middle` shows at half and nothing clips (Reinhard et al. 2002), then gamma encoded."""
    x = np.clip(np.asarray(linear), 0, None) / middle
    return (x / (1 + x)) ** GAMMA


def sphere_picture(rows, white, mapping=encode):
    """A probe picture's rows (RGB or None per pixel) as an image on a white page, each pixel mapped with
    `white` by `mapping`."""
    out = np.full((len(rows), len(rows[0]), 3), BACKGROUND)
    for y, row in enumerate(rows):
        for x, pixel in enumerate(row):
            if pixel is not None:
                out[y, x] = mapping(pixel, white)
    return out


def luminance(rgb):
    """Rec. 709 luminance of linear RGB."""
    rgb = np.asarray(rgb)
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def sphere_plot(data, out):
    fig = figure(SPHERE_SIZE)
    grid = fig.add_gridspec(2, 4, height_ratios=SPHERE_HEIGHTS)
    white = max(luminance(row[1]) for row in data["meridian"])
    sky_white = float(np.max([luminance(p) for row in data["sky"] for p in row if p is not None
                              and luminance(p) < luminance(data["coefficients"][0]) * 4]))
    panels = (("sky", "what the probe sees\n(radiance; the sun clips)", sky_white),
              ("exact", "exact irradiance\n(every direction, cosine weighted)", white),
              ("l1", "the probe: L1\n(constant + gradient)", white),
              ("flat", "constant band only\n(flat ambient)", white))
    for column, (key, title, scale) in enumerate(panels):
        axis = fig.add_subplot(grid[0, column])
        axis.imshow(sphere_picture(data[key], scale), interpolation="nearest")
        axis.set_axis_off()
        axis.set_title(title, fontsize=LABEL_SIZE, color=INK)
    axis = fig.add_subplot(grid[1, :])
    degrees = [row[0] for row in data["meridian"]]
    for index, colour, label in ((1, EXACT, "exact"), (2, L1, "L1 probe"), (3, FLAT, "constant band only")):
        axis.plot(degrees, [luminance(row[index]) for row in data["meridian"]], color=colour, label=label)
    axis.set_xlim(0, 180)
    axis.set_xticks(range(0, 181, 30))
    axis.set_ylim(bottom=0)
    axis.set_xlabel("normal's angle from straight up, turning toward the sun's side, degrees", fontsize=LABEL_SIZE,
                    color=MUTED)
    axis.set_ylabel("irradiance / pi (luminance)", fontsize=LABEL_SIZE, color=MUTED)
    axis.legend(fontsize=LABEL_SIZE, frameon=False)
    style_axes(axis, "Along one meridian: L1 keeps the sky-to-ground gradient, not the sun's peak")
    save(fig, out / "light-probe-sphere.png")


def blend_plot(data, out):
    grid = data["grid"]
    columns, rows, margin = grid["x"], grid["z"], grid["margin"]
    extent = (-margin, columns - 1 + margin, rows - 1 + margin, -margin)
    plane = np.array(data["grid"]["plane"])
    fig, axis = subplots(1, 1, BLEND_SIZE)
    axis.imshow(encode(plane, 1.0), extent=extent, interpolation="bilinear")
    axis.add_patch(Rectangle(
        (0, 0), columns - 1, rows - 1, fill=False, edgecolor=INK, linewidth=0.8, linestyle="--"))
    xs = [p % columns for p in range(columns * rows)]
    zs = [p // columns for p in range(columns * rows)]
    axis.scatter(xs, zs, s=PROBE_DOT, c=[encode(c, 1.0) for c in grid["probes"]], edgecolors=INK, linewidths=1,
                 zorder=3)
    point = grid["point"]
    for p, weight in enumerate(grid["weights"]):
        if weight > 0:
            axis.plot([point[0], xs[p]], [point[2], zs[p]], color=INK, linewidth=0.8, zorder=2)
            axis.annotate(f"{weight:.2f}", ((point[0] + xs[p]) / 2, (point[2] + zs[p]) / 2), fontsize=LABEL_SIZE,
                          color=INK, ha="center", va="center", backgroundcolor="white", zorder=4)
    axis.scatter([point[0]], [point[2]], s=POINT_DOT, c="white", edgecolors=INK, zorder=5)
    axis.text(columns - 1 + margin * 0.9, -margin * 0.6, "past the last probe: clamped,\nthe edge probes' value",
              fontsize=LABEL_SIZE, color=INK, ha="right", va="center", backgroundcolor="white")
    axis.set_xlabel("x (probe spacing)", fontsize=LABEL_SIZE, color=MUTED)
    axis.set_ylabel("z (probe spacing)", fontsize=LABEL_SIZE, color=MUTED)
    style_axes(axis, f"A {columns}x1x{rows} grid seen from above: each point blends its corners' coefficients",
               square=True)
    save(fig, out / "light-probe-blend.png")


def grid_path(given, scene):
    """The SHPR file: `given`, or the scene's locked bake's cached copy."""
    if given:
        return pathlib.Path(given)
    out = subprocess.run([sys.executable, str(BAKE), "path", scene_id(scene) + SUFFIX], check=True,
                         capture_output=True, text=True)
    return pathlib.Path(out.stdout.strip().splitlines()[-1])


def scene_settings(path):
    """(the scene's flat ambient, colour times intensity, as the runtime lights a mover without a grid; the light
    the scene's tone map takes as its white, which the figures show at half)."""
    scene = tomllib.loads(path.read_text(encoding="utf-8"))
    return np.asarray(scene["ambient"]["color"], float) * scene["ambient"]["intensity"], scene["tonemap_white"]


def layers(data):
    """The grid's upward irradiance indexed [y][z][x], and the probe positions along x, y and z."""
    dims = data["dims"]
    up = np.array([probe[UP] for probe in data["irradiance"]]).reshape(dims[2], dims[1], dims[0], 3)
    axes = [data["origin"][a] + data["spacing"][a] * np.arange(dims[a]) for a in range(3)]
    return up.transpose(1, 0, 2, 3), axes


def grid_plot(data, middle, name, out):
    up, (xs, ys, zs) = layers(data)
    rows = -(-len(ys) // LAYER_COLUMNS)
    fig, axes = subplots(rows, LAYER_COLUMNS, LAYER_SIZE)
    half = np.array(data["spacing"]) / 2
    extent = (xs[0] - half[0], xs[-1] + half[0], zs[-1] + half[2], zs[0] - half[2])
    for index, axis in enumerate(axes.flat):
        if index >= len(ys):
            axis.set_axis_off()
            continue
        axis.imshow(tone(up[index], middle), extent=extent, interpolation="nearest")
        axis.set_title(f"layer {index}: y = {ys[index]:.0f}", fontsize=LABEL_SIZE, color=INK)
        axis.tick_params(labelsize=LABEL_SIZE - 2, colors=MUTED)
    dims = data["dims"]
    fig.suptitle(f"{name.title()}'s {dims[0]}x{dims[1]}x{dims[2]} grid from above, layer by layer: the light an upward "
                 "face gets (x across, z down; the scene's tone-map white at half grey)", fontsize=LABEL_SIZE + 1)
    save(fig, out / f"light-probe-{name}.png")


def floor_points(data):
    """(label, point) for the brightest, median and darkest probe of the first layer above the floor."""
    up, (xs, ys, zs) = layers(data)
    order = np.argsort(luminance(up[FLOOR_LAYER]).ravel())
    picks = (("brightest", order[-1]), ("median", order[len(order) // 2]), ("darkest", order[0]))
    return [(label, (xs[i % len(xs)], ys[FLOOR_LAYER], zs[i // len(xs)])) for label, i in picks]


def spheres_plot(data, flat, middle, points, name, out):
    flat_rows = [[flat if p is not None else None for p in row] for row in data["spheres"][0]["lit"]]
    fig, axes = subplots(1, len(points) + 1, SPHERES_SIZE)
    axes[0].imshow(sphere_picture(flat_rows, middle, tone), interpolation="nearest")
    axes[0].set_title("before: the scene's\nflat ambient", fontsize=LABEL_SIZE, color=INK)
    for axis, (label, point), sphere in zip(axes[1:], points, data["spheres"]):
        axis.imshow(sphere_picture(sphere["lit"], middle, tone), interpolation="nearest")
        axis.set_title(f"probes: the {label}\nat ({point[0]:.0f}, {point[1]:.0f}, {point[2]:.0f})",
                       fontsize=LABEL_SIZE, color=INK)
    for axis in axes:
        axis.set_axis_off()
    fig.suptitle(f"A white diffuse sphere in {name.title()} lit by the grid (sky, bounce and sun) and, before, by the "
                 "flat ambient", fontsize=LABEL_SIZE + 1)
    save(fig, out / f"light-probe-{name}-spheres.png")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--scene", type=pathlib.Path, required=True, help="the scene whose probe grid is drawn")
    parser.add_argument("--grid", help="an SHPR file in place of the scene's locked bake")
    args = parser.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    name = scene_id(args.scene)
    shpr = grid_path(args.grid, args.scene)
    with tempfile.TemporaryDirectory() as work:
        probe = build_probe(args.cc, SOURCES, work)
        example = probe_json(probe)
        sphere_plot(example, args.out)
        blend_plot(example, args.out)
        grid = probe_json(probe, shpr)
        flat, middle = scene_settings(args.scene)
        grid_plot(grid, middle, name, args.out)
        points = floor_points(grid)
        spheres = probe_json(probe, shpr, *(",".join(f"{v:g}" for v in point) for _, point in points))
        spheres_plot(spheres, flat, middle, points, name, args.out)


if __name__ == "__main__":
    main()
