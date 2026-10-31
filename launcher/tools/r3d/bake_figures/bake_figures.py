"""The figures of docs/render/offline/Bake-Method.md, from the bake's and the scoring's own functions on small scenes.

Local occlusion: the rays of one floor point at the foot of a wall (r3d.light's directions, Mitsuba's hits) and
the factor `local_occlusion` gives along the floor. Bounced light: the per-ray light `PathLight` gathers for that
point, and its bounced light along the floor for one to three bounces and against the ray count. The scores: one
test picture under three changes, scored by render_compare.py's own ΔE, SSIM and edge mask.

    python launcher/tools/r3d/bake_figures/bake_figures.py --out DIR

Needs launcher/tools/r3d/requirements.txt. launcher/tools/render/render_doc_images.sh runs it.
"""

import argparse
import math
import pathlib
import sys
from types import SimpleNamespace

import numpy as np
from matplotlib import colormaps
from matplotlib.cm import ScalarMappable
from matplotlib.colors import Normalize
from PIL import Image, ImageFilter

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "render"))
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, figure, save, subplots  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Line3DCollection, Poly3DCollection  # noqa: E402
from r3d import path_bake  # noqa: E402
from r3d.light import LIGHT_GAMMA, light, local_occlusion, sky_directions, tangent_frame  # noqa: E402
import render_compare  # noqa: E402
# The bounce tests' floor and wall; importing it traces one ray at a time, no libLLVM needed.
from tests import soup  # noqa: E402
from tests.soup import SUN, corridor, floor_points, path_light  # noqa: E402

RAY_OFFSET = 0.01
# The tests' corridor with a wall low enough that its bounced light falls off along the floor.
WALL_HEIGHT = 4.0
WALL_ALBEDO = (0.9, 0.5, 0.15)
CEILING = 20.0
# The drawn point, its distance from the wall, and the occlusion settings it is shown with.
POINT_X = 2.0
SHOWN_RAYS = 48
AO_DISTANCE = 6.0
AO_DISTANCES = (3.0, 6.0, 12.0)
AO_STRENGTH = 1.0
FLOOR_SAMPLES = 49
FLOOR_REACH = 16.0
# Bounced light: the counts swept, the ray count of the bounce sweep and the reference the noise is measured from.
BOUNCES = (1, 2, 3)
SWEEP_RAYS = 256
NOISE_RAYS = (4, 8, 16, 32, 64, 128, 256, 512)
NOISE_REFERENCE_RAYS = 4096
NOISE_POINTS = np.linspace(0.5, 8.0, 16)
# The 3D views: from VIEW_ELEVATION degrees up, turned so the wall stands nearly edge-on at the left and each ray's
# length to it shows.
VIEW_ELEVATION = 14
VIEW_AZIMUTH = -20
BOX = 8.0
BOX_ZOOM = 1.15
SUN_ARROW = 3.0
WEIGHT_MAP = "plasma_r"
PANEL = (3.4, 3.0)  # inches per panel
# The score figure: a test picture of SCORE_SIZE pixels, and the changes scored against it.
SCORE_SIZE = 96
BRIGHTNESS_STEP = 18
BLUR_RADIUS = 2
FLAT_BLOCK = 8
SCALE_ROWS = 6
# The test picture, in picture widths: a floor lit brighter toward the bottom, a ball lit from the upper left, and
# its shadow on the floor.
FLOOR_COLOUR = (0.55, 0.5, 0.42)
FLOOR_TOP_LIGHT = 0.6
BALL_CENTRE = (0.45, 0.45)
BALL_RADIUS = 0.27
BALL_COLOUR = (0.85, 0.3, 0.2)
BALL_LIGHT = (-0.5, -0.6, 0.6)
BALL_AMBIENT = 0.05
SHADOW_CENTRE = (0.62, 0.8)
SHADOW_RADII = (0.3, 0.09)
SHADOW_LIGHT = 0.45
DIRECTION_COUNTS = (8, 32, 128)


def source():
    """The corridor as the bake's source mesh, and its ray queries."""
    mesh = corridor(wall_kd=tuple(value ** (1 / LIGHT_GAMMA) for value in WALL_ALBEDO), wall_height=WALL_HEIGHT)
    return mesh, soup.rays(soup.Soup(mesh.p, mesh.tri_v))


def occlusion_settings(distance, rays):
    return SimpleNamespace(distance=distance, rays=rays, strength=AO_STRENGTH, indirect=False)


