#!/usr/bin/env python3
"""Where a fit's probes (probes.py) sit in its scene, and what became of each, seen from above the four corners of the
probe region. The source is cut through the region's centre facing the eye and drawn see-through, as the engine's
`cull` view draws a culled cluster (render/view/raster_cull_view.c): a wash of the surface colour, one pixel of each
CULLED_DOOR square at full strength, and borders dashed CULLED_DASH on, CULLED_DASH off where the source object
changes. The camera path and every probe go on top, each probe in its fate's colour (FATES); a kept probe also
shows its kept cube faces as arrows.

    python launcher/tools/r3d/views/probe_figures.py --scene SCENE.scene.toml --object NAME --out DIR

The probes are the fit's own: the reference set's probe_poses picks them, at the import's geometry.probes or,
without one, at its defaults. Writes DIR/SCENE-OBJECT-probes-1.png .. -4.png and DIR/SCENE-OBJECT-probes-legend.png,
the fates with their counts.
"""

import argparse
import dataclasses
import json
import pathlib
import sys
import tempfile
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define  # noqa: E402
from docs.figures import save_png  # noqa: E402
from docs.plot_style import DPI, LABEL_SIZE, MUTED, SLOTS, figure, save  # noqa: E402
from docs.producer import Producer  # noqa: E402
from r3d.fitted_variant import placed_variant  # noqa: E402
from r3d.geometry import face_normals, hit_barycentrics  # noqa: E402
from r3d.import_settings import load_probes, load_scene  # noqa: E402
from r3d.light import AlphaTest, encode_srgb8  # noqa: E402
from r3d.mesh_source import camera_path_poses, load_source  # noqa: E402
from r3d.poses import camera_basis, camera_rays, read_poses  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.reference_render import hit_albedo  # noqa: E402
from r3d.reference_set import PROBE_RECORD, PROBE_SEED, PROBE_TRAINING, probe_poses, reference_inputs  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402
from r3d.views import probes  # noqa: E402

PRODUCER = Producer(area="render", needs=("host",),
                    run=("--scene", "launcher/demo/sponza/sponza.scene.toml", "--object", "atrium_fitted_full",
                         "--out", "{out}"),
                    reads=("launcher/demo/sponza/sponza.scene.toml", "launcher/demo/sponza/sponza.import.toml"),
                    outputs=("sponza-atrium_fitted_full-probes-1.png", "sponza-atrium_fitted_full-probes-2.png",
                             "sponza-atrium_fitted_full-probes-3.png", "sponza-atrium_fitted_full-probes-4.png",
                             "sponza-atrium_fitted_full-probes-legend.png"))

CULL_VIEW = ROOT / "launcher/main/render/view/raster_cull_view.c"
DOOR = read_define(CULL_VIEW, "CULLED_DOOR")
DASH = read_define(CULL_VIEW, "CULLED_DASH")
# Each view's size in pixels: half a doc page's width, so two sit side by side in a table.
VIEW_SIZE = (640, 400)
# Share of the surface colour every covered pixel takes over the background, so surfaces read between door pixels.
WASH = 0.4
BACKGROUND = (246, 246, 246)
BORDER = (110, 110, 110)
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
# The legend spans two views; its height in inches.
LEGEND_HEIGHT = 0.6


@dataclasses.dataclass(frozen=True)
class Fate:
    colour: str
    label: str
    cross: bool  # drawn as a cross (the probe has no faces) rather than a disc


# What became of a probe, in drawing order: one entry per fate probes.py gives, so a new fate is a new row.
FATES = {
    "kept": Fate(SLOTS[2], "kept (arrows: its kept faces)", False),
    "greedy": Fate(MUTED, "dropped by the greedy pick: shows nothing new", False),
    "inside": Fate(SLOTS[7], "dropped: inside geometry", True),
    "near": Fate(SLOTS[3], "dropped: a surface too near", True),
}


def rgb(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5))


def views(lo, hi, lens):
    """(centre, [(eye, forward)]): one view from above each of the box lo..hi's four top corners. Each is cut by the
    vertical plane through the centre facing it (cut_normal), looks at the middle of the box behind that cut and
    stands as near as keeps all of it in view with FRAME_MARGIN to spare."""
    lo, hi = np.asarray(lo, float), np.asarray(hi, float)
    centre, radius = (lo + hi) / 2, np.linalg.norm(hi - lo) / 2
    steps = np.linspace(0, 1, FRAME_SAMPLES)
    lattice = lo + np.stack(np.meshgrid(steps, steps, steps, indexing="ij"), axis=-1).reshape(-1, 3) * (hi - lo)
    result = []
    for x, z in ((hi[0], hi[2]), (lo[0], hi[2]), (lo[0], lo[2]), (hi[0], lo[2])):
        toward = np.array([x, hi[1], z]) - centre
        toward /= np.linalg.norm(toward)
        behind = lattice[(lattice - centre) @ cut_normal(-toward) >= 0]
        target = behind.mean(axis=0)
        near, far = 0.0, 2 * radius * np.sqrt(1 + 1 / lens ** 2)  # far: a sphere around all of it fills the view
        for _ in range(FRAME_STEPS):
            middle = (near + far) / 2
            xy, front = project(behind, target + toward * middle, -toward, lens)
            fits = front.all() and (np.abs(xy / VIEW_SIZE * 2 - 1) <= 1 - FRAME_MARGIN).all()
            near, far = (near, middle) if fits else (middle, far)
        result.append((target + toward * far, -toward))
    return centre, result


def project(points, eye, forward, lens):
    """Pixel positions of `points` in the view camera_rays casts, and whether each lies in front of the eye."""
    right, up, forward = camera_basis(forward)
    width, height = VIEW_SIZE
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


