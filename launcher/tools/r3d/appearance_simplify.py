#!/usr/bin/env python3
"""Fit a baked mesh's vertex positions and colours to reference renders
along a camera path, keeping its triangles (appearance-driven
simplification, Hasselgren et al. 2021). A smooth mesh fits a colour per
vertex, a flat one a colour per triangle.

    python launcher/tools/r3d/appearance_simplify.py --scene SCENE.scene.toml --start NAME.mesh \\
        --poses POSES.txt --reference DIR [--poses ... --reference ...] --out DIR [--per-shot] \\
        [--object NAME --surface-weight W --surface-margin E --seam-margin E] [--coverage-poses POSES.txt]

--start is a baked smooth or flat mesh, usually the simplifier's output at the
triangle budget. Each --poses file (tools/anim/track_host.py) pairs with
the --reference directory reference_render.py wrote for it; --scene is the
scene they were rendered from, whose camera background shows where nothing
is drawn. All pairs train one mesh; with --per-shot each pair trains its
own. A mesh is written as NAME.mesh by the same writer as mesh_import.py,
under DIR, or DIR/shot<k>. The surface term anchors the fit to the scene's
alpha-masked source; --object names its renderer.

The optimiser needs PyTorch with CUDA and nvdiffrast; see the README.
"""

import argparse
import json
from contextlib import contextmanager
import os
import pathlib
import sys
import time
from typing import NamedTuple

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.geometry import closest_point_on_triangles, triangle_areas  # noqa: E402
from r3d.fit.copies import check_hidden_copies, hidden_copy_pixels, joined, placed_copies  # noqa: E402
from r3d.import_settings import load_scene, match_meshes  # noqa: E402
from r3d.lit_mesh import finest_triangles, read_lit_mesh  # noqa: E402
from r3d.cost_model import load as load_cost  # noqa: E402
from r3d.poses import camera_basis, read_poses  # noqa: E402
from r3d.repair import _border_edges  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from colour_maths import lab  # noqa: E402

FAR = 1.0e5
SURFACE_QUERY_EVERY = 25
SURFACE_SAMPLES_PER_TRIANGLE = 6
SURFACE_REFERENCE_SAMPLES = 4_000_000
SURFACE_TANGENTIAL_WEIGHT = 0.05
# A start border point within this many capped local start edge scales of another part's triangle is held on it during the fit.
SEAM_MARGIN = 0.25
CUBLAS_WORKSPACE_ENV = "CUBLAS_WORKSPACE_CONFIG"
CUBLAS_WORKSPACE = ":4096:8"
RASTER_EPSILON = 1.0 / 16.0
# A triangle counts as stretched when its longest fitted edge exceeds this many times its longest start edge.
STRETCH_RATIO = 2.0
# A triangle counts as grown when its fitted area exceeds this many times its start area.
AREA_GROWTH_RATIO = 4.0
# A triangle counts as moved when its largest corner displacement exceeds this many mean start edges.
MOVED_EDGES = 1.0
# How many worst parts the log lists.
STRETCH_WORST = 5


@contextmanager
def deterministic_fit():
    """Keep CUDA reductions deterministic without changing subsequent in-process bakes."""
    try:
        import torch
    except ModuleNotFoundError as missing:
        if missing.name != "torch":
            raise
        # Argument validation also runs without the optional GPU dependencies.
        torch = None

    previous = None if torch is None else torch.are_deterministic_algorithms_enabled()
    warn_only = None if torch is None else torch.is_deterministic_algorithms_warn_only_enabled()
    fill_memory = None if torch is None else torch.utils.deterministic.fill_uninitialized_memory
    workspace = os.environ.get(CUBLAS_WORKSPACE_ENV)
    os.environ[CUBLAS_WORKSPACE_ENV] = CUBLAS_WORKSPACE
    try:
        if torch is not None:
            torch.use_deterministic_algorithms(True)
            torch.utils.deterministic.fill_uninitialized_memory = False
        yield
    finally:
        if torch is not None:
            torch.use_deterministic_algorithms(previous, warn_only=warn_only)
            torch.utils.deterministic.fill_uninitialized_memory = fill_memory
        if workspace is None:
            os.environ.pop(CUBLAS_WORKSPACE_ENV, None)
        else:
            os.environ[CUBLAS_WORKSPACE_ENV] = workspace


def edge_neighbours(tris):
    """Neighbours by vertex index; edges shared by 3+ triangles have no neighbour."""
    edges = np.stack([tris, np.roll(tris, -1, axis=1)], axis=-1).reshape(-1, 2)
    edges.sort(axis=1)
    order = np.lexsort((edges[:, 1], edges[:, 0]))
    boundaries = np.r_[0, np.flatnonzero(np.any(np.diff(edges[order], axis=0), axis=1)) + 1, len(edges)]
    pairs = boundaries[:-1][np.diff(boundaries) == 2]
    neighbours = np.full(len(edges), -1, dtype=np.int64)
    a, b = order[pairs], order[pairs + 1]
    neighbours[a], neighbours[b] = b // 3, a // 3
    return neighbours.reshape(-1, 3)


class FitMesh(NamedTuple):
    """What the fit moves and colours. points are the welded positions in
    model units; vertex_point maps each vertex to its position, so a colour
    seam cannot open into a crack when positions move. rgb (0..1) is one
    colour per vertex, or per triangle when the mesh is flat."""
    points: np.ndarray
    rgb: np.ndarray
    tris: np.ndarray
    double: np.ndarray
    scale: int
    vertex_point: np.ndarray
    flat: bool = False


def start_mesh(path):
    """The FitMesh of a generated mesh: a smooth one welded back to one
    vertex per position and colour, a flat one to one per position."""
    mesh = read_lit_mesh(path)
    q, rgb, tris, double, face = finest_triangles(mesh)
    flat = rgb is None
    points, vertex_point = np.unique(q, axis=0, return_inverse=True)
    return FitMesh(points / float(mesh.position_scale), (face if flat else rgb) / 255.0, tris, double, mesh.position_scale,
                   vertex_point.reshape(-1), flat)


def point_edges(tris, vertex_point):
    """The unique undirected edges between welded positions."""
    corners = vertex_point[tris]
    edges = np.concatenate([corners[:, [0, 1]], corners[:, [1, 2]], corners[:, [2, 0]]])
    edges = np.sort(edges[edges[:, 0] != edges[:, 1]], axis=1)
    return np.unique(edges, axis=0)


