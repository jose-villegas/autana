#!/usr/bin/env python3
"""Where a fit's probes (probes.py) sit in its scene, and what became of each. The source is drawn see-through as
the engine's `cull` view draws a culled cluster (render/view/raster_cull_view.c), with the camera path and the
probes on top, each probe in its fate's colour (FATES); a kept probe also shows its kept cube faces as arrows.

    python launcher/tools/r3d/views/probe_figures.py --scene SCENE.scene.toml OBJECT [--scene ...] --out DIR

Per scene object: DIR/SCENE-OBJECT-probes-orbit.gif, ORBIT_FRAMES views circling the probe region uncut, and
DIR/SCENE-OBJECT-probes-1.png .. -4.png from above its four top corners, each cut through the region's centre with
only the probes behind the cut drawn. Then DIR/probe-fates.png, one legend for them all: each fate's count per
scene object. probes.select picks the probes as a fit's reference set does (reference_set.probe_poses): beside
the camera path's coverage poses, at the import's geometry.probes or, for an import that sets none, at their
defaults.
"""

import argparse
import dataclasses
import pathlib
import sys
from types import SimpleNamespace
from typing import NamedTuple

import numpy as np
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define  # noqa: E402
from docs.figures import rgb, save_gif, save_png  # noqa: E402
from docs.plot_style import DPI, LABEL_SIZE, MUTED, SLOTS, figure, save  # noqa: E402
from r3d.fitted_variant import placed_variant  # noqa: E402
from r3d.geometry import face_normals, hit_barycentrics  # noqa: E402
from r3d.import_settings import load_probes, load_scene  # noqa: E402
from r3d.light import AlphaTest, encode_srgb8  # noqa: E402
from r3d.mesh_source import camera_path_poses, load_source  # noqa: E402
from r3d.poses import camera_basis, camera_rays  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.reference_render import hit_albedo  # noqa: E402
from r3d.reference_set import PROBE_SEED, reference_inputs  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402
from r3d.views import probes  # noqa: E402


CULL_VIEW = ROOT / "launcher/main/render/view/raster_cull_view.c"
DOOR = read_define(CULL_VIEW, "CULLED_DOOR")
DASH = read_define(CULL_VIEW, "CULLED_DASH")
# Each still's size in pixels, large enough to stay legible at the fifth of a page a doc table row gives it.
VIEW_SIZE = (640, 400)
# Share of the surface colour every covered pixel takes over the background, so surfaces read between door pixels.
WASH = 0.4
# The doc figures' white page, and the theme's muted grey for the dashed borders.
BACKGROUND = rgb("#ffffff")
BORDER = rgb(MUTED)
# A kept face's arrow, as a share of the probe spacing.
ARROW_SPACING = 0.4
# Probe marks in pixels: a kept probe's disc radius, every other fate's mark radius, and line widths.
KEPT_RADIUS = 3
MARK_RADIUS = 2
LINE_WIDTH = 1
PATH_WIDTH = 2
# Share of each half view the framed box stays inside, the halvings that find that distance, and the points a side
# the box is sampled at to frame it.
FRAME_MARGIN = 0.04
FRAME_STEPS = 24
FRAME_SAMPLES = 9
PATH_COLOUR = SLOTS[0]
# The orbit: its views, each one's time on screen, and their height above the region's centre in degrees, enough
# to look over a roof into the street behind it.
ORBIT_FRAMES = 48
ORBIT_FRAME_MS = 125
# The orbit's size in pixels: half a still's, to keep the GIF small.
ORBIT_SIZE = (320, 200)
ORBIT_ELEVATION = 40.0
# The legend spans two stills, one key per line; a line's height in inches.
LEGEND_LINE = 0.2


@dataclasses.dataclass(frozen=True)
class Fate:
    colour: str
    label: str
    cross: bool  # dropped for where it sits (a cross), not for what it shows (a disc)


# What became of a probe, in legend order: one row per fate placed gives.
FATES = {
    "kept": Fate(SLOTS[2], "kept (arrows: its kept faces)", False),
    "greedy": Fate(MUTED, "dropped by the greedy pick: adds too little", False),
    "inside": Fate(SLOTS[7], "dropped: inside geometry", True),
    "near": Fate(SLOTS[3], "dropped: a surface too near", True),
}