def see_through(source, query, eye, forward, lens, centre):
    """The source behind the cut through `centre`, drawn as the cull view draws a culled cluster: every ray starts
    where it crosses the cut."""
    width, height = VIEW_SIZE
    origin, direction = camera_rays(width, height, lens, eye, forward, 1)
    normal = cut_normal(forward)
    along = direction @ normal
    to_plane = np.where(along > 0, ((centre - eye) @ normal) / np.where(along > 0, along, 1), 0)
    origin = origin + direction * np.maximum(to_plane, 0)[:, None]
    hit, distance, tri = query.first_hits(origin, direction)
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


def fates(query, source, record, near, kept_eyes):
    """(position, fate, kept face directions) per training probe, placed again as select places them; their counts
    must be the record's."""
    lo, hi = record["region"]
    points, cell = probes.probe_points(lo, hi, record["spacing"], PROBE_SEED)
    moved, state = probes.classify(query, face_normals(source.p, source.tri_v)[0], points, cell, near)
    found = []
    for at, placed in zip(moved, state):
        faces = [pose[3:6] for pose in kept_eyes if np.allclose(pose[:3], at)]
        found.append((at, "kept" if faces else ("greedy" if placed == "kept" else str(placed)), faces))
    train = record["train"]
    counts = {name: sum(fate == name for _, fate, _ in found) for name in FATES}
    expected = {"inside": train["inside"], "near": train["near"]}
    if any(counts[name] != value for name, value in expected.items()) or len(found) != train["probes"]:
        raise RuntimeError(f"placing the probes again gives {counts}, the selection recorded {train}")
    return found, counts


def draw(image, found, path, eye, forward, lens, centre, arrow):
    """The camera path, then the probes behind the cut through `centre`, farthest first."""
    canvas = ImageDraw.Draw(image)
    if path is not None:
        xy, front = project([pose[:3] for pose in path], eye, forward, lens)
        canvas.line([tuple(p) for p in xy[front]], fill=rgb(PATH_COLOUR), width=PATH_WIDTH)
    behind = [probe for probe in found if (probe[0] - centre) @ cut_normal(forward) >= 0]
    order = sorted(behind, key=lambda probe: -np.linalg.norm(probe[0] - eye))
    for at, name, faces in order:
        fate = FATES[name]
        x, y = project([at], eye, forward, lens)[0][0]
        tips = project([at + np.asarray(face) * arrow for face in faces], eye, forward, lens)[0] if faces else []
        for tip in tips:
            canvas.line([(x, y), tuple(tip)], fill=rgb(fate.colour), width=LINE_WIDTH)
        r = KEPT_RADIUS if name == "kept" else MARK_RADIUS
        if fate.cross:
            canvas.line([(x - r, y - r), (x + r, y + r)], fill=rgb(fate.colour), width=LINE_WIDTH)
            canvas.line([(x - r, y + r), (x + r, y - r)], fill=rgb(fate.colour), width=LINE_WIDTH)
        else:
            canvas.ellipse([x - r, y - r, x + r, y + r], fill=rgb(fate.colour))
    return image


def legend(counts, faces, path, out):
    """The fates with their counts, and the camera path, as one row of keys."""
    from matplotlib.lines import Line2D

    fig = figure((2 * VIEW_SIZE[0] / DPI, LEGEND_HEIGHT))
    handles = []
    for name, fate in FATES.items():
        extra = f", {faces} faces" if name == "kept" else ""
        handles.append(Line2D([], [], color=fate.colour, linestyle="none", marker="x" if fate.cross else "o",
                              label=f"{fate.label}: {counts[name]}{extra}"))
    if path:
        handles.append(Line2D([], [], color=PATH_COLOUR, label="camera path"))
    fig.legend(handles=handles, loc="center", ncol=3, frameon=False, fontsize=LABEL_SIZE)
    save(fig, out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scene", type=pathlib.Path, required=True)
    parser.add_argument("--object", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    scene = load_scene(args.scene)
    job = placed_variant(scene, args.object)
    inputs = reference_inputs(job, scene)
    if inputs.probes is None:
        inputs = dataclasses.replace(inputs, probes=load_probes({}))
    camera = scene.camera.component
    path = None
    if camera.path is not None:
        visibility = SimpleNamespace(size=inputs.size, every_ms=None)
        coverage = camera_path_poses(inputs.scene(), visibility, inputs.poses["coverage_every_ms"])
        path = (coverage[0], coverage[1], coverage[2], coverage[4])
    with tempfile.TemporaryDirectory() as work:
        work = pathlib.Path(work)
        probe_poses(inputs, work, path)
        kept = read_poses(work / PROBE_TRAINING[0])[4]
        record = json.loads((work / PROBE_RECORD).read_text())
    source = load_source(job.settings, np.float32)
    keep_alpha = job.settings.alpha_keep
    query = RayQuery(source.p, source.tri_v, hit_filter=None if keep_alpha is None else AlphaTest(source, keep_alpha))
    found, counts = fates(query, source, record, camera.near_z, kept)
    centre, eyes = views(*record["region"], camera.half_fov_short_tan)
    stem = f"{scene_id(args.scene)}-{args.object}-probes"
    for index, (eye, forward) in enumerate(eyes, 1):
        image = see_through(source, query, eye, forward, camera.half_fov_short_tan, centre)
        draw(image, found, path[3] if path else None, eye, forward, camera.half_fov_short_tan, centre,
             ARROW_SPACING * record["spacing"])
        save_png(image, args.out / f"{stem}-{index}.png")
    legend(counts, len(kept), path is not None, args.out / f"{stem}-legend.png")


if __name__ == "__main__":
    main()
