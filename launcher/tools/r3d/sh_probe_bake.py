"""Bake scene incident radiance into an L1 spherical-harmonic probe grid."""

import math
import time

import numpy as np

from r3d.geometry import face_normals
from r3d.import_settings import PUNCTUAL_LIGHTS
from r3d.light import AlphaTest, baked_lights, light_rays
from r3d.mesh_source import load_source
from r3d.mitsuba_reference import integrator
from r3d.path_bake import PathLight, SEED
from r3d.ray_query import LAUNCH, RayQuery
from r3d.sh_probe_settings import load
from r3d.sh_probe_asset import CHANNELS, COEFFICIENTS, L1_AXES, encode
from r3d.views.probes import classify, probe_points, spacing_for, sphere_directions

Y00 = 0.5 * math.sqrt(1 / math.pi)
Y1 = 0.5 * math.sqrt(3 / math.pi)


def basis(directions):
    """Real SH in Sloan's order: constant, +y, +z, +x."""
    directions = np.asarray(directions)
    return np.column_stack((np.full(len(directions), Y00), *(Y1 * directions[:, axis] for axis in L1_AXES)))


def project(directions, radiance):
    return (4 * math.pi / len(directions)) * np.einsum('nk,...nc->...kc', basis(directions), radiance)


def light_visible(intersector, origin, toward, reach, traced):
    """Which NumPy origins see the light: the `traced` ones (indices) whose shadow ray toward it, no farther than
    `reach`, is clear. Every other origin is unlit and traces no ray."""
    rays = np.ascontiguousarray(np.broadcast_to(toward, origin.shape)[traced])
    seen = np.zeros(len(origin), dtype=bool)
    seen[traced] = ~intersector.blocked(origin[traced], rays, None if reach is None else reach[traced])
    return seen


def direct_term(query, points, lights, floor):
    """The directional, point and spot lights straight to each probe, which no gathered path can hit: one shadow ray
    per probe per light, no farther than the light, and none where the light's falloff is zero. Returns the
    coefficients and the rays traced."""
    out, traced = np.zeros((len(points), COEFFICIENTS, CHANNELS)), 0
    for light in baked_lights(lights):
        if light['type'] not in PUNCTUAL_LIGHTS:
            continue
        toward, reach, falloff = light_rays(light, points, floor)
        reached = np.flatnonzero(np.broadcast_to(np.asarray(falloff) > 0, len(points)))
        visible = light_visible(query, points, toward, reach, reached)
        traced += len(reached)
        colour = np.asarray(light['color']) * light['intensity']
        weights = basis(toward[None] if toward.ndim == 1 else toward).reshape(-1, COEFFICIENTS, 1)
        out += math.pi * (visible * falloff)[:, None, None] * weights * colour
    return out, traced


def dilate(values, inside):
    """Fill each frontier from its valid six-neighbours, one synchronous layer at a time."""
    out, valid = np.array(values, copy=True), ~np.asarray(inside, dtype=bool)
    if not valid.any():
        raise ValueError('SH grid has no outside probes to dilate')
    while not valid.all():
        sums, counts = np.zeros_like(out), np.zeros(valid.shape, dtype=int)
        for axis in range(valid.ndim):
            low = [slice(None)] * valid.ndim
            high = low.copy()
            low[axis], high[axis] = slice(None, -1), slice(1, None)
            low, high = tuple(low), tuple(high)
            for target, source in ((low, high), (high, low)):
                sums[target] += out[source] * valid[source][..., None, None]
                counts[target] += valid[source]
        frontier = ~valid & (counts > 0)
        out[frontier] = sums[frontier] / counts[frontier, None, None]
        valid |= frontier
    return out


def grid(source, camera, requested, seed):
    lo, hi = camera.region if camera.region is not None else (source.p.min(axis=0), source.p.max(axis=0))
    lo, hi = np.asarray(lo), np.asarray(hi)
    spacing = spacing_for(lo, hi, requested, camera.near_z)
    points, cell = probe_points(lo, hi, spacing, seed)
    dims = np.maximum(np.ceil((hi - lo) / spacing).astype(int), 1)
    step = (hi - lo) / dims
    if np.any(step <= 0):
        raise ValueError('SH grid region must have positive extent on each axis')
    return points, cell, lo + step / 2, step, dims


def bake(job, scene):
    """SHPR bytes for one import and light rig; classification uses the full source with alpha-tested hits."""
    started = time.perf_counter()
    settings, camera = job.settings, scene.camera.component
    requested = load(scene.path)
    source = load_source(settings)
    hit_filter = AlphaTest(source, settings.alpha_keep) if settings.alpha_keep is not None else None
    query = RayQuery(source.p, source.tri_v, hit_filter=hit_filter)
    points, cell, origin, step, dims = grid(source, camera, requested.spacing, settings.seed)
    moved, state = classify(query, face_normals(source.p, source.tri_v)[0], points, cell, camera.near_z)
    outside = state != 'inside'
    lights = baked_lights(scene.lights)
    transport = PathLight(source, lights, settings.double_sided, job.bake.indirect,
                          scene.indirect.albedo_boost)
    transport.integrator = transport.mi.load_dict({**integrator(job.bake.indirect.bounces + 1), 'hide_emitters': False})
    directions = sphere_directions(requested.directions)
    coefficients = np.zeros((len(points), COEFFICIENTS, CHANNELS))
    chunk_size = max(1, LAUNCH // requested.directions)
    selected = np.flatnonzero(outside)
    for chunk_index, start in enumerate(range(0, len(selected), chunk_size)):
        indices = selected[start:start + chunk_size]
        origins = np.repeat(moved[indices], requested.directions, axis=0)
        rays = np.tile(directions, (len(indices), 1))
        radiance = sum(transport._radiance(origins, rays, SEED + chunk_index * requested.spp + sample)
                       for sample in range(requested.spp)) / requested.spp
        coefficients[indices] = project(
            directions, radiance.reshape(len(indices), requested.directions, CHANNELS))
    direct, direct_rays = direct_term(query, moved[outside], lights, job.bake.ray_offset)
    coefficients[outside] += direct
    # ProbeViews enumerates z fastest; SHPR's x-fastest order is the transposed cell lattice.
    shape = tuple(dims) + (COEFFICIENTS, CHANNELS)
    inside = (state == 'inside').reshape(tuple(dims))
    filled = dilate(coefficients.reshape(shape), inside)
    data = encode(origin, step, dims, filled.transpose(2, 1, 0, 3, 4).copy(), inside.transpose(2, 1, 0))
    paths = int(outside.sum()) * requested.directions * requested.spp
    print(f'SH probes: {len(points)} probes, {np.count_nonzero(~outside)} inside, '
          f'{paths} primary paths, {direct_rays} direct rays, '
          f'{len(data)} bytes, {time.perf_counter() - started:.3f} s', flush=True)
    return data