def start_parts(mesh) -> np.ndarray:
    """Connected-part labels for welded start points, including colour seams."""
    edges = point_edges(mesh.tris, mesh.vertex_point)
    graph = coo_matrix((np.ones(len(edges)), (edges[:, 0], edges[:, 1])),
                       shape=(len(mesh.points), len(mesh.points)))
    _, labels = connected_components(graph, directed=False)
    return labels


class SeamHolds(NamedTuple):
    """Border points tied to another start part's triangle at fixed barycentrics."""
    points: np.ndarray
    corners: np.ndarray
    barycentric: np.ndarray


def seam_holds(mesh: FitMesh, tolerance: np.ndarray) -> SeamHolds:
    """Hold each border point that starts within `tolerance` (a per-point distance) of
    another part's triangle on that triangle's closest point. Nearest contacts win,
    and a point already held, or a corner of a held point's target, is never used
    again, so no hold depends on another."""
    from scipy.spatial import cKDTree

    border = np.unique(_border_edges(mesh.vertex_point, mesh.tris))
    corners = mesh.vertex_point[mesh.tris]
    candidates = []
    if len(corners):
        labels = start_parts(mesh)
        triangles = mesh.points[corners]
        centroids = triangles.mean(axis=1)
        triangle_radius = np.linalg.norm(triangles - centroids[:, None], axis=2).max(axis=1)
        radius = triangle_radius.max()
        tree = cKDTree(centroids)
        for point in border:
            nearby = np.array(sorted(tree.query_ball_point(mesh.points[point], radius + tolerance[point])), dtype=np.int64)
            nearby = nearby[labels[corners[nearby, 0]] != labels[point]]
            nearby = nearby[np.linalg.norm(mesh.points[point] - centroids[nearby], axis=1)
                            <= triangle_radius[nearby] + tolerance[point]]
            if not len(nearby):
                continue
            a, b, c = triangles[nearby].transpose(1, 0, 2)
            barycentric, distance = closest_point_on_triangles(np.broadcast_to(mesh.points[point], a.shape), a, b, c)
            nearest = np.argmin(distance)
            if distance[nearest] <= tolerance[point]:
                ratio = distance[nearest] / tolerance[point] if tolerance[point] > 0 else 0.0
                candidates.append((ratio, int(point), int(nearby[nearest]), barycentric[nearest]))
    held, targets, weights = [], [], []
    held_points, target_points = set(), set()
    for _ratio, point, triangle, barycentric in sorted(candidates, key=lambda item: item[:2]):
        target = corners[triangle]
        if point in target_points or any(int(corner) in held_points for corner in target):
            continue
        held.append(point)
        targets.append(target)
        weights.append(barycentric)
        held_points.add(point)
        target_points.update(map(int, target))
    return SeamHolds(np.asarray(held, dtype=np.int64), np.asarray(targets, dtype=np.int64).reshape(-1, 3),
                     np.asarray(weights, dtype=float).reshape(-1, 3))


def hold_seams(points: "torch.Tensor", holds: SeamHolds) -> "torch.Tensor":
    """Apply independent seam holds out of place, with gradients into target corners."""
    import torch

    held = torch.as_tensor(holds.points, device=points.device)
    corners = torch.as_tensor(holds.corners, device=points.device)
    barycentric = torch.as_tensor(holds.barycentric, dtype=points.dtype, device=points.device)
    return points.index_put((held,), (points[corners] * barycentric[..., None]).sum(dim=1))


def triangle_edges(points, tris, vertex_point):
    """Each triangle's edge lengths in corner order, shaped (n, 3)."""
    corners = vertex_point[tris]
    return np.stack([np.linalg.norm(points[corners[:, k]] - points[corners[:, (k + 1) % 3]], axis=1) for k in range(3)], axis=1)


def ratio_with_inf(value, start):
    """Ratios with infinity for positive values over zero and unity for zero over zero."""
    ratio = np.where(value > 0, np.inf, 1.0)
    return np.divide(value, start, out=ratio, where=start > 0)


def triangle_stretch(start_points, fitted_points, mesh) -> np.ndarray:
    """Each triangle's longest fitted edge relative to its longest start edge."""
    start_edge = triangle_edges(start_points, mesh.tris, mesh.vertex_point).max(axis=1)
    fitted_edge = triangle_edges(fitted_points, mesh.tris, mesh.vertex_point).max(axis=1)
    return ratio_with_inf(fitted_edge, start_edge)


def triangle_area_growth(start_points, fitted_points, mesh) -> np.ndarray:
    """Each triangle's fitted area relative to its start area."""
    corners = mesh.vertex_point[mesh.tris]
    return ratio_with_inf(triangle_areas(fitted_points, corners), triangle_areas(start_points, corners))


def triangle_moved(start_points, fitted_points, mesh) -> np.ndarray:
    """Each triangle's largest corner move relative to its mean start edge."""
    corners = mesh.vertex_point[mesh.tris]
    moved = np.linalg.norm(fitted_points[corners] - start_points[corners], axis=2).max(axis=1)
    mean_edge = triangle_edges(start_points, mesh.tris, mesh.vertex_point).mean(axis=1)
    return ratio_with_inf(moved, mean_edge)


def stretch_report(start, fitted, kept) -> dict:
    """Edge stretch, area growth and displacement by start part, with small-part totals and worst parts."""
    labels = start_parts(start)
    parts = labels[start.vertex_point[start.tris[kept, 0]]]
    flags = {
        "stretch": triangle_stretch(start.points, fitted.points, fitted) > STRETCH_RATIO,
        "area_growth": triangle_area_growth(start.points, fitted.points, fitted) > AREA_GROWTH_RATIO,
        "moved": triangle_moved(start.points, fitted.points, fitted) > MOVED_EDGES,
    }
    edges = point_edges(start.tris, start.vertex_point)
    length = np.linalg.norm(start.points[edges[:, 0]] - start.points[edges[:, 1]], axis=1)
    global_edge = length.mean()
    edge_parts = labels[edges[:, 0]]
    part_count = int(labels.max()) + 1
    edge_count = np.bincount(edge_parts, minlength=part_count)
    edge_total = np.bincount(edge_parts, weights=length, minlength=part_count)
    mean_edge = np.divide(edge_total, edge_count, out=np.zeros(part_count), where=edge_count > 0)
    count = np.bincount(parts, minlength=part_count)
    rows = [dict(part=int(part), triangles=int(count[part]), mean_start_edge=float(mean_edge[part]))
            for part in np.flatnonzero(count)]
    groups = {"small": mean_edge < global_edge, "rest": mean_edge >= global_edge}
    report = dict(parts=rows, worst={})
    for name, mask in groups.items():
        report[name] = dict(triangles=int(count[mask].sum()))
    for measure, flagged in flags.items():
        measured = np.bincount(parts, weights=flagged, minlength=part_count)
        for row in rows:
            row[measure] = float(100 * measured[row["part"]] / row["triangles"])
        for name, mask in groups.items():
            triangles = report[name]["triangles"]
            report[name][measure] = float(100 * measured[mask].sum() / triangles) if triangles else 0.0
        report["worst"][measure] = sorted(rows, key=lambda row: (-row[measure], -row["triangles"], row["part"]))[:STRETCH_WORST]
    return report


