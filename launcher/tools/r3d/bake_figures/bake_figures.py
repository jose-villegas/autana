"""The figures of docs/render/Bake-Method.md, from the bake's and the scoring's own functions on small scenes.

Local occlusion: the rays of one floor point at the foot of a wall (r3d.light's directions, Mitsuba's hits) and
the factor `local_occlusion` gives along the floor. Bounced light: the per-ray light `PathLight` gathers for that
point, and its bounced light along the floor for one to three bounces and against the ray count. The scores: one
test picture under three changes, scored by render_compare.py's own ΔE, SSIM and edge mask.

    python launcher/tools/r3d/bake_figures/bake_figures.py --out DIR

Needs launcher/tools/r3d/requirements.txt and matplotlib. launcher/tools/render/render_doc_images.sh runs it.
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
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, figure, save, save_gif, subplots  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Line3DCollection, Poly3DCollection  # noqa: E402
from r3d import ray_query  # noqa: E402
from r3d.geometry import corner_normals  # noqa: E402
from r3d.light import light, local_occlusion, sky_directions, tangent_frame  # noqa: E402
from r3d.path_bake import PathLight  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
import render_compare  # noqa: E402

# One ray at a time, as the bake's tests trace: no libLLVM, and the same bytes on every machine.
VARIANT = "scalar_rgb"
RAY_OFFSET = 0.01
# The scene, in bake units: a floor (y = 0) and a wall (x = 0) facing it, both much larger than anything drawn.
EXTENT = 400.0
WALL_HEIGHT = 4.0
CEILING = 20.0
WALL_ALBEDO = (0.9, 0.5, 0.15)
FLOOR_ALBEDO = 0.5
DISPLAY_GAMMA = 2.2
SUN = {"type": "directional", "direction": [0.6, 1.0, 0.25], "color": [1.0, 1.0, 1.0], "intensity": 2.0}
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
# The 3D views: an orbit of ORBIT_FRAMES frames at ORBIT_FPS, from VIEW_ELEVATION degrees up.
ORBIT_FRAMES = 36
ORBIT_FPS = 12
FULL_TURN = 360
VIEW_ELEVATION = 22
VIEW_AZIMUTH = -60
BOX = 8.0
BOX_ZOOM = 1.15
SUN_ARROW = 3.0
WEIGHT_MAP = "plasma_r"
PANEL = (3.4, 3.0)  # inches per panel
GIF_COLORS = 64
GIF_DPI = 80
# The score figure: a test picture of SCORE_SIZE pixels, and the changes scored against it.
SCORE_SIZE = 96
BRIGHTNESS_STEP = 18
BLUR_RADIUS = 2
FLAT_BLOCK = 8
SCALE_ROWS = 6


def corridor_source():
    """The floor (material 0) and the wall facing it (material 1), as the bake's source mesh and its ray queries."""
    floor = [[-EXTENT, 0, EXTENT], [EXTENT, 0, EXTENT], [EXTENT, 0, -EXTENT], [-EXTENT, 0, -EXTENT]]
    wall = [[0, WALL_HEIGHT, -EXTENT], [0, WALL_HEIGHT, EXTENT], [0, 0, EXTENT], [0, 0, -EXTENT]]
    p = np.array(floor + wall, dtype=float)
    tri_v = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
    # Only the floor's +x half is drawn and lit: the wall stands on it.
    p[[0, 3], 0] = 0.0
    kd = lambda albedo: tuple(value ** (1 / DISPLAY_GAMMA) for value in albedo)  # noqa: E731
    source = SimpleNamespace(colors=None, p=p, uv=np.zeros((len(p), 2)), tri_v=tri_v, tri_t=tri_v,
                             tri_m=np.array([0, 0, 1, 1]), names=["floor", "wall"],
                             materials={"floor": {"Kd": kd((FLOOR_ALBEDO,) * 3)}, "wall": {"Kd": kd(WALL_ALBEDO)}},
                             textures=[None, None])
    source.corner_normals = corner_normals(p, tri_v)
    return source, RayQuery(p, tri_v, VARIANT)