def point_rays(rays):
    """The directions the bake gives the drawn point: `sky_directions` in its own tangent frame."""
    _, normals = floor_points([POINT_X])
    tu, tv = tangent_frame(normals)
    local = sky_directions(rays)
    return local[:, 0:1] * tu + local[:, 1:2] * tv + local[:, 2:3] * normals


def occlusion_data(rays):
    """Each ray of the drawn point: where it stops (the hit, or AO_DISTANCE out) and its weight 1 - t / D."""
    _, query = source()
    origin = floor_points([POINT_X])[0] + np.array([0.0, RAY_OFFSET, 0.0])
    directions = point_rays(SHOWN_RAYS)
    hit, distance, _ = query.first_hits(np.repeat(origin, len(directions), axis=0), directions)
    reach = np.where(hit, np.minimum(distance, AO_DISTANCE), AO_DISTANCE)
    weight = np.where(hit, np.clip(1.0 - distance / AO_DISTANCE, 0.0, 1.0), 0.0)
    factor = local_occlusion(*floor_points([POINT_X]), query, occlusion_settings(AO_DISTANCE, SHOWN_RAYS),
                             RAY_OFFSET)[0]
    # The drawn weights are the factor's own: f = 1 - s * mean(w).
    assert np.isclose(factor, 1.0 - AO_STRENGTH * weight.mean())
    xs = np.linspace(0.0, FLOOR_REACH, FLOOR_SAMPLES)[1:]
    # The weight curve: one ray straight at a ceiling, the point moved to distance t below it, so 1 - f is that ray's w.
    big = soup.BIG
    ceiling = soup.rays(soup.Soup([[-big, CEILING, -big], [big, CEILING, -big], [big, CEILING, big],
                                   [-big, CEILING, big]], [[0, 1, 2], [0, 2, 3]]))
    up = sky_directions(1)[0][2]
    t = np.linspace(0.0, max(AO_DISTANCES) * 1.25, FLOOR_SAMPLES)[1:]
    weights = {d: 1.0 - local_occlusion(*floor_points(np.zeros(len(t)), CEILING - t * up), ceiling,
                                         occlusion_settings(d, 1), 0.0) for d in AO_DISTANCES}
    along = {d: local_occlusion(*floor_points(xs), query, occlusion_settings(d, rays), RAY_OFFSET) for d in AO_DISTANCES}
    return SimpleNamespace(origin=origin[0], directions=directions, reach=reach, weight=weight, factor=factor,
                           xs=xs, along=along, t=t, weights=weights)


def draw_corridor(axis):
    """The floor and the wall, cut to the drawn box (x and y from 0 to BOX, z from -BOX / 2 to BOX / 2)."""
    half = BOX / 2
    floor = [[0, 0, -half], [BOX, 0, -half], [BOX, 0, half], [0, 0, half]]
    wall = [[0, 0, -half], [0, 0, half], [0, WALL_HEIGHT, half], [0, WALL_HEIGHT, -half]]
    wall_colour = tuple(value ** (1 / LIGHT_GAMMA) for value in WALL_ALBEDO)
    axis.add_collection3d(Poly3DCollection([floor], facecolor=FAINT, alpha=0.35, edgecolor=MUTED))
    axis.add_collection3d(Poly3DCollection([wall], facecolor=wall_colour, alpha=0.3, edgecolor=MUTED))
    axis.set(xlim=(0, BOX), ylim=(0, BOX), zlim=(-half, half))
    axis.set_box_aspect((1, 1, 1), zoom=BOX_ZOOM)
    axis.set_axis_off()


def draw_rays(axis, origin, directions, reach, colours, shown):
    """Each ray as a segment from `origin`, cut at the drawn box; `shown` rays solid in `colours`, the rest dotted."""
    half = BOX / 2
    low, high = np.array([0.0, 0.0, -half]), np.array([BOX, BOX, half])
    with np.errstate(divide="ignore"):
        exits = np.where(directions > 0, (high - origin) / directions, (low - origin) / directions)
    reach = np.minimum(reach, np.where(directions != 0, exits, np.inf).min(axis=1))
    ends = origin + directions * reach[:, None]
    axis.add_collection3d(Line3DCollection([(origin, end) for end in ends[~shown]], colors=MUTED, linewidths=0.6,
                                           linestyles="dotted"))
    axis.add_collection3d(Line3DCollection([(origin, end) for end in ends[shown]], colors=colours, linewidths=1.8))
    axis.scatter(*ends[shown].T, color=colours, s=10, depthshade=False)
    axis.scatter(*origin, color=INK, s=24, depthshade=False)