def projection(eye, forward, width, height, lens, near):
    """A 4x4 world-to-clip matrix whose pixel centres cast the same rays as
    reference_render.py's pinhole, clip y up."""
    right, up, forward = camera_basis(forward)
    short = min(width, height)
    horizontal, vertical = lens * width / short, lens * height / short
    a, b = (FAR + near) / (FAR - near), -2.0 * FAR * near / (FAR - near)
    rows = np.array([right / horizontal, up / vertical, forward * a, forward])
    offsets = -rows @ np.asarray(eye, dtype=float) + np.array([0.0, 0.0, b, 0.0])
    return np.concatenate([rows, offsets[:, None]], axis=1)


def pose_views(poses_path, scale):
    """Target-free views and scaled render size for counting shown pixels."""
    width, height, lens, near, poses = read_poses(poses_path)
    return [(projection(pose[:3], pose[3:], width, height, lens, near), None, None, pose[:3]) for pose in poses], (width * scale, height * scale)


def load_views(pairs, scale):
    """[(clip matrix, target sRGB image 0..1, target normals or None, eye)],
    images at render size, for every pose of every (poses file, reference
    directory) pair, and the render size. The normals are the reference's
    NNNN.normal.npy, when it wrote them."""
    from PIL import Image

    views = []
    size = None
    for poses_path, reference in pairs:
        width, height, lens, near, poses = read_poses(poses_path)
        images = sorted(pathlib.Path(reference).glob("*.png"))
        if len(images) != len(poses):
            raise ValueError(f"{reference} has {len(images)} images for {len(poses)} poses")
        size = (width * scale, height * scale)
        for pose, image in zip(poses, images):
            target = np.asarray(Image.open(image).convert("RGB"), dtype=np.float32) / 255.0
            target = target.repeat(scale, axis=0).repeat(scale, axis=1)
            normal_path = image.with_name(image.stem + ".normal.npy")
            normal = None
            if normal_path.exists():
                normal = np.load(normal_path).astype(np.float32).repeat(scale, axis=0).repeat(scale, axis=1)
            views.append((projection(pose[:3], pose[3:], width, height, lens, near), target, normal, pose[:3]))
    return views, size


def delta_e76(a, b):
    """Per-pixel CIE76 dE in torch; the floor and the epsilon keep the
    gradient finite at black and at zero."""
    import torch

    return ((lab(torch, a, 1e-6) - lab(torch, b, 1e-6)) ** 2).sum(dim=-1).add(1e-6).sqrt()


def vertex_normals(points, tris, vertex_point):
    """Per vertex, the unit area-weighted normal of the triangles around its
    welded position, in torch, differentiable in `points`."""
    import torch

    corners = points[vertex_point][tris]
    face = torch.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0], dim=1)
    around = torch.zeros_like(points).index_add_(0, vertex_point[tris].reshape(-1), face.repeat_interleave(3, dim=0))
    return torch.nn.functional.normalize(around, dim=1)[vertex_point]


