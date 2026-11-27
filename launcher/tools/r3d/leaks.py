"""Sky a simplified mesh shows through its walls, and the source vertices that close it.

A source built from pieces that only overlap is not one surface: wherever a simplified piece moves off its source,
at a border or across a curve, the piece behind may no longer cover the gap. `LeakProbes` looks at the source from
probes spread through the camera's region (views/probes.py's placement), each through six cube faces.
Holes are connected regions of flagged sky pixels. A sky pixel is flagged if its region
holds no source sky or it lies more than REACH probe pixels from the source's sky. `pins` answers each hole with one
source vertex to lock: the vertex of the source triangles seen through the hole that lies farthest from the simplified surface,
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
# Sky pixels touching at a corner are one region.
CONNECTED = np.ones((3, 3), dtype=int)
# Simplification can move an outline by this many probe pixels.
# Sky beyond that reach is a hole even if it joins the source's sky.
REACH = 3
# Most times a pass is simplified again; a pass that still leaks after these keeps its last accepted run.
ROUNDS = 24


class LeakProbes:
    """Cube faces from probes in `region` ((lo, hi), or the source's bounds when None)."""

    def __init__(self, positions, tris, region, near):
        self.positions, self.tris = np.asarray(positions, dtype=float), np.asarray(tris)
        lo, hi = region if region is not None else (self.positions.min(axis=0), self.positions.max(axis=0))
        self.query = RayQuery(self.positions, self.tris)
        poses, _ = probes.region_poses(self.query, face_normals(self.positions, self.tris)[0], lo, hi,
                                       probes.spacing_for(lo, hi, None, near), near, PROBE_SEED)
        face = probes.PROBE_FACE
        self.views = [(face, face, probes.FACE_LENS, pose) for pose in poses]
        self.source_sky = [~self.query.first_hits(origins, directions)[0] for _, origins, directions in self._batches()]
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

    def holes(self, positions, tris, reach=REACH):
        """Per hole a mesh shows, the source triangles seen through it."""
        holes = []
        for origins, directions, masks in self._view_holes(RayQuery(positions, tris), reach):
            hit, _, seen = self.query.first_hits(origins, directions)
            holes.extend(np.unique(seen[mask.reshape(-1) & hit]) for mask in masks)
        return holes

    def rims(self, positions, tris, width=1, reach=REACH):
        """Per hole a mesh shows, the rows of its `tris` seen within `width` pixels around the hole."""
        result = RayQuery(positions, tris)
        rims = []
        for origins, directions, masks in self._view_holes(result, reach):
            hit, _, seen = result.first_hits(origins, directions)
            for mask in masks:
                ring = (ndimage.binary_dilation(mask, CONNECTED, iterations=width) & ~mask).reshape(-1)
                rims.append(np.unique(seen[ring & hit]))
        return rims

    def closed_gap_hits(self, positions, tris):
        """Batches of rays through fully covered source-sky regions, with their first triangle rows."""
        result = RayQuery(positions, tris)
        gaps = []
        for (views, origins, directions), source_sky in zip(self._batches(), self.source_sky):
            if not np.any(source_sky):
                continue
            sky_origins, sky_directions = origins[source_sky], directions[source_sky]
            hit, _, rows = result.first_hits(sky_origins, sky_directions)
            if not np.any(hit):
                continue
            remaining_sky = source_sky.copy()
            remaining_sky[source_sky] = ~hit
            closed = np.zeros_like(source_sky)
            at = 0
            for width, height, _, _ in views:
                view = slice(at, at + width * height)
                at += width * height
                shape = (height, width)
                closed[view] = _unshared_sky(source_sky[view].reshape(shape),
                                             remaining_sky[view].reshape(shape)).ravel()
            keep = closed[source_sky]
            if np.any(keep):
                gaps.append((sky_origins[keep], sky_directions[keep], rows[keep]))
        return gaps

    def reopened_gap_rows(self, positions, tris, gaps):
        """Original first-hit rows for closed gap rays that now see sky."""
        result = RayQuery(positions, tris)
        return np.unique(np.concatenate([rows[~result.blocked(origins, directions)]
                                         for origins, directions, rows in gaps]))

    def _view_holes(self, result, reach=REACH):
        """(origins, directions, hole masks) of each view where the RayQuery `result` shows a hole."""
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
                masks = view_holes(sky[rows].reshape(height, width), source_sky[rows].reshape(height, width), reach=reach)
                if masks:
                    yield origins[rows], directions[rows], masks

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


def _unshared_sky(sky, other_sky):
    """Mask of connected sky regions with no pixels in the other sky mask."""
    label, regions = ndimage.label(sky, CONNECTED)
    isolated = np.ones(regions + 1, dtype=bool)
    isolated[0] = False
    isolated[label[other_sky]] = False
    return isolated[label]


def view_holes(sky, source_sky, reach=REACH):
    """One mask per connected hole in a view. Combine sky regions holding no source sky with sky pixels
    whose chessboard distance from the nearest source sky pixel exceeds `reach`."""
    holes = _unshared_sky(sky, source_sky)
    if source_sky.any():
        distance = ndimage.distance_transform_cdt(~source_sky, metric="chessboard")
        holes |= sky & (distance > reach)
    label, regions = ndimage.label(holes, CONNECTED)
    return [label == region for region in range(1, regions + 1)]


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