def frame_lattice(lo, hi):
    """FRAME_SAMPLES points a side through the box lo..hi, its corners among them."""
    steps = np.linspace(0, 1, FRAME_SAMPLES)
    return lo + np.stack(np.meshgrid(steps, steps, steps, indexing="ij"), axis=-1).reshape(-1, 3) * (hi - lo)


def distance_to_frame(points, target, toward, lens, radius, size=VIEW_SIZE):
    """How far along `toward` from `target` an eye looking back at it stands to keep `points`, which lie within
    `radius` of it, in view with FRAME_MARGIN to spare."""
    # far: twice the distance at which a sphere of `radius` just fills the view, so the points surely fit.
    near, far = 0.0, 2 * radius * np.sqrt(1 + 1 / lens ** 2)
    for _ in range(FRAME_STEPS):
        middle = (near + far) / 2
        xy, front = project(points, target + toward * middle, -toward, lens, size)
        fits = front.all() and (np.abs(xy / size * 2 - 1) <= 1 - FRAME_MARGIN).all()
        near, far = (near, middle) if fits else (middle, far)
    return far


def views(lo, hi, lens):
    """(centre, [(eye, forward)]): one view from above each of the box lo..hi's four top corners. Each is cut by the
    vertical plane through the centre facing it (cut_normal), looks at the middle of the box behind that cut and
    stands as near as keeps all of it in view."""
    lo, hi = np.asarray(lo, float), np.asarray(hi, float)
    centre, radius = (lo + hi) / 2, np.linalg.norm(hi - lo) / 2
    lattice = frame_lattice(lo, hi)
    result = []
    for x, z in ((hi[0], hi[2]), (lo[0], hi[2]), (lo[0], lo[2]), (hi[0], lo[2])):
        toward = np.array([x, hi[1], z]) - centre
        toward /= np.linalg.norm(toward)
        behind = lattice[(lattice - centre) @ cut_normal(-toward) >= 0]
        target = behind.mean(axis=0)
        result.append((target + toward * distance_to_frame(behind, target, toward, lens, radius), -toward))
    return centre, result


def orbit_views(lo, hi, lens):
    """ORBIT_FRAMES (eye, forward) circling the box lo..hi's centre ORBIT_ELEVATION degrees up, all at the one
    distance that keeps the whole box in every view."""
    lo, hi = np.asarray(lo, float), np.asarray(hi, float)
    centre, radius = (lo + hi) / 2, np.linalg.norm(hi - lo) / 2
    lattice, up = frame_lattice(lo, hi), np.radians(ORBIT_ELEVATION)
    towards = [np.array([np.cos(turn) * np.cos(up), np.sin(up), np.sin(turn) * np.cos(up)])
               for turn in 2 * np.pi * np.arange(ORBIT_FRAMES) / ORBIT_FRAMES]
    distance = max(distance_to_frame(lattice, centre, toward, lens, radius, ORBIT_SIZE) for toward in towards)
    return [(centre + toward * distance, -toward) for toward in towards]


def project(points, eye, forward, lens, size=VIEW_SIZE):
    """Pixel positions of `points` in the `size` view camera_rays casts, and whether each lies in front of the eye."""
    right, up, forward = camera_basis(forward)
    width, height = size
    short = min(width, height)
    offset = np.asarray(points, float) - eye
    depth = offset @ forward
    safe = np.where(depth > 0, depth, 1)
    x = ((offset @ right) / safe / (lens * width / short) + 1) * width / 2
    y = (1 - (offset @ up) / safe / (lens * height / short)) * height / 2
    return np.stack([x, y], axis=1), depth > 0


def cut_normal(forward):
    """The normal of the view's cut, a vertical plane facing the eye, pointing away from it."""
    normal = np.array([forward[0], 0.0, forward[2]])
    return normal / np.linalg.norm(normal)