class Renderer:
    """Draws the mesh as the device does: Gouraud vertex colours, or with
    `flat` one colour per triangle, single-sided triangles culled when they
    face away, `clear` (0..1 RGB) where nothing is drawn. A flat mesh is drawn
    with three vertices of its own per triangle, so every edge between two
    colours is antialiased and moves its positions."""

    def __init__(self, tris, double, vertex_point, size, device, clear=(0.0, 0.0, 0.0), flat=False):
        import nvdiffrast.torch as dr
        import torch

        if flat:
            vertex_point = np.asarray(vertex_point)[tris].reshape(-1)
            tris = np.arange(len(vertex_point)).reshape(-1, 3)
        self.neighbours = torch.as_tensor(edge_neighbours(np.asarray(tris)), device=device)
        self.flat = flat
        self.dr = dr
        self.context = dr.RasterizeCudaContext(device=device)
        self.tris = torch.as_tensor(tris, dtype=torch.int32, device=device)
        self.double = torch.as_tensor(np.asarray(double, dtype=bool), device=device)
        self.vertex_point = torch.as_tensor(vertex_point, dtype=torch.int64, device=device)
        self.size = size
        self.clear = torch.as_tensor(clear, dtype=torch.float32, device=device)

    def clip(self, points, clip_matrix):
        """Clip-space positions of every vertex, y up."""
        import torch

        positions = points[self.vertex_point]
        return torch.cat([positions, torch.ones_like(positions[:, :1])], dim=1) @ clip_matrix.T

    def facing(self, clip):
        """Which triangles the device would draw: double-sided, front-facing,
        or crossing the eye plane, where facing is not yet known."""
        import torch

        with torch.no_grad():
            corners = clip[self.tris.long()]
            w = corners[..., 3]
            xy = corners[..., :2] / w.clamp_min(1e-6)[..., None]
            area = (xy[:, 1, 0] - xy[:, 0, 0]) * (xy[:, 2, 1] - xy[:, 0, 1]) - (xy[:, 2, 0] - xy[:, 0, 0]) * (xy[:, 1, 1] - xy[:, 0, 1])
            return self.double | (area > 0) | (w <= 0).any(dim=1)

    def visible_ids(self, points, clip_matrix):
        """The triangle each pixel shows, -1 where none."""
        import torch

        _clip, rast, keep = self.raster(points, clip_matrix)
        local = rast[0, ..., 3].long() - 1
        mapping = torch.cat([keep, keep.new_full((1,), -1)])
        return mapping[torch.where(local >= 0, local, len(keep))]


    def raster(self, points, clip_matrix, clip=None):
        """Detached visibility from the library; positions retain only torch gradients."""
        import torch

        clip = self.clip(points, clip_matrix) if clip is None else clip
        keep = torch.nonzero(self.facing(clip))[:, 0]
        width, height = self.size
        with torch.no_grad():
            rast, _ = self.dr.rasterize(self.context, clip[None].detach().contiguous(),
                                       self.tris[keep].contiguous(), resolution=[height, width])
        return clip, rast, keep

    def barycentrics(self, clip, rast, keep, locations=None):
        """Perspective weights via 2D homogeneous coordinates (Olano and Greer 1997)."""
        import torch

        from r3d.appearance_gather import gather

        height, width = rast.shape[1:3]
        local = rast[0, ..., 3].long() - 1
        compact = locations is not None
        locations = torch.nonzero(local >= 0) if locations is None else locations
        y, x = locations.unbind(-1)
        pixel = torch.stack([(2 * x + 1) / width - 1, (2 * y + 1) / height - 1], dim=-1)
        corners = gather(clip, self.tris[keep[local[y, x]]].long())
        relative = corners[..., :2] - pixel[..., None, :] * corners[..., 3:]
        following = relative.roll(-1, dims=-2)
        weights = relative[..., 0] * following[..., 1] - relative[..., 1] * following[..., 0]
        weights = weights.roll(-1, dims=-1)
        denominator = weights.sum(-1, keepdim=True)
        valid = denominator != 0
        weights = torch.where(valid, weights / torch.where(valid, denominator, 1), 0)
        # Subpixel coverage can include centres just outside the exact triangle.
        uv = weights[..., :2].clamp(0, 1)
        weights = torch.cat([uv, torch.where(valid, 1 - uv.sum(-1, keepdim=True), 0)], dim=-1)
        if compact:
            return weights
        return clip.new_zeros((height, width, 3)).index_put((y, x), weights)

    def interpolate(self, attributes, clip, rast, keep):
        """Interpolate covered pixels, leaving background zero."""
        import torch

        from r3d.appearance_gather import gather

        local = rast[0, ..., 3].long() - 1
        locations = torch.nonzero(local >= 0)
        y, x = locations.unbind(-1)
        indices = self.tris[keep[local[y, x]]].long()
        weights = self.barycentrics(clip, rast, keep, locations)
        values = (weights[..., None] * gather(attributes, indices)).sum(-2)
        pixels = attributes.new_zeros((local.numel(), attributes.shape[-1]))
        return pixels.index_copy(0, y * local.shape[1] + x, values).reshape(*local.shape, attributes.shape[-1])

    def antialias(self, image, clip, rast, keep):
        """Analytic visibility coverage, Laine et al. 2020 section 3.6."""
        import torch

        from r3d.appearance_gather import gather

        height, width = image.shape[:2]
        local = rast[0, ..., 3].long() - 1
        ids = torch.cat([keep, keep.new_full((1,), -1)])[torch.where(local >= 0, local, len(keep))]
        depth = rast[0, ..., 2]
        xy = (clip[:, :2] / torch.where(clip[:, 3:] != 0, clip[:, 3:], 1) + 1) * clip.new_tensor([width, height]) / 2
        with torch.no_grad():
            corners = xy[self.tris.long()]
            ab, ac = corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]
            area = ab[:, 0] * ac[:, 1] - ab[:, 1] * ac[:, 0]
            facing = torch.zeros(len(self.tris), dtype=torch.bool, device=clip.device)
            facing[keep] = True
            adjacent = self.neighbours.clamp_min(0)
            silhouette = (self.neighbours < 0) | ~facing[adjacent] | (area[:, None] * area[adjacent] <= 0)
        with torch.no_grad():
            horizontal = torch.nn.functional.pad(ids[:, :-1] != ids[:, 1:], (0, 1))
            vertical = torch.nn.functional.pad(ids[:-1] != ids[1:], (0, 0, 0, 1))
            axis, y, x = torch.nonzero(torch.stack([horizontal, vertical])).unbind(-1)
            other_y, other_x = y + axis, x + 1 - axis
            left, right = ids[y, x], ids[other_y, other_x]
            near = torch.where((left >= 0) & ((right < 0) | (depth[y, x] <= depth[other_y, other_x])), left, right)
            edges = self.tris[near].long()
        start = gather(xy, edges)
        finish = start.roll(-1, dims=1)
        direction = finish - start
        horizontal = (axis == 0)[:, None]
        along = torch.where(horizontal, direction[..., 0], direction[..., 1])
        denominator = torch.where(horizontal, direction[..., 1], direction[..., 0])
        centre_along = torch.where(axis == 0, x, y).to(clip.dtype) + 0.5
        centre_across = torch.where(axis == 0, y, x).to(clip.dtype) + 0.5
        start_along = torch.where(horizontal, start[..., 0], start[..., 1])
        start_across = torch.where(horizontal, start[..., 1], start[..., 0])
        safe = torch.where(denominator != 0, denominator, 1)
        fraction = (centre_across[:, None] - start_across) / safe
        crossing = start_along + fraction * along - centre_along[:, None]
        with torch.no_grad():
            valid = silhouette[near] & (denominator.abs() > along.abs())
            valid &= (fraction >= 0) & (fraction <= 1)
            valid &= (crossing >= -RASTER_EPSILON) & (crossing <= 1 + RASTER_EPSILON)
            selected = valid.to(torch.int64).argmax(-1)
            active = valid.any(-1)
        selected = torch.arange(crossing.shape[1], device=selected.device) == selected[:, None]
        crossing = torch.where(selected, crossing, 0).sum(-1).clamp(0, 1)
        weight = torch.where(active, 0.5 - crossing, 0)
        pixels = image.reshape(-1, image.shape[-1])
        pair = gather(pixels, torch.stack([y * width + x, other_y * width + other_x], dim=-1))
        difference = pair[:, 1] - pair[:, 0]
        delta = torch.zeros_like(pixels)
        delta = delta.index_add(0, y * width + x, difference * weight.clamp_min(0)[:, None])
        delta = delta.index_add(0, other_y * width + other_x, difference * weight.clamp_max(0)[:, None])
        return image + delta.reshape_as(image) + clip.sum() * 0

    def normals(self, points, clip_matrix, eye, clip=None, raster=None, values=None):
        """World normals facing the eye and the covered mask, in image row order."""
        import torch

        clip, rast, keep = self.raster(points, clip_matrix, clip) if raster is None else raster
        if values is None:
            vertex = vertex_normals(points, self.tris.long(), self.vertex_point)
            values = self.interpolate(torch.cat([vertex, points[self.vertex_point]], dim=1), clip, rast, keep)
        normal = torch.nn.functional.normalize(values[..., :3], dim=-1)
        towards = torch.as_tensor(eye, dtype=normal.dtype, device=normal.device) - values[..., 3:]
        normal = torch.where((normal * towards).sum(-1, keepdim=True) < 0, -normal, normal)
        return normal.flip(0), (rast[0, ..., 3] > 0).flip(0)

    def __call__(self, points, colours, clip_matrix, clip=None, raster=None, values=None):
        import torch

        clip, rast, keep = self.raster(points, clip_matrix, clip) if raster is None else raster
        if values is None:
            attributes = colours.repeat_interleave(3, dim=0) if self.flat else colours
            values = self.interpolate(attributes, clip, rast, keep)
        colour = values
        colour = torch.where(rast[0, ..., 3:] > 0, colour, self.clear.expand_as(colour))
        return self.antialias(colour, clip, rast, keep).flip(0)


