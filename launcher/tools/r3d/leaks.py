"""Sky a simplified mesh shows through its walls, and the source vertices that close it.

A source built from pieces that only overlap is not one surface: wherever a simplified piece moves off its source,
at a border or across a curve, the piece behind may no longer cover the gap. `LeakProbes` looks at the source from
probes spread through the camera's region (fit/probes.py's placement), each through six cube faces, and from the
camera path's poses when the scene has one. In a view, the pixels where a simplified mesh shows sky form connected
regions; a region with no pixel of the source's own sky is a hole. `pins` answers each hole with one source vertex
to lock: the vertex of the source triangles seen through the hole that lies farthest from the simplified surface,
the farthest-point step of Douglas and Peucker's line reduction, taken on a surface and only where sky shows.
simplify.py locks the pins and simplifies again until no view has a hole.
"""

import numpy as np
from scipy import ndimage

from r3d.fit import probes
from r3d.geometry import face_normals, nearest_triangles, triangle_search
from r3d.poses import camera_rays
from r3d.ray_query import RayQuery

# The seed of the probes' jitter.
PROBE_SEED = 1
# Rays traced in one batch of views.
RAYS_PER_BATCH = 1 << 22
# Sky pixels touching at a corner are one region.
CONNECTED = np.ones((3, 3), dtype=int)
# Most times a pass is simplified again: each round halves the holes or better on the meshes measured, and a
# pass that still leaks after these keeps what it has.
ROUNDS = 24


class LeakProbes:
    """Views of a source mesh: cube faces from probes in `region` ((lo, hi), or the source's bounds when None),
    then the `path` views ((width, height, lens, poses) as fit/probes.py's select takes them, or None)."""

    def __init__(self, positions, tris, region, near, path=None):
        self.positions, self.tris = np.asarray(positions, dtype=float), np.asarray(tris)
        lo, hi = region if region is not None else (self.positions.min(axis=0), self.positions.max(axis=0))
        self.query = RayQuery(self.positions, self.tris)
        points, cell = probes.probe_points(lo, hi, probes.spacing_for(lo, hi, None, near), PROBE_SEED)
        moved, state = probes.classify(self.query, face_normals(self.positions, self.tris)[0], points, cell, near)
        face = probes.PROBE_FACE
        self.views = [(face, face, probes.FACE_LENS, pose) for pose in probes.cube_poses(moved[state == "kept"])]
        if path is not None:
            width, height, lens, poses = path
            self.views += [(width, height, lens, pose) for pose in poses]
        self.source_sky = [~self.query.first_hits(origins, directions)[0] for _, origins, directions in self._batches()]

    def _batches(self):
        """(views, origins, directions) for runs of views of at most RAYS_PER_BATCH rays, a view's rays in rows."""
        start = 0
        while start < len(self.views):
            end, rays = start, 0
            while end < len(self.views) and (end == start or rays + self.views[end][0] * self.views[end][1] <= RAYS_PER_BATCH):
                rays += self.views[end][0] * self.views[end][1]
                end += 1
            traced = [camera_rays(width, height, lens, pose[:3], pose[3:6], 1) for width, height, lens, pose in self.views[start:end]]
            yield self.views[start:end], np.concatenate([o for o, _ in traced]), np.concatenate([d for _, d in traced])
            start = end

    def holes(self, positions, tris):
        """Per hole a mesh shows, the source triangles seen through it."""
        result = RayQuery(positions, tris)
        holes = []
        for (views, origins, directions), source_sky in zip(self._batches(), self.source_sky):
            sky = ~result.blocked(origins, directions)
            if not np.any(sky & ~source_sky):
                continue
            at = 0
            for width, height, _, _ in views:
                rows = slice(at, at + width * height)
                at += width * height
                if not np.any(sky[rows] & ~source_sky[rows]):
                    continue
                masks = view_holes(sky[rows].reshape(height, width), source_sky[rows].reshape(height, width))
                if masks:
                    _, _, seen = self.query.first_hits(origins[rows], directions[rows])
                    holes.extend(np.unique(seen[mask.reshape(-1)]) for mask in masks)
        return holes

    def pins(self, rows, result_positions, result_tris, free):
        """(holes, vertices to lock) for a pass that simplified the source triangles `rows` into the result: the
        holes of the scene with the result in their place, among the source vertices `free` allows. The rest of
        the scene stands as the source has it, so every triangle seen through a hole is one of the pass's."""
        elsewhere = np.ones(len(self.tris), dtype=bool)
        elsewhere[rows] = False
        scene_tris = np.concatenate([result_tris, self.tris[elsewhere] + len(result_positions)])
        holes = self.holes(np.concatenate([result_positions, self.positions]), scene_tris)
        return len(holes), pins(self.positions, self.tris, holes, result_positions, result_tris, free)


def view_holes(sky, source_sky):
    """The holes of one view, a mask each: the connected regions of `sky` (pixels where a mesh shows sky) that
    hold no pixel of `source_sky` (where the source shows it too). A region that reaches the source's sky is the
    mesh's outline moving, not a hole."""
    label, regions = ndimage.label(sky, CONNECTED)
    return [label == region for region in np.setdiff1d(np.arange(1, regions + 1), label[source_sky])]


def pins(positions, tris, holes, result_positions, result_tris, free):
    """One vertex per hole: of the vertices of its triangles (rows of `tris`) that `free` allows, the one farthest
    from the result's surface. A hole with no free vertex gets none."""
    search = triangle_search(result_positions[result_tris])
    picked = set()
    for seen in holes:
        candidates = np.unique(tris[seen])
        candidates = candidates[free[candidates]]
        if len(candidates):
            distance = nearest_triangles(positions[candidates], search)[2]
            picked.add(int(candidates[np.argmax(distance)]))
    return np.array(sorted(picked), dtype=np.int64)