def floor_points(xs, height=0.0):
    xs = np.asarray(xs, dtype=float)
    points = np.stack([xs, np.broadcast_to(height, xs.shape), np.zeros_like(xs)], axis=1)
    return points, np.tile([0.0, 1.0, 0.0], (len(points), 1))


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
    source, query = corridor_source()
    origin = floor_points([POINT_X])[0] + np.array([0.0, RAY_OFFSET, 0.0])
    directions = point_rays(SHOWN_RAYS)
    hit, distance, _ = query.first_hits(np.repeat(origin, len(directions), axis=0), directions)
    reach = np.where(hit, np.minimum(distance, AO_DISTANCE), AO_DISTANCE)
    weight = np.where(hit, np.clip(1.0 - distance / AO_DISTANCE, 0.0, 1.0), 0.0)
    factor = local_occlusion(*floor_points([POINT_X]), query, occlusion_settings(AO_DISTANCE, SHOWN_RAYS),
                             RAY_OFFSET)[0]
    xs = np.linspace(0.0, FLOOR_REACH, FLOOR_SAMPLES)[1:]
    # One ray straight at a ceiling: its weight is 1 - f, at the distance the ray travels to it.
    ceiling = RayQuery(np.array([[-EXTENT, CEILING, -EXTENT], [EXTENT, CEILING, -EXTENT], [EXTENT, CEILING, EXTENT],
                                 [-EXTENT, CEILING, EXTENT]]), np.array([[0, 1, 2], [0, 2, 3]]), VARIANT)
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
    wall_colour = tuple(value ** (1 / DISPLAY_GAMMA) for value in WALL_ALBEDO)
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


def orbit(fig, axis, path, title):
    def draw(frame):
        axis.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH + FULL_TURN * frame / ORBIT_FRAMES, vertical_axis="y")
        axis.set_title(title, fontsize=TITLE_SIZE)

    draw(0)
    fig.set_dpi(GIF_DPI)
    save_gif(fig, draw, ORBIT_FRAMES, path, ORBIT_FPS, colors=GIF_COLORS)


