"""Ray queries against a triangle mesh, traced by Mitsuba on Dr.Jit.

`light.py`, the importer and the reference trace ask three questions of a mesh: the first hit along each ray,
whether a ray is blocked, and hit rounds a caller can retire rays from. `RayQuery` answers them on Dr.Jit arrays
(`device_hits`, `device_blocked`), where the reference trace keeps a whole pose on the device, and on NumPy arrays
(`first_hit`, `first_hits`, `blocked`, `hit_rounds`) for callers that hold their rays on the host. Triangle ids are
the row numbers of the `tris` given, and every triangle is hit from both sides. An optional `hit_filter` (a
light.AlphaTest) says which hits are solid; every query steps past the rest on the device, so a cut-out texel hides
nothing behind it.

A query traces on the LLVM variant `VARIANT` unless its caller names another: Embree builds one tree per mesh, so a
mesh bake repeats byte for byte on one CPU model. A reference set names the fastest variant instead, CUDA on a GPU
(reference_render.reference_variant; README, "Reference on the GPU"). Both are JIT variants: the queries run as
Dr.Jit kernels of at most `LAUNCH` rays, a constant, so a result never depends on the device's memory.
"""
import sys
from typing import NamedTuple

import numpy as np

from r3d.mitsuba_reference import import_mitsuba

VARIANT = "llvm_ad_rgb"


def is_jit(variant):
    """True for a Mitsuba variant whose arrays are Dr.Jit kernels (LLVM or CUDA)."""
    return (variant or "").startswith(("llvm", "cuda"))


# Rays per Dr.Jit launch: bounds the arrays a large query allocates, and is large enough to keep a GPU busy.
LAUNCH = 1 << 22
# Distance a multi-hit ray steps past a hit before it is traced again: the larger of STEP and STEP_RELATIVE times
# the hit's largest coordinate (a few float32 spacings there), so a far scene does not hit a triangle twice.
STEP = 1e-4
STEP_RELATIVE = 1e-6
MAX_HITS = 100


class DeviceHit(NamedTuple):
    """The first solid hit of each ray, as Dr.Jit arrays: whether there is one, the distance along the ray's
    direction, the triangle and the hit location. Distance, triangle and location mean nothing where `valid` is
    false."""
    valid: object
    t: object
    prim: object
    where: object


def true_count(dr, mask):
    """How many entries of a Dr.Jit mask are set, as a Python int."""
    found = dr.count(mask)
    return int(found[0] if dr.is_array_v(found) else found)


def vectors(kind, rows):
    """NumPy (n, 3) rows as a Dr.Jit 3-vector of `kind` (mi.Point3f, mi.Vector3f, mi.Color3f)."""
    rows = np.asarray(rows, np.float32).reshape(-1, 3)
    float_type = type(kind(0).x)
    return kind(*(float_type(rows[:, axis]) for axis in range(3)))


def rows(value, count, channels=3):
    """A Dr.Jit vector of `channels` components as NumPy (count, channels) float64 rows."""
    return np.broadcast_to(np.array(value, np.float64).reshape(channels, -1).T, (count, channels)).copy()


def joined(parts):
    """Dr.Jit arrays of one type, end to end: a vector type joins component by component."""
    import drjit as dr

    first = parts[0]
    if dr.depth_v(first) > 1:
        return type(first)(*(joined([part[axis] for part in parts]) for axis in range(len(first))))
    return dr.concat(list(parts))


def chunks(count, per):
    """(start, size) of consecutive slices of `count` items at most `per` long."""
    return [(start, min(per, count - start)) for start in range(0, count, per)]


