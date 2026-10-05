"""Ray queries against a triangle mesh, traced by Mitsuba.

`light.py` and the importer ask three questions of a mesh: the first hit along each ray, whether a ray is blocked, and
every hit along a ray. `RayQuery` answers them with the signatures of the intersector it replaced, so those callers
only change where the object is built. Triangle ids are the row numbers of the `tris` given, and every triangle is hit
from both sides.
"""
import numpy as np

from r3d.mitsuba_reference import default_variant, import_mitsuba

# Rays per Mitsuba call: bounds the device arrays a large query allocates.
BATCH = 1 << 22
# Distance a multi-hit ray steps past a hit before it is traced again.
STEP = 1e-4
MAX_HITS = 64


class RayQuery:
    def __init__(self, positions, tris, variant=None):
        mi = import_mitsuba()
        if mi is None:
            raise RuntimeError("ray queries need Mitsuba: pip install -r launcher/tools/r3d/requirements.txt")
        if mi.variant() is None:
            mi.set_variant(variant or default_variant(mi))
        self.mi = mi
        self.positions, self.tris = np.asarray(positions, dtype=np.float64), np.asarray(tris, dtype=np.int64)
        self.scalar = mi.variant().startswith("scalar")
        mesh = mi.Mesh("query", len(positions), len(tris))
        params = mi.traverse(mesh)
        params["vertex_positions"] = np.asarray(positions, dtype=np.float32).ravel()
        params["faces"] = np.asarray(tris, dtype=np.uint32).ravel()
        params.update()
        self.scene = mi.load_dict({"type": "scene", "mesh": mesh})

    def first_hits(self, origins, directions):
        """(hit mask, distance along the direction, triangle) for every ray."""
        count = len(origins)
        hit, distance, tri = np.zeros(count, bool), np.zeros(count), np.zeros(count, np.int64)
        for start in range(0, count, BATCH):
            chunk = slice(start, start + BATCH)
            hit[chunk], distance[chunk], tri[chunk] = self._trace(origins[chunk], directions[chunk])
        return hit, distance, tri

    def _trace(self, origins, directions):
        mi = self.mi
        o, d = np.asarray(origins, np.float32), np.asarray(directions, np.float32)
        if self.scalar:
            found = np.zeros(len(o), bool)
            distance, tri = np.zeros(len(o)), np.zeros(len(o), np.int64)
            for index in range(len(o)):
                si = self.scene.ray_intersect(mi.Ray3f(mi.Point3f(*o[index]), mi.Vector3f(*d[index])))
                if si.is_valid():
                    found[index], distance[index], tri[index] = True, si.t, si.prim_index
            return found, distance, tri
        point = mi.Point3f(mi.Float(o[:, 0]), mi.Float(o[:, 1]), mi.Float(o[:, 2]))
        vector = mi.Vector3f(mi.Float(d[:, 0]), mi.Float(d[:, 1]), mi.Float(d[:, 2]))
        si = self.scene.ray_intersect(mi.Ray3f(point, vector))
        return np.array(si.is_valid(), bool), np.array(si.t, np.float64), np.array(si.prim_index, np.int64)

    def intersects_location(self, origins, directions, multiple_hits=False):
        """(locations, ray index, triangle) of each ray's first hit; rays that miss are left out."""
        if multiple_hits:
            raise ValueError("use intersects_id for every hit along a ray")
        origins, directions = np.asarray(origins, np.float64), np.asarray(directions, np.float64)
        hit, distance, tri = self.first_hits(origins, directions)
        ray = np.flatnonzero(hit)
        return origins[ray] + directions[ray] * distance[ray, None], ray, tri[ray]

    def intersects_first(self, origins, directions):
        """The first triangle each ray hits, -1 where it hits none."""
        hit, _, tri = self.first_hits(np.asarray(origins), np.asarray(directions))
        return np.where(hit, tri, -1)

    def intersects_any(self, origins, directions):
        """True for each ray that hits the mesh."""
        return self.first_hits(np.asarray(origins), np.asarray(directions))[0]

    def intersects_id(self, origins, directions, multiple_hits=True, return_locations=False):
        """(triangle, ray index[, location]) of every hit along each ray, nearest first, up to `MAX_HITS` per ray."""
        origins, directions = np.asarray(origins, np.float64), np.asarray(directions, np.float64)
        alive = np.arange(len(origins))
        start = origins.copy()
        tris, rays, locations = [], [], []
        for _ in range(MAX_HITS if multiple_hits else 1):
            if not len(alive):
                break
            hit, distance, tri = self.first_hits(start[alive], directions[alive])
            alive, distance, tri = alive[hit], distance[hit], tri[hit]
            where = start[alive] + directions[alive] * distance[:, None]
            tris.append(tri), rays.append(alive), locations.append(where)
            start[alive] = where + directions[alive] * STEP
        if not tris:
            tris, rays, locations = [np.zeros(0, np.int64)], [np.zeros(0, np.int64)], [np.zeros((0, 3))]
        result = (np.concatenate(tris), np.concatenate(rays))
        return result + (np.concatenate(locations),) if return_locations else result
