"""Bake scene incident radiance into an L1 spherical-harmonic probe grid."""

import math
import struct
import time

import numpy as np

from r3d.geometry import face_normals
from r3d.light import AlphaTest
from r3d.mesh_source import load_source
from r3d.path_bake import PathLight, BATCH, SEED
from r3d.ray_query import RayQuery
from r3d.sh_probe_settings import DIRECTIONS, SPP, SUFFIX
from r3d.views.probes import classify, probe_points, spacing_for, sphere_directions

VERSION = 1
DIRECT_INCLUDED = 1
CHANNELS = 3
COEFFICIENTS = 4
Y00 = 0.5 * math.sqrt(1 / math.pi)
Y1 = 0.5 * math.sqrt(3 / math.pi)
HEADER = struct.Struct('<BB3f3f3Hf')
QUANT_MAX = np.iinfo(np.int16).max
DIM_MAX = np.iinfo(np.uint16).max


def basis(directions):
    """Real SH in Sloan's order: constant, +y, +z, +x."""
    directions = np.asarray(directions)
    return np.column_stack((np.full(len(directions), Y00), Y1 * directions[:, 1],
                            Y1 * directions[:, 2], Y1 * directions[:, 0]))


def project(directions, radiance):
    return (4 * math.pi / len(directions)) * np.einsum('nk,...nc->...kc', basis(directions), radiance)


def sun_term(query, points, lights):
    out = np.zeros((len(points), COEFFICIENTS, CHANNELS))
    for light in lights:
        if light['type'] != 'directional':
            continue
        direction = np.asarray(light['direction'], dtype=float)
        direction /= np.linalg.norm(direction)
        visible = ~query.blocked(points, np.broadcast_to(direction, points.shape).copy())
        colour = np.asarray(light['color']) * light['intensity']
        weights = basis(direction[None]).reshape(1, COEFFICIENTS, 1)
        out += math.pi * visible[:, None, None] * weights * colour
    return out


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


def encode(origin, spacing, dims, coefficients):
    dims = np.asarray(dims)
    coefficients = np.asarray(coefficients, dtype=float)
    if dims.shape != (3,) or np.any(dims < 1) or np.any(dims > DIM_MAX) or np.any(dims != dims.astype(int)):
        raise ValueError('SH grid dimensions must fit positive u16')
    if coefficients.size != int(np.prod(dims)) * COEFFICIENTS * CHANNELS or not np.isfinite(coefficients).all():
        raise ValueError('SH coefficients must be finite and match dimensions')
    if not np.isfinite(origin).all() or not np.isfinite(spacing).all() or np.any(np.asarray(spacing) <= 0):
        raise ValueError('SH origin and positive spacing must be finite')
    maximum = float(np.max(np.abs(coefficients)))
    scale = np.float32(maximum / QUANT_MAX if maximum else 1.0)
    if scale == 0 or not np.isfinite(scale):
        raise ValueError('SH scale cannot be represented as f32')
    if float(scale) * QUANT_MAX < maximum:
        scale = np.nextafter(scale, np.float32(np.inf))
    quantized = np.rint(coefficients / scale).astype('<i2')
    return HEADER.pack(VERSION, DIRECT_INCLUDED, *origin, *spacing, *dims, scale) + quantized.tobytes()


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated SHPR header')
    version, flags, *fields = HEADER.unpack_from(data)
    origin, spacing, dims, scale = fields[:3], fields[3:6], fields[6:9], fields[9]
    expected = HEADER.size + math.prod(dims) * COEFFICIENTS * CHANNELS * np.dtype('<i2').itemsize
    if version != VERSION or flags != DIRECT_INCLUDED or not all(dims) or len(data) != expected:
        raise ValueError('invalid SHPR version, flags, dimensions or length')
    if not np.isfinite([*origin, *spacing, scale]).all() or min(*spacing, scale) <= 0:
        raise ValueError('invalid SHPR origin, spacing or scale')
    coefficients = np.frombuffer(data, dtype='<i2', offset=HEADER.size).reshape(-1, COEFFICIENTS, CHANNELS) * scale
    return origin, spacing, dims, scale, coefficients


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
    requested = settings.sh_probes
    source = load_source(settings)
    hit_filter = AlphaTest(source, settings.alpha_keep) if settings.alpha_keep is not None else None
    query = RayQuery(source.p, source.tri_v, hit_filter=hit_filter)
    points, cell, origin, step, dims = grid(source, camera, requested.spacing, settings.seed)
    moved, state = classify(query, face_normals(source.p, source.tri_v)[0], points, cell, camera.near_z)
    outside = state != 'inside'
    transport = PathLight(source, scene.lights, settings.double_sided, job.bake.indirect,
                          scene.indirect.albedo_boost)
    from r3d.mitsuba_reference import integrator
    transport.integrator = transport.mi.load_dict({**integrator(job.bake.indirect.bounces + 1), 'hide_emitters': False})
    directions = sphere_directions(requested.directions)
    coefficients = np.zeros((len(points), COEFFICIENTS, CHANNELS))
    chunk_size = max(1, BATCH // requested.directions)
    selected = np.flatnonzero(outside)
    for chunk_index, start in enumerate(range(0, len(selected), chunk_size)):
        indices = selected[start:start + chunk_size]
        origins = np.repeat(moved[indices], requested.directions, axis=0)
        rays = np.tile(directions, (len(indices), 1))
        radiance = sum(transport._radiance(origins, rays, SEED + chunk_index * requested.spp + sample)
                       for sample in range(requested.spp)) / requested.spp
        coefficients[indices] = project(
            directions, radiance.reshape(len(indices), requested.directions, CHANNELS))
    coefficients[outside] += sun_term(query, moved[outside], scene.lights)
    # ProbeViews enumerates z fastest; SHPR's x-fastest order is the transposed cell lattice.
    shape = tuple(dims) + (COEFFICIENTS, CHANNELS)
    filled = dilate(coefficients.reshape(shape), (state == 'inside').reshape(tuple(dims)))
    data = encode(origin, step, dims, filled.transpose(2, 1, 0, 3, 4).copy())
    sun_rays = sum(light['type'] == 'directional' for light in scene.lights)
    paths = int(outside.sum()) * requested.directions * requested.spp
    print(f'SH probes: {len(points)} probes, {np.count_nonzero(~outside)} inside, '
          f'{paths} primary paths, {int(outside.sum()) * sun_rays} sun rays, '
          f'{len(data)} bytes, {time.perf_counter() - started:.3f} s', flush=True)
    return data