def visible(mesh, views, size, device="cuda"):
    """Visible triangle IDs for each view, without gradients."""
    import torch

    render = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, device, flat=mesh.flat)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device=device)
    for matrix, *_rest in views:
        with torch.no_grad():
            ids = render.visible_ids(points, torch.as_tensor(matrix, dtype=torch.float32, device=device))
        yield ids


def coverage(mesh, views, size, device="cuda"):
    """Per triangle, the pixels it shows summed over every view: zero for a
    triangle no view sees, small for a sliver or a speck."""
    import torch

    total = torch.zeros(len(mesh.tris), dtype=torch.int64, device=device)
    for ids in visible(mesh, views, size, device):
        total += torch.bincount(ids[ids >= 0], minlength=len(mesh.tris))
    return total.cpu().numpy()


def prune(mesh, shown, budget):
    """Drop never-shown triangles first, then those showing fewest pixels to
    meet `budget`. A flat mesh's colours go with their triangles. Return the
    pruned mesh and the kept start indices."""
    order = np.argsort(-np.asarray(shown), kind="stable")
    keep = np.sort(order[: min(budget, int(np.count_nonzero(shown)))])
    pruned = mesh._replace(rgb=mesh.rgb[keep] if mesh.flat else mesh.rgb, tris=mesh.tris[keep],
                           double=np.asarray(mesh.double)[keep])
    return pruned, keep


def uniform_laplacian(points, edges):
    """Each position minus the mean of its neighbours."""
    import torch

    total = torch.zeros_like(points)
    count = torch.zeros(len(points), 1, device=points.device)
    for a, b in ((0, 1), (1, 0)):
        total.index_add_(0, edges[:, a], points[edges[:, b]])
        count.index_add_(0, edges[:, a], torch.ones(len(edges), 1, device=points.device))
    return points - total / count.clamp_min(1.0)


def vertex_scale(start, edges):
    """Each point's mean start edge length, capped at the start's mean edge length
    so large parts keep the global hold; points without edges take the global
    mean (the scale-dependent Laplacian of Desbrun, Meyer, Schröder and Barr,
    Implicit Fairing of Irregular Meshes, SIGGRAPH 1999)."""
    import torch

    length = (start[edges[:, 0]] - start[edges[:, 1]]).norm(dim=1)
    edge_length = float(length.mean())
    total = start.new_zeros(len(start))
    count = torch.zeros_like(total)
    for endpoint in (0, 1):
        total.index_add_(0, edges[:, endpoint], length)
        count.index_add_(0, edges[:, endpoint], torch.ones_like(length))
    local = total / count.clamp_min(1)
    return torch.where(count > 0, local.clamp_max(edge_length), edge_length)[:, None]


def predicted_ms(xp, cost, clip, tris, double, size, scale):
    """cost_model's milliseconds for one view, less the constant and the
    cluster terms, which moving vertices cannot change."""
    from r3d.cost_model import triangle_terms, variable_ms

    drawn, rows, pixels = triangle_terms(xp, clip, tris, double, size[0] // scale, size[1] // scale)
    return variable_ms(cost, drawn, rows, pixels)


def normal_pairs(render, points, view, clip, **samples):
    """(the mesh's normals, the reference's, the pixels both cover) in `view`."""
    matrix, _target, reference, eye = view
    normal, covered = render.normals(points, matrix, eye, clip, **samples)
    return normal, reference, covered & (reference.abs().sum(-1) > 0)


def normal_l1(render, points, view, clip, angles=True, **samples):
    """Mean L1 distance between the mesh's and the reference's normals over
    the pixels both cover. Optional angles in degrees need pixel compaction."""
    import torch

    normal, reference, both = normal_pairs(render, points, view, clip, **samples)
    distance = torch.where(both, (normal - reference).abs().sum(-1), 0).sum() / both.sum().clamp_min(1)
    if not angles:
        return distance, None
    cosine = (normal * reference).sum(-1)[both].clamp(-1.0, 1.0)
    return distance, torch.rad2deg(torch.acos(cosine.detach()))


def normal_error(mesh, views, size, device="cuda", angle_dir=None):
    """Mean angular error in degrees between the mesh's normals and the
    reference's, over the pixels both cover in every view. With `angle_dir`,
    each view's per-pixel angle is also written there as NNNN.angle.npy, NaN
    where the mesh or the reference shows nothing."""
    import torch

    render = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, device, flat=mesh.flat)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device=device)
    angles = []
    with torch.no_grad():
        for index, (matrix, target, reference, eye) in enumerate(views):
            view = (torch.as_tensor(matrix, dtype=torch.float32, device=device), None,
                    torch.as_tensor(reference, device=device), eye)
            angles.append(normal_l1(render, points, view, None)[1])
            if angle_dir is not None:
                normal, reference, both = normal_pairs(render, points, view, None)
                cosine = (normal * reference).sum(-1).clamp(-1.0, 1.0)
                degrees = torch.where(both, torch.rad2deg(torch.acos(cosine)), torch.full_like(cosine, float("nan")))
                pathlib.Path(angle_dir).mkdir(parents=True, exist_ok=True)
                np.save(pathlib.Path(angle_dir) / f"{index:04d}.angle.npy", degrees.cpu().numpy())
    return float(torch.cat(angles).mean())


