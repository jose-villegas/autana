"""Compare each source part's front silhouette and relative Hausdorff distance.

Front looks down the thinnest principal axis. Relative Hausdorff is the greater
of the p99 distances from source part samples to the whole drawn mesh and from
assigned drawn triangle samples to the whole source mesh, over the part's bounds
diagonal.
"""
from collections import deque
from concurrent.futures import ProcessPoolExecutor
import hashlib
from itertools import batched, islice
import multiprocessing
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw

from r3d.geometry import bounds_diagonal, directed_distance, grouped_indices, triangle_areas, triangle_search
from r3d.objects.object_error import main_materials, nearest_source, part_labels, part_names
from r3d.objects.objects_report import principal_view
from r3d.objects.part_surface import PartSurface
from r3d.surface_samples import area_samples

SAMPLE_COUNT = 256
SAMPLE_SEED = 42
HAUSDORFF_PERCENTILE = 99
MASK_SIDE = 64
MASK_MARGIN = 1.05
MIN_EXTENT = 1e-9
PARTS_PER_QUERY = 32
BATCHES_PER_WORKER = 4


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


def directed_p99(corners, target, rng):
    """Distances from area samples to the target's nearest triangles, at p99."""
    if not len(corners):
        return 0.0
    points = part_samples(corners, rng)
    return directed_distance(points, target, percentile=HAUSDORFF_PERCENTILE)


def relative_distance(source, there, back):
    return max(there, back) / max(bounds_diagonal(source.reshape(-1, 3)), MIN_EXTENT)


def shape_measure(source, drawn, source_search=None, drawn_search=None, rng=None):
    """Front IoU and relative Hausdorff for an isolated part or a part of two searched meshes."""
    if drawn_search is None and not len(drawn):
        return 0.0, None
    source_search = source_search or triangle_search(source)
    drawn_search = drawn_search or triangle_search(drawn)
    rng = np.random.default_rng(SAMPLE_SEED) if rng is None else rng
    there = directed_p99(source, drawn_search, rng)
    back = directed_p99(drawn, source_search, rng)
    return front_iou(source, drawn), relative_distance(source, there, back)


def part_surfaces(source_p, source_tris, drawn_p, drawn_tris):
    return PartSurface(source_p, source_tris), PartSurface(drawn_p, drawn_tris)


def init_part_worker(*meshes):
    global _worker_surfaces
    _worker_surfaces = part_surfaces(*meshes)


def part_batch(batch, surfaces=None):
    source_search, drawn_search = _worker_surfaces if surfaces is None else surfaces
    part_ids, corners, fitted, forward, reverse = zip(*batch)
    there = directed_distance(np.concatenate(forward), drawn_search, HAUSDORFF_PERCENTILE,
                              groups=list(map(len, forward)))
    back = directed_distance(np.concatenate(reverse), source_search, HAUSDORFF_PERCENTILE,
                             groups=list(map(len, reverse)))
    return [(part, front_iou(model, kept), relative_distance(model, a, b))
            for part, model, kept, a, b in zip(part_ids, corners, fitted, there, back, strict=True)]


def measure_parts(source, drawn_p, drawn_tris, sample=None, workers=1):
    """Measure all named source parts, including parts with no assigned drawn triangles."""
    p, tri_v, tri_o = source.p, source.tri_v, source.tri_o
    drawn_tris = np.asarray(drawn_tris, dtype=int).reshape(-1, 3)
    labels, _ = part_labels(p, tri_v, tri_o)
    names, owners, _ = part_names(labels, tri_o, source.object_names)
    materials = main_materials(labels, source.tri_m, source.names, len(names))
    drawn_labels = labels[nearest_source(p, tri_v, drawn_p, drawn_tris)]
    drawn_parts = grouped_indices(drawn_labels)
    source_corners, drawn_corners = p[tri_v], drawn_p[drawn_tris]
    rng = np.random.default_rng(SAMPLE_SEED)
    def parts():
        for part, indices in grouped_indices(labels).items():
            corners = source_corners[indices]
            fitted = drawn_corners[drawn_parts[part]] if part in drawn_parts else drawn_corners[:0]
            yield part, corners, fitted

    def samples():
        for part, corners, fitted in parts():
            if sample:
                part_digest = hashlib.sha256(corners.tobytes()).digest()
                points = sample(part_digest, rng, lambda: part_samples(corners, rng))
            else:
                points = part_samples(corners, rng)
            reverse = part_samples(fitted, rng) if len(fitted) else fitted.reshape(-1, 3)
            yield part, corners, fitted, points, reverse

    def row(part, iou, distance):
        return dict(part=names[part], source_object=source.object_names[owners[part]],
                    material=materials[part], iou=iou, hausdorff=distance)

    if not len(drawn_corners):
        return [row(part, 0.0, None) for part, _, _ in parts()]
    meshes = (p, tri_v, drawn_p, drawn_tris)
    batches = batched(samples(), PARTS_PER_QUERY)
    if workers == 1:
        surfaces = part_surfaces(*meshes)
        return [row(*result) for batch in batches for result in part_batch(batch, surfaces)]

    rows = []
    context = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(max_workers=workers, mp_context=context, initializer=init_part_worker,
                             initargs=meshes) as pool:
        pending = deque(pool.submit(part_batch, batch) for batch in islice(batches, workers * BATCHES_PER_WORKER))
        for batch in batches:
            rows.extend(row(*result) for result in pending.popleft().result())
            pending.append(pool.submit(part_batch, batch))
        for future in pending:
            rows.extend(row(*result) for result in future.result())
    return rows