class RayQuery:
    def __init__(self, positions, tris, variant=None, hit_filter=None):
        self.hit_filter = hit_filter
        variant = variant or VARIANT
        mi = import_mitsuba()
        if mi is None:
            raise RuntimeError("ray queries need Mitsuba: pip install -r launcher/tools/r3d/requirements.txt")
        if not is_jit(variant):
            raise RuntimeError(f"ray queries run on a JIT variant (llvm_ad_rgb or cuda_ad_rgb), not {variant}")
        import drjit as dr

        if variant.startswith("llvm") and not dr.has_backend(dr.JitBackend.LLVM):
            raise RuntimeError("ray queries need libLLVM (apt install llvm, or the LLVM runtime for your platform)")
        mi.set_variant(variant)
        self.mi, self.dr, self.variant = mi, dr, variant
        self.positions, self.tris = np.asarray(positions, dtype=np.float64), np.asarray(tris, dtype=np.int64)
        mesh = mi.Mesh("query", len(self.positions), len(self.tris))
        params = mi.traverse(mesh)
        params["vertex_positions"] = self.positions.astype(np.float32).ravel()
        params["faces"] = self.tris.astype(np.uint32).ravel()
        params.update()
        # The device copies the mesh keeps: corners for barycentrics and facing.
        self.vertex_positions, self.faces = params["vertex_positions"], params["faces"]
        self.scene = mi.load_dict({"type": "scene", "mesh": mesh})

    def _check_variant(self):
        if self.mi.variant() != self.variant:
            raise RuntimeError(f"the ray query was built on {self.variant} but Mitsuba is now on {self.mi.variant()}")

    # Device queries: Dr.Jit arrays in and out.

    def rays(self, origins, directions):
        """NumPy (n, 3) origins and directions as a Mitsuba ray."""
        return self.mi.Ray3f(vectors(self.mi.Point3f, origins), vectors(self.mi.Vector3f, directions))

    def corners(self, prim, active=True):
        """The three corner positions of triangles `prim`, read only where `active` (a missed ray's `prim` is no
        triangle)."""
        dr, mi = self.dr, self.mi
        return tuple(dr.gather(mi.Point3f, self.vertex_positions,
                               dr.gather(mi.UInt32, self.faces, prim * 3 + corner, active), active)
                     for corner in range(3))

    def barycentrics(self, prim, where, active=True):
        """Barycentric weights of locations on triangles `prim`, from unsigned area ratios (geometry.hit_barycentrics
        on the device), where `active`."""
        dr = self.dr
        a, b, c = self.corners(prim, active)
        total = dr.sum(dr.abs(dr.cross(b - a, c - a)))
        wa = dr.sum(dr.abs(dr.cross(b - where, c - where))) / total
        wb = dr.sum(dr.abs(dr.cross(c - where, a - where))) / total
        return wa, wb, 1.0 - wa - wb

    def device_hits(self, ray, active=True, passes=None):
        """The first solid hit along each ray (a DeviceHit). With a `hit_filter`, or a `passes(prim, where, ray,
        active)` that returns the hits a caller looks through, a ray steps past every other hit by the larger of
        `STEP` and its coordinate-scaled `STEP_RELATIVE`, up to `MAX_HITS` hits; rays still crossing then are cut off
        and reported on stderr."""
        self._check_variant()
        dr, mi = self.dr, self.mi
        active = mi.Bool(active)
        solid = None if self.hit_filter is None else self.hit_filter.device(self)
        if solid is None and passes is None:
            hit = self.scene.ray_intersect_preliminary(ray, active=active)
            valid = active & hit.is_valid()
            return DeviceHit(valid, hit.t, hit.prim_index, ray(hit.t))
        d = mi.Vector3f(ray.d)
        o0 = mi.Point3f(ray.o)

        def body(o, alive, found, prim, where, rounds):
            hit = self.scene.ray_intersect_preliminary(mi.Ray3f(o, d), active=alive)
            valid = alive & hit.is_valid()
            at = o + d * hit.t
            keep = valid
            if solid is not None:
                keep = keep & solid(hit.prim_index, at, valid)
            if passes is not None:
                keep = keep & ~passes(hit.prim_index, at, d, keep)
            found = found | keep
            prim = dr.select(keep, hit.prim_index, prim)
            where = dr.select(keep, at, where)
            step = dr.maximum(STEP, STEP_RELATIVE * dr.max(dr.abs(at)))
            o = dr.select(valid & ~keep, at + d * step, o)
            rounds = rounds + 1
            return o, valid & ~keep, found, prim, where, rounds

        width = dr.width(ray.d)
        if not width:
            return DeviceHit(dr.zeros(mi.Bool, 0), dr.zeros(mi.Float, 0), dr.zeros(mi.UInt32, 0),
                             dr.zeros(mi.Point3f, 0))
        state = (mi.Point3f(o0), active & dr.full(mi.Bool, True, width), dr.zeros(mi.Bool, width),
                 dr.zeros(mi.UInt32, width), dr.zeros(mi.Point3f, width), mi.UInt32(0))
        o, alive, found, prim, where, _ = dr.while_loop(
            state, lambda o, alive, found, prim, where, rounds: alive & (rounds < MAX_HITS), body,
            labels=("o", "alive", "found", "prim", "where", "rounds"))
        dr.eval(alive, found, prim, where)
        cut = true_count(dr, alive)
        if cut:
            print(f"ray query: {cut} rays still crossing triangles after {MAX_HITS} hits were cut off",
                  file=sys.stderr)
        t = dr.dot(where - o0, d) / dr.squared_norm(d)
        return DeviceHit(found, t, prim, where)

    def device_blocked(self, ray, active=True):
        """True for each ray that hits a solid part of the mesh (every hit without a `hit_filter`)."""
        self._check_variant()
        if self.hit_filter is None:
            active = self.mi.Bool(active)
            return active & self.scene.ray_test(ray, active=active)
        return self.device_hits(ray, active).valid

    # Host queries: NumPy in and out, in launches of at most LAUNCH rays.

    def first_hits(self, origins, directions):
        """(hit mask, distance along the direction, triangle) for every ray; distance and triangle mean nothing where
        the mask is false."""
        origins, directions = np.asarray(origins), np.asarray(directions)
        count = len(origins)
        hit, distance, tri = np.zeros(count, bool), np.zeros(count), np.zeros(count, np.int64)
        for start, size in chunks(count, LAUNCH):
            part = slice(start, start + size)
            found = self.device_hits(self.rays(origins[part], directions[part]))
            self.dr.eval(found.valid, found.t, found.prim)
            hit[part], distance[part], tri[part] = (np.array(found.valid, bool), np.array(found.t, np.float64),
                                                     np.array(found.prim, np.int64))
        return hit, distance, tri

    def first_hit(self, origins, directions):
        """(locations, ray index, triangle) of each ray's first hit; rays that miss are left out."""
        origins, directions = np.asarray(origins, np.float64), np.asarray(directions, np.float64)
        hit, distance, tri = self.first_hits(origins, directions)
        ray = np.flatnonzero(hit)
        return origins[ray] + directions[ray] * distance[ray, None], ray, tri[ray]

    def blocked(self, origins, directions):
        """True for each ray that hits a solid part of the mesh (every hit without a `hit_filter`)."""
        origins, directions = np.asarray(origins), np.asarray(directions)
        blocked = np.zeros(len(origins), bool)
        for start, size in chunks(len(origins), LAUNCH):
            part = slice(start, start + size)
            mask = self.device_blocked(self.rays(origins[part], directions[part]))
            self.dr.eval(mask)
            blocked[part] = np.array(mask, bool)
        return blocked

    def hit_rounds(self, origins, directions, active=None):
        """Yield (triangle, ray index, location) of solid hits in nearest-first rounds, stepping past each hit by the
        larger of `STEP` and its coordinate-scaled `STEP_RELATIVE`. The caller may clear entries in the per-ray `active`
        mask between rounds to retire rays. Rays still active after `MAX_HITS` are cut off and reported on stderr."""
        origins, directions = np.asarray(origins, np.float64), np.asarray(directions, np.float64)
        active = np.ones(len(origins), bool) if active is None else active
        alive = np.flatnonzero(active)
        start = origins.copy()
        for _ in range(MAX_HITS):
            if not len(alive):
                break
            hit, distance, tri = self.first_hits(start[alive], directions[alive])
            alive, distance, tri = alive[hit], distance[hit], tri[hit]
            where = start[alive] + directions[alive] * distance[:, None]
            yield tri, alive, where
            keep = active[alive]
            alive, where = alive[keep], where[keep]
            step = np.maximum(STEP, STEP_RELATIVE * np.abs(where).max(axis=1, initial=0.0))
            start[alive] = where + directions[alive] * step[:, None]
        else:
            if len(alive):
                print(f"ray query: {len(alive)} rays still crossing triangles after {MAX_HITS} hits were cut off", file=sys.stderr)