def triangle_error(mesh, views, size, clear, device="cuda"):
    """Per triangle, the dE76 summed over the pixels it shows in every view."""
    import torch

    render = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, device, clear, mesh.flat)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device=device)
    colours = torch.as_tensor(mesh.rgb, dtype=torch.float32, device=device)
    total = torch.zeros(len(mesh.tris), device=device)
    with torch.no_grad():
        for matrix, target, *_rest in views:
            matrix = torch.as_tensor(matrix, dtype=torch.float32, device=device)
            error = delta_e76(render(points, colours, matrix), torch.as_tensor(target, device=device))
            ids = render.visible_ids(points, matrix).flip(0)
            shown = ids >= 0
            total.index_add_(0, ids[shown], error[shown])
    return total.cpu().numpy()


def refine(mesh, error, budget):
    """The mesh with the longest edge of its worst triangles split, both
    sides of the edge at once, until it holds about `budget` triangles: a
    fitted coarse mesh warm-starts a finer one where its error is. A flat
    mesh's halves keep their triangle's colour."""
    from r3d.tessellate import split_marked_edges

    points, rgb, tris, double, _scale, vertex_point, flat = mesh
    positions = points[vertex_point]
    while len(tris) < budget:
        corners = vertex_point[tris]
        lengths = triangle_edges(points, tris, vertex_point)
        longest = lengths.argmax(axis=1)
        marked_points = set()
        for index in np.argsort(-error)[: max(1, (budget - len(tris)) // 2)]:
            a, b = corners[index, longest[index]], corners[index, (longest[index] + 1) % 3]
            marked_points.add((min(a, b), max(a, b)))
        marked = set()
        for t in tris:
            for k in range(3):
                a, b = t[k], t[(k + 1) % 3]
                pa, pb = vertex_point[a], vertex_point[b]
                if (min(pa, pb), max(pa, pb)) in marked_points:
                    marked.add((min(a, b), max(a, b)))
        positions, tris, midpoint, parent = split_marked_edges(positions, tris, marked)
        if flat:
            colours = rgb[parent]
        else:
            colours = np.concatenate([rgb, np.zeros((len(positions) - len(rgb), 3))])
            for (a, b), m in midpoint.items():
                colours[m] = (rgb[a] + rgb[b]) / 2
        rgb, double, error = colours, np.asarray(double)[parent], error[parent] / 2
        points, vertex_point = np.unique(positions, axis=0, return_inverse=True)
        vertex_point = vertex_point.reshape(-1)
    return mesh._replace(points=points, rgb=rgb, tris=tris, double=double, vertex_point=vertex_point)


def surface_loss(samples, centres, normals, scale, margin):
    """From the fit to the source: a point-to-plane band of half-width `margin`
    and a small pull, normalised by scalar or per-sample start geometry scales."""
    offset = (samples - centres) / scale
    envelope = ((offset * normals).sum(-1).abs() - margin).clamp_min(0).square()
    return (envelope + SURFACE_TANGENTIAL_WEIGHT * offset.square().sum(-1)).mean()


@deterministic_fit()
def optimise(mesh, views, size, steps, batch, lr_position, lr_colour, laplacian, seed=1, device="cuda", report=100,
             clear=(0.0, 0.0, 0.0), cost=None, cost_weight=0.0, scale=1, normal_weight=0.0, surface_weight=0.0,
             surface_margin=0.0, source=None, seam_margin=SEAM_MARGIN, copies=None):
    """Fit fixed triangles; return positions, colours and colour-only error history.
    Position steps use bounding diagonals and each vertex's capped local start scale.
    Laplacian drift and the source envelope use capped local start edge scales,
    averaged per triangle for the envelope; normals and predicted cost add their
    weighted penalties.
    Both learning rates decay tenfold over the run.
    Seam holds pin border points that start near another part to its moving
    triangles by default; `seam_margin=0` turns them off.
    `copies` (a FitMesh, fit/copies.py) is drawn after the mesh, unchanged."""
    import torch

    points0, rgb0, tris, double, _scale, vertex_point, flat = mesh
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    diagonal = float(np.linalg.norm(points0.max(axis=0) - points0.min(axis=0)))
    renderers = {}
    drawn = joined(mesh, copies)

    def renderer_for(index):
        """The renderer at view `index`'s size: a reference of either
        orientation trains the same mesh."""
        height, width = targets[index].shape[:2]
        if (width, height) not in renderers:
            renderers[width, height] = Renderer(drawn.tris, drawn.double, drawn.vertex_point, (width, height), device, clear, flat)
        return renderers[width, height]

    edges = torch.as_tensor(point_edges(tris, vertex_point), device=device)
    start = torch.as_tensor(points0, dtype=torch.float32, device=device)
    delta = torch.zeros_like(start).requires_grad_(True)
    colours = torch.as_tensor(rgb0, dtype=torch.float32, device=device).requires_grad_(True)
    rest = uniform_laplacian(start, edges)
    point_scale = vertex_scale(start, edges)
    holds = None
    if seam_margin > 0:
        holds = seam_holds(mesh, seam_margin * point_scale.detach().cpu().numpy()[:, 0])
        log(f"seam holds: {len(holds.points)} points")
    step_scale = point_scale / point_scale.max()

    def held_positions():
        points = start + step_scale * delta
        return hold_seams(points, holds) if holds is not None else points

    copy_points, copy_colours = (None, None) if copies is None else (
        torch.as_tensor(values, dtype=torch.float32, device=device) for values in (copies.points, copies.rgb))

    def with_copies(points, colours):
        if copies is None:
            return points, colours
        return torch.cat([points, copy_points]), torch.cat([colours, copy_colours])

    matrices = [torch.as_tensor(view[0], dtype=torch.float32, device=device) for view in views]
    targets = [torch.as_tensor(view[1], device=device) for view in views]
    normals = [None if len(view) < 3 or view[2] is None else torch.as_tensor(view[2], device=device) for view in views]
    if normal_weight and any(normal is None for normal in normals):
        raise ValueError("a normal term needs the reference's normal buffers: reference_render.py --normals")
    if surface_weight:
        from scipy.spatial import cKDTree
        from r3d.surface_samples import area_samples, uniform_barycentric

        if source is None:
            raise ValueError("a surface term needs the alpha-masked source")
        surface_rng = np.random.default_rng(seed)
        reference, face_normals = area_samples(source, np.arange(len(source.tri_v)),
                                               SURFACE_REFERENCE_SAMPLES, surface_rng)
        tree = cKDTree(reference)
        reference = torch.as_tensor(reference, dtype=start.dtype, device=device)
        face_normals = torch.as_tensor(face_normals, dtype=start.dtype, device=device)
        corners = torch.as_tensor(vertex_point[tris], device=device)
        sample_scale = point_scale[:, 0][corners].mean(1).repeat_interleave(SURFACE_SAMPLES_PER_TRIANGLE)[:, None]
        barycentric = torch.as_tensor(uniform_barycentric(surface_rng, (len(tris), SURFACE_SAMPLES_PER_TRIANGLE)),
                                      dtype=start.dtype, device=device)
    adam = torch.optim.Adam([{"params": [delta], "lr": lr_position * diagonal}, {"params": [colours], "lr": lr_colour}])
    schedule = torch.optim.lr_scheduler.LambdaLR(adam, lambda step: 0.1 ** (step / max(steps, 1)))
    history = torch.empty(steps, device=device)
    finite = torch.ones((), dtype=torch.bool, device=device)
    started = time.time()
    for step in range(steps):
        chosen = rng.choice(len(views), size=min(batch, len(views)), replace=False)
        adam.zero_grad()
        points = held_positions()
        draw, draw_colours = with_copies(points, colours)
        error, predicted, colour_error, surface = 0.0, 0.0, 0.0, 0.0
        if normal_weight:
            render = renderer_for(chosen[0])
            vertex = vertex_normals(draw, render.tris.long(), render.vertex_point)
            attributes = draw_colours.repeat_interleave(3, dim=0) if flat else draw_colours
            attributes = torch.cat([attributes, vertex, draw[render.vertex_point]], dim=1)
        for i in chosen:
            render = renderer_for(i)
            clip = render.clip(draw, matrices[i])
            samples = {}
            if normal_weight:
                raster = render.raster(draw, matrices[i], clip)
                values = render.interpolate(attributes, *raster)
                samples = dict(raster=raster, values=values[..., :3])
            colour = delta_e76(render(draw, draw_colours, matrices[i], clip, **samples), targets[i]).mean() / len(chosen)
            colour_error = colour_error + colour
            error = error + colour
            if cost is not None:
                predicted = predicted + predicted_ms(torch, cost, clip, render.tris.long(), render.double, render.size,
                                                     scale) / len(chosen)
            if normal_weight:
                distance, _angles = normal_l1(render, draw, (matrices[i], None, normals[i], views[i][3]), clip,
                                              angles=False, raster=raster, values=values[..., 3:])
                error = error + normal_weight * distance / len(chosen)
        drift = ((uniform_laplacian(points, edges) - rest) / point_scale).pow(2).sum(dim=1).mean()
        if surface_weight:
            from r3d.appearance_gather import gather

            samples = (gather(points, corners)[:, None, :, :] * barycentric[..., None]).sum(-2).reshape(-1, 3)
            if step % SURFACE_QUERY_EVERY == 0:
                nearest = torch.as_tensor(tree.query(samples.detach().cpu().numpy())[1], device=device)
            surface = surface_weight * surface_loss(samples, reference[nearest], face_normals[nearest],
                                                     sample_scale, surface_margin)
        loss = error + laplacian * drift + cost_weight * predicted + surface
        loss.backward()
        adam.step()
        schedule.step()
        with torch.no_grad():
            colours.clamp_(0.0, 1.0)
        history[step] = colour_error.detach()
        finite &= torch.isfinite(loss.detach())
        if (report and step % report == 0) or step == steps - 1:
            if not (finite & torch.isfinite(delta).all() & torch.isfinite(colours).all()).item():
                raise ValueError("fit requires finite loss and parameters")
        if report and (step % report == 0 or step == steps - 1):
            log(f"step {step}: mean dE76 {float(history[step]):.3f}, predicted {float(predicted):.2f} ms, drift {float(drift):.3e}, "
                f"surface {float(surface):.6f}, {time.time() - started:.1f} s")
    points = held_positions()
    return points.detach().cpu().numpy().astype(np.float64), colours.detach().cpu().numpy().astype(np.float64), history.cpu().tolist()


def write_mesh(out_dir, name, mesh):
    """The fitted mesh as <name>.mesh and <name>.obj, a flat one's OBJ with
    three vertices of each triangle's colour; returns the baked mesh's
    triangle count."""
    from r3d.lit_mesh import write_lit_mesh

    positions = mesh.points[mesh.vertex_point]
    rgb = mesh.rgb
    colours = np.clip(np.rint(rgb * 255.0), 0, 255)
    out_dir = pathlib.Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    tris = mesh.tris
    if mesh.flat:
        baked = write_lit_mesh(out_dir, name, positions, None, tris, mesh.double, position_scale=mesh.scale,
                               face_rgb=colours)
        positions, rgb, tris = positions[tris].reshape(-1, 3), rgb.repeat(3, axis=0), np.arange(3 * len(tris)).reshape(-1, 3)
    else:
        baked = write_lit_mesh(out_dir, name, positions, colours, tris, mesh.double, position_scale=mesh.scale)
    with open(out_dir / f"{name}.obj", "w", newline="\n") as obj:
        for p, c in zip(positions, rgb):
            obj.write("v %.4f %.4f %.4f %.4f %.4f %.4f\n" % (*p, *c))
        for t in tris:
            obj.write("f %d %d %d\n" % tuple(t + 1))
    return len(baked.tris)


@deterministic_fit()
def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scene", required=True, help="the scene file the references were rendered from")
    parser.add_argument("--start", required=True, help="a smooth or flat NAME.mesh to start from")
    parser.add_argument("--poses", action="append", required=True, help="a track_host.py poses file")
    parser.add_argument("--reference", action="append", required=True, help="reference_render.py output for the poses")
    parser.add_argument("--out", required=True)
    parser.add_argument("--per-shot", action="store_true", help="one mesh per --poses file")
    parser.add_argument("--scale", type=int, default=2, help="render pixels per reference pixel")
    parser.add_argument("--steps", type=int, default=1500)
    parser.add_argument("--batch", type=int, default=4)
    parser.add_argument("--lr-position", type=float, default=2e-4, help="per step, in bounding diagonals")
    parser.add_argument("--lr-colour", type=float, default=0.01)
    parser.add_argument("--laplacian", type=float, default=10.0)
    parser.add_argument("--budget", type=int, help="prune to this many triangles, those that show least first")
    parser.add_argument("--coverage-poses", help="a denser poses file to count shown pixels over; the training poses otherwise")
    parser.add_argument("--cost-model", help="a cost_model.py weights file, such as board_cost_weights.txt")
    parser.add_argument("--cost-weight", type=float, default=0.0, help="dE76 per predicted millisecond")
    parser.add_argument("--object", help="the scene object whose source anchors the fit")
    parser.add_argument("--surface-weight", type=float, default=0.0, help="weight of the source-surface envelope")
    parser.add_argument("--surface-margin", type=float, default=0.0, help="source envelope margin in local start edge scales")
    parser.add_argument("--seam-margin", type=float, default=SEAM_MARGIN, help="seam hold margin in local start edge scales (0 holds nothing)")
    parser.add_argument("--copies", help="a carved start bundle: its copies are drawn with the start, unchanged")
    parser.add_argument("--copy-poses", help="with --copies, the poses the copy-coverage gate checks the fit over")
    parser.add_argument("--normal-weight", type=float, default=0.0, help="weight of the L1 normal term")
    parser.add_argument("--refine-to", type=int, help="first split the start's worst triangles up to this many")
    parser.add_argument("--score", action="store_true", help="fit nothing: print the start's mean normal error on the poses")
    parser.add_argument("--angle-dir", help="with --score, also write each pose's per-pixel normal angle there, for render_compare.py --angle-sheet")
    args = parser.parse_args(argv)
    if len(args.poses) != len(args.reference):
        parser.error("give one --reference per --poses")
    if any(not np.isfinite(value) or value < 0 for value in (args.surface_weight, args.surface_margin, args.seam_margin)):
        parser.error("surface weight, surface margin and seam margin must be finite and nonnegative")
    if args.surface_weight and not args.score and not args.object:
        parser.error("a surface term needs --object, the scene object whose source anchors the fit")
    if bool(args.copies) != bool(args.copy_poses) or (args.copies and not args.object):
        parser.error("--copies needs --copy-poses and --object, the scene object whose source shows the copies")
    coverage_views, coverage_size = pose_views(args.coverage_poses, args.scale) if args.coverage_poses else (None, None)
    scene = load_scene(args.scene)
    if scene.camera is None:
        parser.error("--scene needs a camera")
    background = scene.camera.component.background
    clear = tuple(((background >> shift) & 255) / 255.0 for shift in (16, 8, 0))
    start = pathlib.Path(args.start)
    if start.suffix != ".mesh":
        parser.error("--start must be a NAME.mesh")
    name = start.stem
    mesh = start_mesh(start)
    log(f"start: {len(mesh.tris)} {'flat' if mesh.flat else 'smooth'} triangles, {len(mesh.points)} positions")
    copies = None if args.copies is None else placed_copies(args.copies, mesh.flat)
    if copies is not None:
        log(f"copies: {len(copies.tris)} triangles drawn with the start")
    pairs = list(zip(args.poses, args.reference))
    shots = [[pair] for pair in pairs] if args.per_shot else [pairs]
    for index, shot in enumerate(shots):
        views, size = load_views(shot, args.scale)
        out = pathlib.Path(args.out) / (f"shot{index}" if args.per_shot else "")
        log(f"{out}: {len(views)} views at {size[0]}x{size[1]}")
        if args.score:
            print(f"normal error: {normal_error(mesh, views, size, angle_dir=args.angle_dir):.3f} degrees")
            continue
        fitted = mesh
        if args.refine_to is not None:
            fitted = refine(mesh, triangle_error(mesh, views, size, clear), args.refine_to)
            log(f"refined {len(mesh.tris)} -> {len(fitted.tris)} triangles")
        source = None
        if args.surface_weight or copies is not None:
            from r3d.reference_render import source_for

            source, job = source_for(scene, args.object, lit=False)
        fit_start = fitted
        cost = None if args.cost_model is None else load_cost(args.cost_model)[0]
        points, rgb, history = optimise(fitted, views, size, args.steps, args.batch, args.lr_position, args.lr_colour,
                                        args.laplacian, clear=clear, cost=cost, cost_weight=args.cost_weight,
                                        scale=args.scale, normal_weight=args.normal_weight, source=source,
                                        surface_weight=args.surface_weight, surface_margin=args.surface_margin,
                                        seam_margin=args.seam_margin, copies=copies)
        fitted = fitted._replace(points=points, rgb=rgb)
        kept = np.arange(len(fit_start.tris))
        if args.budget is not None:
            shown = coverage(joined(fitted, copies), coverage_views if args.coverage_poses else views,
                             coverage_size if args.coverage_poses else size)[:len(fitted.tris)]
            pruned, kept = prune(fitted, shown, args.budget)
            log(f"pruned {len(fitted.tris) - len(pruned.tris)} triangles: {np.count_nonzero(shown == 0)} never shown")
            fitted = pruned
        if copies is not None:
            carved = np.zeros(len(source.object_names), dtype=bool)
            carved[list(match_meshes(source.tri_o, source.tri_m, source.object_names, source.names,
                                     job.settings.meshes))] = True
            hidden = hidden_copy_pixels(source, carved[source.tri_o], (fit_start, fitted), args.copy_poses)
            log(check_hidden_copies(*hidden))
        count = write_mesh(out, name, fitted)
        report = stretch_report(fit_start, fitted, kept)
        for measure, threshold in (("stretch", STRETCH_RATIO), ("area_growth", AREA_GROWTH_RATIO), ("moved", MOVED_EDGES)):
            log(f"{measure} > {threshold:g}x: small parts {report['small'][measure]:.2f}%, rest {report['rest'][measure]:.2f}%")
            log(f"worst {measure} parts: " + ", ".join(f"{row['part']}: {row[measure]:.2f}% ({row['triangles']} triangles)"
                                                    for row in report["worst"][measure]))
        (out / "stretch.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        np.savetxt(out / "loss.txt", np.array(history), fmt="%.4f", header="mean dE76 before pruning")
        print(f"{out}: {count} triangles, mean dE76 before pruning {np.mean(history[:20]):.3f} -> {np.mean(history[-20:]):.3f}")
    # The fit's cached device memory goes back for a reference set traced next in this process.
    torch = sys.modules.get("torch")
    if torch is not None and torch.cuda.is_available():
        torch.cuda.empty_cache()
    return 0


if __name__ == "__main__":
    sys.exit(main())
