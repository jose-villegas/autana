"""The reference G-buffer of the fit and the two losses on it: depth and "a copy shows here", per training view,
from the full-detail source: masked sources are traced per hit, opaque sources rasterised.

The front loss is the copy-coverage gate's measure made soft: wherever the source shows a copy and the fitted
rest is drawn in front of it, it pushes the rest back behind. The depth loss holds the rest to the source's own
surface where the source shows the rest, against sheets dragged across grazing views (MeshSplatting's depth term).
"""

from typing import NamedTuple

import numpy as np

# A fitted pixel stands in front of the source when it is nearer than the source by more than this fraction of
# the source's depth.
IN_FRONT = 0.10


def distance(render, points, matrix, raster=None):
    """Per pixel, the drawn surface's distance along the view axis (inf where nothing is drawn), differentiable in
    `points`, and the triangle it shows (-1 where none). `raster` is the view's render.raster, when made already."""
    import torch

    clip, rast, keep = render.raster(points, matrix) if raster is None else raster
    local = rast[0, ..., 3].long() - 1
    drawn = local >= 0
    # Clip w is the view-axis distance, affine in view space, so its perspective-correct interpolation is exact.
    w = render.interpolate(clip[:, 3:], clip, rast, keep)[..., 0]
    return torch.where(drawn, w, torch.inf), torch.where(drawn, keep[local.clamp_min(0)], -1)


class ReferenceBuffers(NamedTuple):
    depth: list  # per view, [height, width] source distance along the view axis, inf where nothing is drawn
    copy: list  # per view, [height, width] bool: the source shows a carved triangle


def traced_buffer(query, matrix, size):
    """Depth and source triangle per pixel in raster row order, through alpha-tested hits, clipped to the view."""
    width, height = size
    x, y = np.meshgrid((2 * np.arange(width) + 1) / width - 1, (2 * np.arange(height) + 1) / height - 1)
    pixels = np.column_stack([x.ravel(), y.ravel(), np.full(x.size, -1.0), np.ones(x.size)])
    inverse = np.linalg.inv(matrix)
    near = pixels @ inverse.T
    pixels[:, 2] = 0.0
    ahead = pixels @ inverse.T
    origin = near[:, :3] / near[:, 3:]
    direction = ahead[:, :3] / ahead[:, 3:] - origin
    direction /= np.linalg.norm(direction, axis=1, keepdims=True)
    where, rays, faces = query.first_hit(origin, direction)
    clip = np.column_stack([where, np.ones(len(where))]) @ matrix.T
    valid = (clip[:, 2] >= -clip[:, 3]) & (clip[:, 2] <= clip[:, 3])
    depth, shown = np.full(x.size, np.inf), np.full(x.size, -1, dtype=np.int64)
    depth[rays[valid]], shown[rays[valid]] = clip[valid, 3], faces[valid]
    return depth.reshape(height, width), shown.reshape(height, width)


def reference_buffers(source, carved, matrices, sizes, device="cuda"):
    """The source's buffers at each view: `carved` flags each source triangle drawn as a copy, `matrices` are the
    views' clip matrices and `sizes` their (width, height)."""
    import torch

    from r3d.appearance_simplify import Renderer

    alpha = getattr(source, "alpha_test", None)
    query = None
    if alpha is not None and alpha.masked.any():
        from r3d.ray_query import RayQuery

        query = RayQuery(source.p, source.tri_v, hit_filter=alpha)
    points = torch.as_tensor(source.p, dtype=torch.float32, device=device)
    carved = torch.as_tensor(carved, device=device)
    renders, depth, copy = {}, [], []
    with torch.no_grad():
        for matrix, size in zip(matrices, sizes):
            if query is not None:
                reference, shown = traced_buffer(query, matrix.detach().cpu().numpy(), size)
                reference = torch.as_tensor(reference, dtype=points.dtype, device=device)
                shown = torch.as_tensor(shown, device=device)
            else:
                if size not in renders:
                    renders[size] = Renderer(source.tri_v, np.ones(len(source.tri_v), dtype=np.int64),
                                             np.arange(len(source.p)), size, device)
                reference, shown = distance(renders[size], points, matrix)
            depth.append(reference)
            copy.append((shown >= 0) & carved[shown.clamp_min(0)])
    return ReferenceBuffers(depth, copy)


def front_loss(fitted, rest_shown, reference, copy):
    """Over the frame, how far the rest stands in front of the copies the source shows, relative to their depth."""
    import torch

    shown = rest_shown & copy
    near = reference * (1 - IN_FRONT)
    return torch.where(shown, torch.relu(near - fitted) / torch.where(shown, reference, 1), 0).mean()


def depth_loss(fitted, rest_shown, reference, copy):
    """Over the frame, how far the rest strays from the source's depth where the source shows the rest."""
    import torch

    shown = rest_shown & ~copy & torch.isfinite(reference)
    return torch.where(shown, (fitted - reference).abs() / torch.where(shown, reference, 1), 0).mean()
