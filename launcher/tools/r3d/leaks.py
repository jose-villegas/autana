"""Where a simplified mesh lets the view past a surface its source closes, and the source vertices that close it.

A source built from pieces that only overlap is not one surface: wherever a simplified piece moves off its source,
at a border or across a curve, the piece behind may no longer cover the gap. `LeakProbes` looks at the source from
probes spread through the camera's region (views/probes.py's placement), each through six cube faces, and from
`path` views when given. In a view, a pixel is open where the simplified mesh shows sky the source covers, or where
the source hits and the mesh hits nothing or lies beyond BEHIND times the source's distance, in a region that holds
an OPENING square whole. Open pixels form connected regions; a region with no pixel of the source's own sky is a hole. `pins` answers each hole with one source vertex
to lock: the vertex of the source triangles seen through the hole that lies farthest from the simplified surface,
the farthest-point step of Douglas and Peucker's line reduction, taken on a surface and only where sky shows.
simplify.py locks the pins and simplifies again until no view has a hole, ROUNDS run out or the locks would
break the pass's budget.
"""

import numpy as np
from scipy import ndimage

from r3d.views import probes
from r3d.geometry import face_normals, nearest_triangles, triangle_search
from r3d.poses import camera_rays
from r3d.ray_query import LAUNCH, RayQuery

# The seed of the probes' jitter.
PROBE_SEED = 1
# A mesh hit this much farther than the source's is the view going past the surface, not the surface moved.
BEHIND = 1.5
# The square a region of pixels past the source must hold whole somewhere to count: silhouettes a pixel wide do not.
OPENING = 3
# Open pixels touching at a corner are one region.
CONNECTED = np.ones((3, 3), dtype=int)
# Most times a pass is simplified again; a pass that still leaks after these keeps its last accepted run.
ROUNDS = 24


class LeakProbes:
    """Views of a source mesh: cube faces from probes in `region` ((lo, hi), or the source's bounds when None),
    then the `path` views ((width, height, lens, poses) as views/probes.py's select takes them, or None)."""

    def __init__(self, positions, tris, region, near, path=None):
        self.positions, self.tris = np.asarray(positions, dtype=float), np.asarray(tris)
        lo, hi = region if region is not None else (self.positions.min(axis=0), self.positions.max(axis=0))
        self.query = RayQuery(self.positions, self.tris)
        poses, _ = probes.region_poses(self.query, face_normals(self.positions, self.tris)[0], lo, hi,
                                       probes.spacing_for(lo, hi, None, near), near, PROBE_SEED)
        face = probes.PROBE_FACE
        self.views = [(face, face, probes.FACE_LENS, pose) for pose in poses]
        if path is not None:
            width, height, lens, poses = path
            self.views += [(width, height, lens, pose) for pose in poses]
        self.source_hits = [self.query.first_hits(origins, directions)[:2] for _, origins, directions in self._batches()]
        self.source = np.ones(len(self.tris), dtype=bool)
        self.finished = []

    def _batches(self):
        """(views, origins, directions) for runs of views of at most LAUNCH rays, a view's rays in rows."""
        start = 0
        while start < len(self.views):
            end, rays = start, 0
            while end < len(self.views) and (end == start or rays + self.views[end][0] * self.views[end][1] <= LAUNCH):
                rays += self.views[end][0] * self.views[end][1]
                end += 1
            traced = [camera_rays(width, height, lens, pose[:3], pose[3:6], 1) for width, height, lens, pose in self.views[start:end]]
            yield self.views[start:end], np.concatenate([o for o, _ in traced]), np.concatenate([d for _, d in traced])
            start = end

    def holes(self, positions, tris):
        """Per hole a mesh shows, the source triangles seen through it."""
        result = RayQuery(positions, tris)
        holes = []
        for (views, origins, directions), (source_hit, source_distance) in zip(self._batches(), self.source_hits):
            hit, distance, _ = result.first_hits(origins, directions)
            past = source_hit & (~hit | (distance > BEHIND * np.where(source_hit, source_distance, 0.0)))
            if not np.any(past):
                continue
            at = 0
            for width, height, _, _ in views:
                rows = slice(at, at + width * height)
                at += width * height
                if not np.any(past[rows]):
                    continue
                shape = (height, width)
                masks = view_holes(open_pixels((~hit[rows]).reshape(shape), past[rows].reshape(shape)),
                                   (~source_hit[rows]).reshape(shape))
                if masks:
                    _, _, seen = self.query.first_hits(origins[rows], directions[rows])
                    holes.extend(np.unique(seen[mask.reshape(-1)]) for mask in masks)
        return holes

    def repair(self, rows, result_positions, result_tris, free):
        """(number of holes, vertices to lock) for a pass that simplified the source triangles `rows` into the result: the
        holes of the scene with the result in their place, among the source vertices `free` allows. The passes
        finished before it stand as their results, the rest of the scene as the source has it."""
        return self._look(rows, result_positions, result_tris, free)

    def finish(self, rows, result_positions, result_tris):
        """The source triangles `rows` are done, simplified by a pass or replaced by a carved mesh's stand-in: the
        result stands in the scene for the passes after it."""
        self.source[rows] = False
        self.finished.append((np.asarray(result_positions, dtype=float), np.asarray(result_tris)))

    def _look(self, rows, result_positions, result_tris, free):
        elsewhere = self.source.copy()
        elsewhere[rows] = False
        positions, tris, base = [self.positions], [self.tris[elsewhere]], len(self.positions)
        for done_positions, done_tris in [(result_positions, result_tris), *self.finished]:
            positions.append(done_positions)
            tris.append(done_tris + base)
            base += len(done_positions)
        holes = self.holes(np.concatenate(positions), np.concatenate(tris))
        return len(holes), pins(self.positions, self.tris, holes, result_positions, result_tris, free)


def open_pixels(sky, past):
    """A view's open pixels: where a mesh shows sky the source covers (`past` and `sky`), any of them, and the rest
    of `past` (where the source hits and the mesh misses or lies beyond BEHIND times it) that an OPENING square
    covers whole."""
    return (sky & past) | ndimage.binary_opening(past, np.ones((OPENING, OPENING), dtype=bool))


def view_holes(open_mask, source_sky):
    """The holes of one view, a mask each: the connected regions of `open_mask` (open pixels) that hold no pixel
    of `source_sky` (where the source shows sky). A region that reaches the source's sky is the mesh's outline
    moving, not a hole."""
    label, regions = ndimage.label(open_mask | source_sky, CONNECTED)
    return [label == region for region in np.setdiff1d(np.arange(1, regions + 1), label[source_sky])]


def pins(positions, tris, holes, result_positions, result_tris, free):
    """One vertex per hole, the farthest first: of the vertices of its triangles (rows of `tris`) that `free`
    allows, the one farthest from the result's surface. A hole with no free vertex gets none, and holes that choose
    one vertex share it."""
    search = triangle_search(result_positions[result_tris])
    picked = {}
    for seen in holes:
        candidates = np.unique(tris[seen])
        candidates = candidates[free[candidates]]
        if len(candidates):
            distance = nearest_triangles(positions[candidates], search)[2]
            picked[int(candidates[np.argmax(distance)])] = float(distance.max())
    return np.array(sorted(picked, key=lambda vertex: (-picked[vertex], vertex)), dtype=np.int64)