def still(fig, axis, path, title):
    axis.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH, vertical_axis="y")
    axis.set_title(title, fontsize=TITLE_SIZE)
    save(fig, path)


def occlusion_rays_figure(data, path):
    fig = figure((PANEL[0] * 1.3, PANEL[1] * 1.4))
    axis = fig.add_subplot(projection="3d")
    draw_corridor(axis)
    hits = data.weight > 0
    weight_map = colormaps[WEIGHT_MAP]
    draw_rays(axis, data.origin, data.directions, data.reach, weight_map(data.weight[hits]), hits)
    bar = fig.colorbar(ScalarMappable(Normalize(0, 1), weight_map), ax=axis, orientation="horizontal", shrink=0.6,
                       pad=0.02)
    bar.set_label("hit weight w = 1 - t / D (dotted: no hit within D)", fontsize=LABEL_SIZE)
    bar.ax.tick_params(labelsize=LABEL_SIZE)
    title = f"{len(data.directions)} rays, reach D = {AO_DISTANCE:g}: f = {data.factor:.2f}"
    still(fig, axis, path, title)


def occlusion_falloff(data, path):
    fig, (weight_axis, floor_axis) = subplots(1, 2, (PANEL[0] * 2, PANEL[1]))
    for d, colour in zip(AO_DISTANCES, SLOTS):
        weight_axis.plot(data.t, data.weights[d], color=colour, label=f"D = {d:g}")
        floor_axis.plot(data.xs, data.along[d], color=colour, label=f"D = {d:g}")
    weight_axis.set(xlabel="hit distance t", ylabel="ray weight w", ylim=(0, 1.05), xlim=(0, data.t[-1]))
    weight_axis.set_title("A hit's weight against its distance", fontsize=TITLE_SIZE)
    floor_axis.axvline(POINT_X, color=FAINT, linestyle="--")
    floor_axis.set(xlabel="distance from the wall", ylabel="occlusion factor f", ylim=(0, 1.05), xlim=(0, data.xs[-1]))
    floor_axis.set_title("f along the floor beside the wall", fontsize=TITLE_SIZE)
    weight_axis.legend(fontsize=LABEL_SIZE, loc="upper right")
    floor_axis.legend(fontsize=LABEL_SIZE, loc="lower right")
    for axis in (weight_axis, floor_axis):
        axis.tick_params(labelsize=LABEL_SIZE)
    save(fig, path)


def directions_figure(path):
    """The fixed directions seen from above (cosine-weighted: even on the disc) for three prefix lengths."""
    counts = DIRECTION_COUNTS
    fig, axes = subplots(1, len(counts), (PANEL[0] * len(counts) * 0.75, PANEL[1] * 0.85))
    circle = np.linspace(0, 2 * math.pi, 200)
    for axis, count in zip(axes, counts):
        d = sky_directions(count)
        axis.plot(np.cos(circle), np.sin(circle), color=MUTED, linewidth=0.8)
        axis.scatter(d[:, 0], d[:, 1], color=SLOTS[0], s=12)
        axis.set_aspect("equal")
        axis.set(xlim=(-1.1, 1.1), ylim=(-1.1, 1.1), xticks=[], yticks=[])
        axis.set_title(f"R = {count}, seen from above", fontsize=TITLE_SIZE)
    save(fig, path)