def occlusion_rays_gif(data, path):
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
    orbit(fig, axis, path, title)


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
    counts = (8, 32, 128)
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
    source, query = corridor_source()
    lights = [SUN]
    gathered = PathLight(source, lights, set(), SimpleNamespace(bounces=BOUNCES[-1], rays=SHOWN_RAYS), 1.0,
                         variant=VARIANT)
    origin = floor_points([POINT_X])[0] + np.array([0.0, RAY_OFFSET, 0.0])
    directions = point_rays(SHOWN_RAYS)
    per_ray = gathered._radiance(np.repeat(origin, SHOWN_RAYS, axis=0), directions, 0)
    hit, distance, _ = query.first_hits(np.repeat(origin, SHOWN_RAYS, axis=0), directions)
    xs = np.linspace(0.0, FLOOR_REACH, FLOOR_SAMPLES // 2)[1:]
    points, normals = floor_points(xs)
    direct = light(points, normals, np.zeros(len(xs), bool), query, lights, RAY_OFFSET)
    sweep = {}
    for bounces in BOUNCES:
        path = PathLight(corridor_source()[0], lights, set(), SimpleNamespace(bounces=bounces, rays=SWEEP_RAYS), 1.0,
                         variant=VARIANT)
        sweep[bounces] = path.bounce(points, normals, RAY_OFFSET)
    noise_points, noise_normals = floor_points(NOISE_POINTS)
    reference = PathLight(corridor_source()[0], lights, set(),
                          SimpleNamespace(bounces=BOUNCES[0], rays=NOISE_REFERENCE_RAYS), 1.0,
                          variant=VARIANT).bounce(noise_points, noise_normals, RAY_OFFSET)
    noise = []
    for rays in NOISE_RAYS:
        got = PathLight(corridor_source()[0], lights, set(), SimpleNamespace(bounces=BOUNCES[0], rays=rays), 1.0,
                        variant=VARIANT).bounce(noise_points, noise_normals, RAY_OFFSET)
        noise.append(float(np.mean(np.abs(got - reference).sum(axis=1) / reference.sum(axis=1))))
    return SimpleNamespace(origin=origin[0], directions=directions, per_ray=per_ray, hit=hit, distance=distance,
                           xs=xs, direct=direct, sweep=sweep, noise=noise)


def bounce_rays_gif(data, path):
    fig = figure((PANEL[0] * 1.3, PANEL[1] * 1.3))
    axis = fig.add_subplot(projection="3d")
    draw_corridor(axis)
    lit = data.per_ray.sum(axis=1) > 0
    shade = np.clip(data.per_ray / max(data.per_ray.max(), 1e-9), 0.0, 1.0) ** (1 / DISPLAY_GAMMA)
    draw_rays(axis, data.origin, data.directions, np.where(data.hit, data.distance, np.inf), shade[lit], lit)
    sun = np.array(SUN["direction"]) / np.linalg.norm(SUN["direction"])
    tip = np.array([BOX * 0.7, WALL_HEIGHT, 0.0])
    axis.quiver(*(tip + sun * SUN_ARROW), *(-sun * SUN_ARROW), color=SLOTS[3], linewidth=2, arrow_length_ratio=0.25)
    axis.text(*(tip + sun * SUN_ARROW), "sun", color=SLOTS[3], fontsize=LABEL_SIZE)
    mean = data.per_ray.mean(axis=0)
    title = (f"{len(data.directions)} rays, each coloured by the light its path gathers\n"
             f"(dotted: nothing). Mean r, g, b = {mean[0]:.2f}, {mean[1]:.2f}, {mean[2]:.2f}")
    orbit(fig, axis, path, title)


def bounce_figure(data, path):
    fig, (sweep_axis, noise_axis) = subplots(1, 2, (PANEL[0] * 2, PANEL[1]))
    total = lambda rgb: rgb.mean(axis=1)  # noqa: E731
    sweep_axis.plot(data.xs, total(data.direct), color=INK, linestyle="--", label="direct")
    for bounces, colour in zip(BOUNCES, SLOTS):
        sweep_axis.plot(data.xs, total(data.sweep[bounces]), color=colour, label=f"K = {bounces}")
    sweep_axis.set(xlabel="distance from the wall", ylabel="light / pi, mean of r, g, b", xlim=(0, data.xs[-1]))
    sweep_axis.set_ylim(bottom=0)
    sweep_axis.set_title("Each bounce adds less than the one before", fontsize=TITLE_SIZE)
    sweep_axis.legend(fontsize=LABEL_SIZE, loc="upper right")
    rays = np.array(NOISE_RAYS)
    fit = data.noise[0] * math.sqrt(rays[0]) / np.sqrt(rays)
    noise_axis.plot(rays, data.noise, color=SLOTS[0], marker="o", label="measured")
    noise_axis.plot(rays, fit, color=MUTED, linestyle="--", label="1 / sqrt(R)")
    noise_axis.set_xscale("log", base=2)
    noise_axis.set(xlabel="rays per point R", ylabel=f"error vs {NOISE_REFERENCE_RAYS} rays", ylim=(0, None))
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
    image[:] = (0.55, 0.5, 0.42) * (0.6 + 0.4 * y[..., None])
    shadow = ((x - 0.62) / 0.3) ** 2 + ((y - 0.8) / 0.09) ** 2 < 1
    image[shadow] *= 0.45
    r = np.hypot(x - 0.45, y - 0.45)
    ball = r < 0.27
    nz = np.sqrt(np.clip(1 - (r / 0.27) ** 2, 0, 1))
    shade = np.clip(((x - 0.45) * -0.5 + (y - 0.45) * -0.6) / 0.27 + nz * 0.6, 0.05, 1.0)
    image[ball] = (np.array([0.85, 0.3, 0.2]) * shade[..., None])[ball]
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
    ray_query.VARIANT = VARIANT
    occlusion = occlusion_data(SHOWN_RAYS)
    occlusion_rays_gif(occlusion, args.out / "bake-ao-rays.gif")
    occlusion_falloff(occlusion, args.out / "bake-ao-falloff.png")
    directions_figure(args.out / "bake-directions.png")
    bounce = bounce_data()
    bounce_rays_gif(bounce, args.out / "bake-bounce-rays.gif")
    bounce_figure(bounce, args.out / "bake-bounce-sweep.png")
    scores_figure(args.out / "bake-scores.png")


if __name__ == "__main__":
    main()
