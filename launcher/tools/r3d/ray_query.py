"""Ray queries against a triangle mesh, traced by Mitsuba.

`light.py` and the importer ask three questions of a mesh: the first hit along each ray, whether a ray is blocked, and
every hit along a ray. `RayQuery` answers them as `first_hit`, `blocked` and `all_hits`, and `first_hits` gives
the raw arrays behind the first. Triangle ids are the row numbers of the `tris` given, and every triangle
is hit from both sides.

The bake is NumPy-bound on the CPU, so the queries run on the LLVM variant: a CUDA context per forked pose worker
would cost memory and copies for nothing, and the JIT of a process that forks workers must not be CUDA. Tests set
`VARIANT` to `scalar_rgb`, which needs no libLLVM and traces one ray at a time. A worker forked after a query
inherits the scene and traces serially on its own thread.
"""
import sys

import numpy as np

from r3d.mitsuba_reference import import_mitsuba

VARIANT = "llvm_ad_rgb"
# Rays per Mitsuba call: bounds the arrays a large query allocates.
BATCH = 1 << 22
# Distance a multi-hit ray steps past a hit before it is traced again: the larger of STEP and STEP_RELATIVE times
# the hit's largest coordinate (a few float32 spacings there), so a far scene does not hit a triangle twice.
STEP = 1e-4
STEP_RELATIVE = 1e-6
MAX_HITS = 100


class RayQuery:
    def __init__(self, positions, tris, variant=None):
        variant = variant or VARIANT
        mi = import_mitsuba()
        if mi is None:
            raise RuntimeError("ray queries need Mitsuba: pip install -r launcher/tools/r3d/requirements.txt")
        if variant.startswith("llvm"):
            import drjit as dr

            if not dr.has_backend(dr.JitBackend.LLVM):
                raise RuntimeError("ray queries need libLLVM (apt install llvm, or the LLVM runtime for your platform)")
        mi.set_variant(variant)
        self.mi, self.variant, self.scalar = mi, variant, variant.startswith("scalar")
        self.positions, self.tris = np.asarray(positions, dtype=np.float64), np.asarray(tris, dtype=np.int64)
        mesh = mi.Mesh("query", len(self.positions), len(self.tris))
        params = mi.traverse(mesh)
        params["vertex_positions"] = self.positions.astype(np.float32).ravel()
        params["faces"] = self.tris.astype(np.uint32).ravel()
        params.update()
        self.scene = mi.load_dict({"type": "scene", "mesh": mesh})

    def _rays(self, origins, directions):
        mi = self.mi
        o, d = np.asarray(origins, np.float32), np.asarray(directions, np.float32)
        point = mi.Point3f(mi.Float(o[:, 0]), mi.Float(o[:, 1]), mi.Float(o[:, 2]))
        vector = mi.Vector3f(mi.Float(d[:, 0]), mi.Float(d[:, 1]), mi.Float(d[:, 2]))
        return mi.Ray3f(point, vector)

    def _check_variant(self):
        if self.mi.variant() != self.variant:
            raise RuntimeError(f"the ray query was built on {self.variant} but Mitsuba is now on {self.mi.variant()}")

    def first_hits(self, origins, directions):
        """(hit mask, distance along the direction, triangle) for every ray; distance and triangle mean nothing where
        the mask is false."""
        self._check_variant()
        count = len(origins)
        hit, distance, tri = np.zeros(count, bool), np.zeros(count), np.zeros(count, np.int64)
        for start in range(0, count, BATCH):
            chunk = slice(start, start + BATCH)
            hit[chunk], distance[chunk], tri[chunk] = self._trace(origins[chunk], directions[chunk])
        return hit, distance, tri

    def _trace(self, origins, directions):
        if self.scalar:
            mi, o, d = self.mi, np.asarray(origins, np.float32), np.asarray(directions, np.float32)
            found, distance, tri = np.zeros(len(o), bool), np.zeros(len(o)), np.zeros(len(o), np.int64)
            for index in range(len(o)):
                si = self.scene.ray_intersect(mi.Ray3f(mi.Point3f(*o[index]), mi.Vector3f(*d[index])))
                if si.is_valid():
                    found[index], distance[index], tri[index] = True, si.t, si.prim_index
            return found, distance, tri
        import drjit as dr

        hit = self.scene.ray_intersect_preliminary(self._rays(origins, directions))
        valid = hit.is_valid()
        dr.eval(valid, hit.t, hit.prim_index)
        return np.array(valid, bool), np.array(hit.t, np.float64), np.array(hit.prim_index, np.int64)

    def first_hit(self, origins, directions):
        """(locations, ray index, triangle) of each ray's first hit; rays that miss are left out."""
        origins, directions = np.asarray(origins, np.float64), np.asarray(directions, np.float64)
        hit, distance, tri = self.first_hits(origins, directions)
        ray = np.flatnonzero(hit)
        return origins[ray] + directions[ray] * distance[ray, None], ray, tri[ray]

    def blocked(self, origins, directions):
        """True for each ray that hits the mesh."""
        if self.scalar:
            return self.first_hits(np.asarray(origins), np.asarray(directions))[0]
        self._check_variant()
        import drjit as dr

        blocked = np.zeros(len(origins), bool)
        for start in range(0, len(origins), BATCH):
            chunk = slice(start, start + BATCH)
            mask = self.scene.ray_test(self._rays(np.asarray(origins)[chunk], np.asarray(directions)[chunk]))
            dr.eval(mask)
            blocked[chunk] = np.array(mask, bool)
        return blocked

    def all_hits(self, origins, directions):
        """(triangle, ray index, location) of every hit along each ray, in rounds: every ray's first hit, then every
        second hit, and so on, so one ray's hits run nearest first. A ray still alive after `MAX_HITS` is cut off and
        reported on stderr."""
        origins, directions = np.asarray(origins, np.float64), np.asarray(directions, np.float64)
        alive = np.arange(len(origins))
        start = origins.copy()
        tris, rays, locations = [], [], []
        for _ in range(MAX_HITS):
            if not len(alive):
                break
            hit, distance, tri = self.first_hits(start[alive], directions[alive])
            alive, distance, tri = alive[hit], distance[hit], tri[hit]
            where = start[alive] + directions[alive] * distance[:, None]
            tris.append(tri), rays.append(alive), locations.append(where)
            step = np.maximum(STEP, STEP_RELATIVE * np.abs(where).max(axis=1, initial=0.0))
            start[alive] = where + directions[alive] * step[:, None]
        else:
            if len(alive):
                print(f"ray query: {len(alive)} rays still crossing triangles after {MAX_HITS} hits were cut off", file=sys.stderr)
        if not tris:
            tris, rays, locations = [np.zeros(0, np.int64)], [np.zeros(0, np.int64)], [np.zeros((0, 3))]
        return np.concatenate(tris), np.concatenate(rays), np.concatenate(locations)