def bounce_data():
    mesh, query = source()
    bounced = lambda bounces, rays: path_light(source()[0], bounces=bounces, rays=rays)  # noqa: E731
    gathered = bounced(BOUNCES[-1], SHOWN_RAYS)
    point, normal = floor_points([POINT_X])
    origin = point + normal * RAY_OFFSET
    directions = point_rays(SHOWN_RAYS)
    per_ray = gathered._radiance(np.repeat(origin, SHOWN_RAYS, axis=0), directions, path_bake.SEED)
    # The drawn rays are bounce()'s own: its first chunk is seeded SEED as well.
    assert np.allclose(per_ray.mean(axis=0), gathered.bounce(point, normal, RAY_OFFSET)[0])
    hit, distance, _ = query.first_hits(np.repeat(origin, SHOWN_RAYS, axis=0), directions)
    xs = np.linspace(0.0, FLOOR_REACH, FLOOR_SAMPLES // 2)[1:]
    points, normals = floor_points(xs)
    direct = light(points, normals, np.zeros(len(xs), bool), query, [SUN], RAY_OFFSET)
    sweep = {bounces: bounced(bounces, SWEEP_RAYS).bounce(points, normals, RAY_OFFSET) for bounces in BOUNCES}
    noise_points, noise_normals = floor_points(NOISE_POINTS)
    reference = bounced(BOUNCES[0], NOISE_REFERENCE_RAYS).bounce(noise_points, noise_normals, RAY_OFFSET)
    noise = []
    for rays in NOISE_RAYS:
        got = bounced(BOUNCES[0], rays).bounce(noise_points, noise_normals, RAY_OFFSET)
        noise.append(float(np.mean(np.abs(got - reference).sum(axis=1) / reference.sum(axis=1))))
    return SimpleNamespace(origin=origin[0], directions=directions, per_ray=per_ray, hit=hit, distance=distance,
                           xs=xs, direct=direct, sweep=sweep, noise=noise)


def bounce_rays_figure(data, path):
    fig = figure((PANEL[0] * 1.3, PANEL[1] * 1.3))
    axis = fig.add_subplot(projection="3d")
    draw_corridor(axis)
    lit = data.per_ray.sum(axis=1) > 0
    shade = np.clip(data.per_ray / max(data.per_ray.max(), 1e-9), 0.0, 1.0) ** (1 / LIGHT_GAMMA)
    draw_rays(axis, data.origin, data.directions, np.where(data.hit, data.distance, np.inf), shade[lit], lit)
    sun = np.array(SUN["direction"]) / np.linalg.norm(SUN["direction"])
    tip = np.array([BOX * 0.7, WALL_HEIGHT, 0.0])
    axis.quiver(*(tip + sun * SUN_ARROW), *(-sun * SUN_ARROW), color=SLOTS[3], linewidth=2, arrow_length_ratio=0.25)
    axis.text(*(tip + sun * SUN_ARROW), "sun", color=SLOTS[3], fontsize=LABEL_SIZE)
    mean = data.per_ray.mean(axis=0)
    title = (f"{len(data.directions)} rays, each coloured by the light its path gathers\n"
             f"(dotted: nothing). Mean r, g, b = {mean[0]:.2f}, {mean[1]:.2f}, {mean[2]:.2f}")
    still(fig, axis, path, title)


def bounce_figure(data, path):
    fig, (sweep_axis, noise_axis) = subplots(1, 2, (PANEL[0] * 2, PANEL[1]))
    previous = np.zeros(len(data.xs))
    for bounces, colour, name in zip(BOUNCES, SLOTS, ("first", "second", "third")):
        reached = data.sweep[bounces].mean(axis=1)
        sweep_axis.plot(data.xs, reached - previous, color=colour, label=f"{name} bounce")
        previous = reached
    sweep_axis.set(xlabel="distance from the wall", ylabel="light / pi it adds (mean of r, g, b)",
                   xlim=(0, data.xs[-1]))
    sweep_axis.set_ylim(bottom=0)
    sweep_axis.set_title(f"Each bounce adds less than the one before\n(direct light here: "
                         f"{data.direct.mean():.2f})", fontsize=TITLE_SIZE)
    sweep_axis.legend(fontsize=LABEL_SIZE, loc="upper right")
    rays = np.array(NOISE_RAYS)
    fit = data.noise[0] * math.sqrt(rays[0]) / np.sqrt(rays)
    noise_axis.plot(rays, data.noise, color=SLOTS[0], marker="o", label="measured")
    noise_axis.plot(rays, fit, color=MUTED, linestyle="--", label="1 / sqrt(R)")
    noise_axis.set_xscale("log", base=2)
    noise_axis.set(xlabel="rays per point R", ylabel=f"relative error vs {NOISE_REFERENCE_RAYS} rays")
    noise_axis.set_ylim(bottom=0)
    noise_axis.set_title("Noise falls as 1 / sqrt(R)", fontsize=TITLE_SIZE)
    noise_axis.legend(fontsize=LABEL_SIZE, loc="upper right")
    for axis in (sweep_axis, noise_axis):
        axis.tick_params(labelsize=LABEL_SIZE)
    save(fig, path)


def test_picture():
    """A lit ball on a floor with a hard shadow: smooth shading, a silhouette and a shadow edge."""
    y, x = np.mgrid[0:SCORE_SIZE, 0:SCORE_SIZE] / SCORE_SIZE
    image = np.zeros((SCORE_SIZE, SCORE_SIZE, 3))
    image[:] = np.array(FLOOR_COLOUR) * (FLOOR_TOP_LIGHT + (1 - FLOOR_TOP_LIGHT) * y[..., None])
    shadow = ((x - SHADOW_CENTRE[0]) / SHADOW_RADII[0]) ** 2 + ((y - SHADOW_CENTRE[1]) / SHADOW_RADII[1]) ** 2 < 1
    image[shadow] *= SHADOW_LIGHT
    nx, ny = (x - BALL_CENTRE[0]) / BALL_RADIUS, (y - BALL_CENTRE[1]) / BALL_RADIUS
    ball = nx ** 2 + ny ** 2 < 1
    nz = np.sqrt(np.clip(1 - nx ** 2 - ny ** 2, 0, 1))
    shade = np.clip(nx * BALL_LIGHT[0] + ny * BALL_LIGHT[1] + nz * BALL_LIGHT[2], BALL_AMBIENT, 1.0)
    image[ball] = (np.array(BALL_COLOUR) * shade[..., None])[ball]
    return Image.fromarray(np.round(image * 255).astype(np.uint8))


def flat_blocks(picture):
    """One colour per FLAT_BLOCK square: the look of a mesh with too few vertices."""
    small = picture.resize((SCORE_SIZE // FLAT_BLOCK,) * 2, Image.Resampling.BOX)
    return small.resize(picture.size, Image.Resampling.NEAREST)


def scores_figure(path):
    reference = test_picture()
    brighter = Image.fromarray(np.clip(np.asarray(reference, int) + BRIGHTNESS_STEP, 0, 255).astype(np.uint8))
    renders = (("brighter", brighter), ("blurred", reference.filter(ImageFilter.BoxBlur(BLUR_RADIUS))),
               ("blocky", flat_blocks(reference)))
    fig, axes = subplots(3, len(renders) + 1, (PANEL[0] * 2.4, PANEL[1] * 2.1))
    axes[0][0].imshow(reference)
    axes[0][0].set_title("reference", fontsize=TITLE_SIZE)
    axes[1][0].imshow(render_compare.edge_overlay(reference))
    axes[1][0].set_title("edge pixels", fontsize=TITLE_SIZE)
    scale = np.tile(np.linspace(0, render_compare.HEAT_FULL_SCALE, SCORE_SIZE), (SCALE_ROWS, 1))
    axes[2][0].imshow(render_compare.reference_heatmap_from_error(scale),
                      extent=(0, render_compare.HEAT_FULL_SCALE, 0, SCALE_ROWS))
    axes[2][0].set_title("heatmap colour per dE", fontsize=TITLE_SIZE)
    for column, (label, render) in enumerate(renders, start=1):
        stats = render_compare.reference_measure(render, reference)
        axes[0][column].imshow(render)
        axes[0][column].set_title(label, fontsize=TITLE_SIZE)
        axes[1][column].imshow(render_compare.reference_heatmap(render, reference))
        axes[1][column].set_title("dE76 heatmap", fontsize=TITLE_SIZE)
        rows = (("mean dE", f"{stats.mean_delta_e:.1f}"), ("p95 dE", f"{stats.p95_delta_e:.1f}"),
                ("luma SSIM", f"{stats.ssim_luma:.3f}"), ("edge dE", f"{stats.edge_delta_e:.1f}"),
                ("interior dE", f"{stats.interior_delta_e:.1f}"))
        axes[2][column].table(cellText=rows, loc="center", cellLoc="left", edges="horizontal").set_fontsize(LABEL_SIZE)
    for axis in np.ravel(axes):
        if axis is not axes[2][0]:
            axis.set_axis_off()
    axes[2][0].set_yticks([])
    axes[2][0].tick_params(labelsize=LABEL_SIZE)
    save(fig, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=pathlib.Path, required=True, help="the folder the figures go to")
    args = parser.parse_args()
    occlusion = occlusion_data(SHOWN_RAYS)
    occlusion_rays_figure(occlusion, args.out / "bake-ao-rays.png")
    occlusion_falloff(occlusion, args.out / "bake-ao-falloff.png")
    directions_figure(args.out / "bake-directions.png")
    bounce = bounce_data()
    bounce_rays_figure(bounce, args.out / "bake-bounce-rays.png")
    bounce_figure(bounce, args.out / "bake-bounce-sweep.png")
    scores_figure(args.out / "bake-scores.png")


if __name__ == "__main__":
    main()
