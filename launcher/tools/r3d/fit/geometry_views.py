"""Independent close probe views for the fit's front and depth terms, without colour references.

Copies need protection from rest movement even where the colour training views cannot see them. These views
reuse probe placement and the source's alpha-aware G-buffers; frustum rejection and small screening buffers bound
the cost of tracing masked sources before the kept faces get their full buffers.
"""

import time
from typing import NamedTuple

import numpy as np

from r3d import log
from r3d.cost_model import clusters_in_view
from r3d.fit import gbuffer
from r3d.geometry import face_normals
from r3d.mesh_source import camera_filter
from r3d.ray_query import RayQuery
from r3d.reference_set import PROBE_SEED
from r3d.views import probes

# Half the training spacing brings geometry views closer to copies.
SPACING_SHARE = 0.5
# A separate placement seed must differ from training and held-out (PROBE_SEED + 1).
SEED_OFFSET = 2
# Bound classification work even when the finer grid fills a large region.
PROBE_COUNT_MAX = 4096
# Bound the CPU rays spent screening masked sources after cheap frustum rejection.
CANDIDATE_COUNT_MAX = 256
# Small buffers screen occluded copies before tracing the full geometry faces.
SELECT_SIZE = 16
# Bound stored buffers and the full-resolution CPU tracing cost independently of colour views. 64 left Sponza's
# columns free to cover the plants in one held-out view once its fabric was a single sheet.
FACE_COUNT_MAX = 128
# Close geometry views need less resolution than colour references.
FACE_SIZE = 128
# A separate draw leaves the colour training sequence unchanged.
BATCH_SEED_OFFSET = 2


class GeometryViews(NamedTuple):
    poses: list
    matrices: list
    buffers: gbuffer.ReferenceBuffers
    size: tuple = (FACE_SIZE, FACE_SIZE)


def select(source, carved, region, near, spacing, device="cuda"):
    """Capped copy-visible faces from an independent, finer sample of the reference probes' region."""
    import torch

    from r3d.appearance_simplify import projection

    started = time.perf_counter()
    poses, matrices = [], []
    buffers = gbuffer.ReferenceBuffers([], [])
    if np.any(carved):
        lo, hi = region if region is not None else (source.p.min(axis=0), source.p.max(axis=0))
        near = np.linalg.norm(np.ptp(source.p, axis=0)) * probes.PROBE_NEAR_SHARE if near is None else near
        if near <= 0:
            raise ValueError("probe source bounds have no extent")
        spacing = probes.spacing_for(lo, hi, spacing, near) * SPACING_SHARE
        seed = PROBE_SEED + SEED_OFFSET
        rng = np.random.default_rng(seed)
        points, cell = probes.probe_points(lo, hi, spacing, seed)
        if len(points) > PROBE_COUNT_MAX:
            points = points[rng.choice(len(points), PROBE_COUNT_MAX, replace=False)]
        query = RayQuery(source.p, source.tri_v, hit_filter=camera_filter(source))
        points, state = probes.classify(query, face_normals(source.p, source.tri_v)[0], points, cell, near)
        candidates = probes.cube_poses(points[state == "kept"])
        corners = source.p[source.tri_v[np.asarray(carved, dtype=bool)]]
        boxes = np.stack([corners.min(axis=1), corners.max(axis=1)], axis=1)
        for index in rng.permutation(len(candidates)):
            pose = candidates[index]
            matrix = projection(pose[:3], pose[3:], FACE_SIZE, FACE_SIZE, probes.FACE_LENS, near)
            if clusters_in_view(boxes, matrix).any():
                poses.append(pose)
                matrices.append(torch.as_tensor(matrix, dtype=torch.float32, device=device))
                if len(poses) == CANDIDATE_COUNT_MAX:
                    break
    buffer_started = time.perf_counter()
    small = (gbuffer.reference_buffers(source, carved, matrices, [(SELECT_SIZE, SELECT_SIZE)] * len(matrices), device)
             if matrices else buffers)
    kept = [i for i, mask in enumerate(small.copy) if mask.any().item()][:FACE_COUNT_MAX]
    poses, matrices = [poses[i] for i in kept], [matrices[i] for i in kept]
    if matrices:
        full = gbuffer.reference_buffers(source, carved, matrices, [(FACE_SIZE, FACE_SIZE)] * len(matrices), device)
        kept = [i for i, mask in enumerate(full.copy) if mask.any().item()]
        poses, matrices = [poses[i] for i in kept], [matrices[i] for i in kept]
        buffers = gbuffer.ReferenceBuffers([full.depth[i] for i in kept], [full.copy[i] for i in kept])
    log(f"geometry views: {len(poses)}; buffers {time.perf_counter() - buffer_started:.2f}s; "
        f"selection total {time.perf_counter() - started:.2f}s")
    return GeometryViews(poses, matrices, buffers)