def see_through(source, eye, forward, lens, cut=None, size=VIEW_SIZE):
    """The source drawn through its ray query (`source.intersector`) as the cull view draws a culled cluster; with a `cut` point, only what lies behind the cut
    through it: every ray starts where it crosses the cut."""
    width, height = size
    origin, direction = camera_rays(width, height, lens, eye, forward, 1)
    if cut is not None:
        normal = cut_normal(forward)
        along = direction @ normal
        to_plane = np.where(along > 0, ((cut - eye) @ normal) / np.where(along > 0, along, 1), 0)
        origin = origin + direction * np.maximum(to_plane, 0)[:, None]
    hit, distance, tri = source.intersector.first_hits(origin, direction)
    faces = tri[hit]
    locations = origin[hit] + direction[hit] * distance[hit][:, None]
    colour = np.zeros((len(origin), 3))
    colour[hit] = encode_srgb8(hit_albedo(source, faces, hit_barycentrics(source.p, source.tri_v, faces, locations)))
    part = np.full(len(origin), -1)
    part[hit] = source.tri_o[faces]
    colour, part, covered = colour.reshape(height, width, 3), part.reshape(height, width), hit.reshape(height, width)
    out = np.empty((height, width, 3))
    out[:] = BACKGROUND
    out[covered] = WASH * colour[covered] + (1 - WASH) * np.array(BACKGROUND)
    y, x = np.mgrid[:height, :width]
    door = covered & (x % DOOR == 0) & (y % DOOR == 0)
    out[door] = colour[door]
    edge = np.zeros_like(covered)
    edge[:, :-1] |= part[:, :-1] != part[:, 1:]
    edge[:-1, :] |= part[:-1, :] != part[1:, :]
    out[edge & (((x + y) // DASH) % 2 == 0)] = BORDER
    return Image.fromarray(np.round(out).astype(np.uint8))


def draw(image, found, path, eye, forward, lens, arrow, cut=None):
    """The camera path, then the probes (with a `cut` point, those behind the cut through it), farthest first."""
    canvas = ImageDraw.Draw(image)
    if path is not None:
        xy, front = project([pose[:3] for pose in path], eye, forward, lens, image.size)
        canvas.line([tuple(p) for p in xy[front]], fill=rgb(PATH_COLOUR), width=PATH_WIDTH)
    shown = [probe for probe in found if cut is None or (np.asarray(probe["at"]) - cut) @ cut_normal(forward) >= 0]
    for probe in sorted(shown, key=lambda probe: -np.linalg.norm(np.asarray(probe["at"]) - eye)):
        at, fate, faces = np.asarray(probe["at"]), FATES[probe["fate"]], probe["faces"]
        x, y = project([at], eye, forward, lens, image.size)[0][0]
        tips = (project([at + np.asarray(probes.FACES[face]) * arrow for face in faces], eye, forward, lens,
                        image.size)[0] if faces else [])
        for tip in tips:
            canvas.line([(x, y), tuple(tip)], fill=rgb(fate.colour), width=LINE_WIDTH)
        r = KEPT_RADIUS if faces else MARK_RADIUS
        if fate.cross:
            canvas.line([(x - r, y - r), (x + r, y + r)], fill=rgb(fate.colour), width=LINE_WIDTH)
            canvas.line([(x - r, y + r), (x + r, y - r)], fill=rgb(fate.colour), width=LINE_WIDTH)
        else:
            canvas.ellipse([x - r, y - r, x + r, y + r], fill=rgb(fate.colour))
    return image


def legend(probe_sets, out):
    """One key per fate, with its count in each of `probe_sets`, and the camera path; the title names each set's
    probe spacing."""
    from matplotlib.lines import Line2D

    handles = []
    for name, fate in FATES.items():
        counts = []
        for probe_set in probe_sets:
            count = sum(probe["fate"] == name for probe in probe_set.found)
            faces = f" ({probe_set.record['train']['kept']} faces)" if name == "kept" else ""
            counts.append(f"{probe_set.scene} {count}{faces}")
        handles.append(Line2D([], [], color=fate.colour, linestyle="none", marker="x" if fate.cross else "o",
                              label=f"{fate.label}: " + ", ".join(counts)))
    if any(probe_set.path is not None for probe_set in probe_sets):
        handles.append(Line2D([], [], color=PATH_COLOUR, label="camera path"))
    spacing = ", ".join(f"{probe_set.scene} {probe_set.record['spacing']:g}" for probe_set in probe_sets)
    fig = figure((2 * VIEW_SIZE[0] / DPI, LEGEND_LINE * (len(handles) + 1)))  # a line more for the title
    fig.legend(handles=handles, loc="center", ncol=1, frameon=False, fontsize=LABEL_SIZE,
               title=f"probe spacing, world units: {spacing}", title_fontsize=LABEL_SIZE)
    save(fig, out)


def placed(moved, state, picked):
    """Each probe as {"at": its moved position, "fate", "faces": its kept face indices into probes.FACES}: fate "kept" with
    a kept face, "greedy" for a probe outside geometry with none, else its classify state."""
    live = np.flatnonzero(state == "kept")
    faces = [[] for _ in moved]
    for index in sorted(picked):
        faces[live[index // len(probes.FACES)]].append(index % len(probes.FACES))
    return [{"at": [float(value) for value in at], "faces": shown,
             "fate": "kept" if shown else "greedy" if fate == "kept" else str(fate)}
            for at, fate, shown in zip(moved, state, faces)]


def placed_training(positions, tris, near, seed, views, query):
    """placed for the training probes of `views`, which probes.select made from these arguments (`near` resolved, not
    None): the probes are drawn and classified again, and each kept face is found among their cube poses."""
    positions, tris = np.asarray(positions, dtype=float), np.asarray(tris)
    points, cell = probes.probe_points(*views.record["region"], views.record["spacing"], seed)
    moved, state = probes.classify(query, face_normals(positions, tris)[0], points, cell, near)
    index = {pose.tobytes(): i for i, pose in enumerate(probes.cube_poses(moved[state == "kept"]))}
    return placed(moved, state, [index[np.asarray(pose, dtype=float).tobytes()] for pose in views.training])


class Probed(NamedTuple):
    """A scene object's probes as its reference set picks them, and its source to draw them over."""
    name: str  # SCENE-OBJECT, which its files are named after
    scene: str  # the scene's name in the legend
    record: dict  # probes.select's record
    found: list  # the training probes, as placed_training gives them
    path: list  # the camera path's coverage poses, or None without a path
    source: object
    lens: float  # the scene camera's


def probed(scene_path, object_name):
    scene = load_scene(scene_path)
    job = placed_variant(scene, object_name)
    inputs = reference_inputs(job, scene)
    if inputs.probes is None:
        inputs = dataclasses.replace(inputs, probes=load_probes({}))
    camera = scene.camera.component
    coverage = None
    if camera.path is not None:
        visibility = SimpleNamespace(size=inputs.size, every_ms=None)
        coverage = camera_path_poses(scene, visibility, inputs.poses["coverage_every_ms"])
    source = load_source(job.settings, np.float32)
    keep_alpha = job.settings.alpha_keep
    source.intersector = RayQuery(source.p, source.tri_v,
                                  hit_filter=None if keep_alpha is None else AlphaTest(source, keep_alpha))
    path = None if coverage is None else (*coverage[:3], coverage[4])
    views = probes.select(source.p, source.tri_v, camera.region, camera.near_z, inputs.probes.spacing, PROBE_SEED,
                          path, query=source.intersector)
    found = placed_training(source.p, source.tri_v, camera.near_z, PROBE_SEED, views, source.intersector)
    unknown = {probe["fate"] for probe in found} - set(FATES)
    if unknown:
        raise ValueError(f"the probes have fates FATES has no row for: {sorted(unknown)}")
    return Probed(f"{scene_id(scene_path)}-{object_name}", scene_id(scene_path).capitalize(), views.record, found,
                  None if coverage is None else coverage[4], source, camera.half_fov_short_tan)


def picture(probe_set, eye, forward, cut=None, size=VIEW_SIZE):
    image = see_through(probe_set.source, eye, forward, probe_set.lens, cut, size)
    return draw(image, probe_set.found, probe_set.path, eye, forward, probe_set.lens,
                ARROW_SPACING * probe_set.record["spacing"], cut)


def stills(probe_set, out):
    centre, eyes = views(*probe_set.record["region"], probe_set.lens)
    for index, (eye, forward) in enumerate(eyes, 1):
        save_png(picture(probe_set, eye, forward, centre), out / f"{probe_set.name}-probes-{index}.png")


def orbit(probe_set, out):
    eyes = orbit_views(*probe_set.record["region"], probe_set.lens)
    save_gif([picture(probe_set, eye, forward, size=ORBIT_SIZE) for eye, forward in eyes], out / f"{probe_set.name}-probes-orbit.gif",
             ORBIT_FRAME_MS)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scene", nargs=2, action="append", required=True, metavar=("SCENE", "OBJECT"))
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    probe_sets = []
    for scene_path, object_name in args.scene:
        probe_set = probed(pathlib.Path(scene_path), object_name)
        orbit(probe_set, args.out)
        stills(probe_set, args.out)
        probe_sets.append(probe_set)
    legend(probe_sets, args.out / "probe-fates.png")


if __name__ == "__main__":
    main()
