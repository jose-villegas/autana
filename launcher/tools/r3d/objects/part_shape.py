"""Compare each source part's front silhouette and relative Hausdorff distance.

Front looks down the thinnest principal axis. Relative Hausdorff is the greater
of the p99 distances from source part samples to the whole drawn mesh and from
assigned drawn triangle samples to the whole source mesh, over the part's bounds
diagonal.
"""
import hashlib
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw

from r3d.geometry import bounds_diagonal, directed_distance, grouped_indices, triangle_areas, triangle_search
from r3d.objects.object_error import main_materials, nearest_source, part_labels, part_names
from r3d.objects.objects_report import principal_view
from r3d.surface_samples import area_samples
from r3d.triangle_bounds import TriangleBounds

SAMPLE_COUNT = 256
SAMPLE_SEED = 42
HAUSDORFF_PERCENTILE = 99
MASK_SIDE = 64
MASK_MARGIN = 1.05
MIN_EXTENT = 1e-9


def front_iou(source, drawn):
    """Mask IoU of the part and its drawn triangles in the part's front view."""
    if not len(drawn):
        return 0.0
    vertices = np.unique(source.reshape(-1, 3), axis=0)
    centre = vertices.mean(axis=0)
    plane = principal_view(vertices, np.eye(3))[:, :2]
    span = max(np.abs((vertices - centre) @ plane).max(), MIN_EXTENT) * MASK_MARGIN
    masks = []
    for corners in (source, drawn):
        pixels = ((corners - centre) @ plane / (2 * span) + 0.5) * (MASK_SIDE - 1)
        image = Image.new("1", (MASK_SIDE, MASK_SIDE))
        draw = ImageDraw.Draw(image)
        for triangle in pixels:
            draw.polygon([tuple(vertex) for vertex in triangle], fill=1, outline=1)
        masks.append(np.asarray(image))
    union = np.count_nonzero(masks[0] | masks[1])
    return float(np.count_nonzero(masks[0] & masks[1]) / union) if union else 1.0


def part_samples(corners, rng):
    """Area samples of a part, or its first point when it has no area."""
    points = corners.reshape(-1, 3)
    triangles = np.arange(len(points)).reshape(-1, 3)
    samples = points[:1]
    if triangle_areas(points, triangles).any():
        samples, _ = area_samples(SimpleNamespace(p=points, tri_v=triangles), np.arange(len(triangles)),
                                  SAMPLE_COUNT, rng)
    return samples


def directed_p99(corners, target, rng, samples=None):
    """Distances from area samples to the target's nearest triangles, at p99."""
    if not len(corners):
        return 0.0
    points = part_samples(corners, rng) if samples is None else samples
    return directed_distance(points, target, percentile=HAUSDORFF_PERCENTILE)


def shape_measure(source, drawn, source_search=None, drawn_search=None, rng=None, source_samples=None):
    """Front IoU and relative Hausdorff for an isolated part or a part of two searched meshes."""
    if drawn_search is None and not len(drawn):
        return 0.0, None
    source_search = source_search or triangle_search(source)
    drawn_search = drawn_search or triangle_search(drawn)
    rng = np.random.default_rng(SAMPLE_SEED) if rng is None else rng
    distance = max(directed_p99(source, drawn_search, rng, source_samples),
                   directed_p99(drawn, source_search, rng))
    return front_iou(source, drawn), distance / max(bounds_diagonal(source.reshape(-1, 3)), MIN_EXTENT)


def measure_parts(source, drawn_p, drawn_tris, sample=None):
    """Measure all named source parts, including parts with no assigned drawn triangles."""
    p, tri_v, tri_o = source.p, source.tri_v, source.tri_o
    drawn_tris = np.asarray(drawn_tris, dtype=int).reshape(-1, 3)
    labels, _ = part_labels(p, tri_v, tri_o)
    names, owners, _ = part_names(labels, tri_o, source.object_names)
    materials = main_materials(labels, source.tri_m, source.names, len(names))
    drawn_labels = labels[nearest_source(p, tri_v, drawn_p, drawn_tris)]
    drawn_parts = grouped_indices(drawn_labels)
    source_corners, drawn_corners = p[tri_v], drawn_p[drawn_tris]
    source_search = TriangleBounds(source_corners)
    drawn_search = TriangleBounds(drawn_corners) if len(drawn_corners) else None
    rng = np.random.default_rng(SAMPLE_SEED)
    rows = []
    for part, indices in grouped_indices(labels).items():
        fitted = drawn_corners[drawn_parts[part]] if part in drawn_parts else drawn_corners[:0]
        corners = source_corners[indices]
        points = None
        if sample and drawn_search:
            part_digest = hashlib.sha256(corners.tobytes()).digest()
            points = sample(part_digest, rng, lambda: part_samples(corners, rng))
        iou, distance = shape_measure(corners, fitted, source_search, drawn_search, rng, points)
        rows.append(dict(part=names[part], source_object=source.object_names[owners[part]],
                         material=materials[part], iou=iou, hausdorff=distance))
    return rows
