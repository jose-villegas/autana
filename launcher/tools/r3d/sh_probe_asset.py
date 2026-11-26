"""SHPR incident-radiance grid encoding and wire layout."""

import math
import pathlib
import struct

from build.c_header import read_define
from r3d.mesh_asset import checked_struct

OWNER = pathlib.Path(__file__).resolve().parents[2] / 'main/render/probe/sh_probes.h'
TYPE = b'SHPR'
SUFFIX = read_define(OWNER, 'SH_PROBES_NAME_SUFFIX')
VERSION = read_define(OWNER, 'SH_PROBES_VERSION')
SH_PROBES_FLAG_INSIDE_MASK = read_define(OWNER, 'SH_PROBES_FLAG_INSIDE_MASK')
MASK_BITS_PER_BYTE = read_define(OWNER, 'SH_PROBES_MASK_BITS_PER_BYTE')
SH1_OWNER = OWNER.parents[3] / 'packages/math/include/math/sh/sh1.h'
CHANNELS = read_define(SH1_OWNER, 'SH1_CHANNEL_COUNT')
COEFFICIENTS = read_define(SH1_OWNER, 'SH1_COEFFICIENT_COUNT')
L1_AXES = (1, 2, 0)
HEADER_FIELDS = (('VERSION', 'H'), ('FLAGS', 'H'), ('ORIGIN', '3f'), ('SPACING', '3f'), ('SCALE', 'f'), ('DIMS', '3H'), ('PADDING', '2x'))
HEADER = checked_struct(OWNER, 'SH_PROBES', HEADER_FIELDS)
RECORD = struct.Struct(f'<{COEFFICIENTS * CHANNELS}h')
U16_BITS = 16
DIM_MAX = (1 << U16_BITS) - 1
QUANT_MAX = DIM_MAX >> 1


def encode(origin, spacing, dims, coefficients, inside=None):
    import numpy as np

    dims = np.asarray(dims)
    coefficients = np.asarray(coefficients, dtype=float)
    if dims.shape != (3,) or np.any(dims < 1) or np.any(dims > DIM_MAX) or np.any(dims != dims.astype(int)):
        raise ValueError('SH grid dimensions must fit positive u16')
    if coefficients.size != int(np.prod(dims)) * COEFFICIENTS * CHANNELS or not np.isfinite(coefficients).all():
        raise ValueError('SH coefficients must be finite and match dimensions')
    if not np.isfinite(origin).all() or not np.isfinite(spacing).all() or np.any(np.asarray(spacing) <= 0):
        raise ValueError('SH origin and positive spacing must be finite')
    count = int(np.prod(dims))
    inside = np.zeros(count, dtype=bool) if inside is None else np.asarray(inside, dtype=bool).ravel()
    if inside.size != count:
        raise ValueError('SH inside mask must match dimensions')
    maximum = float(np.max(np.abs(coefficients)))
    scale = np.float32(maximum / QUANT_MAX if maximum else 1.0)
    if scale == 0 or not np.isfinite(scale):
        raise ValueError('SH scale cannot be represented as f32')
    if float(scale) * QUANT_MAX < maximum:
        scale = np.nextafter(scale, np.float32(np.inf))
    quantized = np.rint(coefficients / scale).astype('<i2')
    mask = np.packbits(inside, bitorder='little').tobytes()
    return HEADER.pack(VERSION, SH_PROBES_FLAG_INSIDE_MASK, *origin, *spacing, scale, *dims) + quantized.tobytes() + mask


def layout(data):
    """Checked header fields and where the inside mask starts: (version, flags, origin, spacing, scale, dims, mask_at).
    The standard library only."""
    if len(data) < HEADER.size:
        raise ValueError('truncated SHPR header')
    version, flags, *fields = HEADER.unpack_from(data)
    origin, spacing, scale, dims = tuple(fields[:3]), tuple(fields[3:6]), fields[6], tuple(fields[7:10])
    count = math.prod(dims)
    mask_at = HEADER.size + count * RECORD.size
    mask_size = (count + MASK_BITS_PER_BYTE - 1) // MASK_BITS_PER_BYTE if flags & SH_PROBES_FLAG_INSIDE_MASK else 0
    if version != VERSION or flags & ~SH_PROBES_FLAG_INSIDE_MASK or not all(dims) or len(data) != mask_at + mask_size:
        raise ValueError('invalid SHPR version, flags, dimensions or length')
    if not all(map(math.isfinite, (*origin, *spacing, scale))) or min(*spacing, scale) <= 0:
        raise ValueError('invalid SHPR origin, spacing or scale')
    return version, flags, origin, spacing, scale, dims, mask_at


def decode(data):
    import numpy as np

    _, flags, origin, spacing, scale, dims, mask_at = layout(data)
    count = math.prod(dims)
    coefficients = np.frombuffer(data, dtype='<i2', count=count * COEFFICIENTS * CHANNELS, offset=HEADER.size).reshape(-1, COEFFICIENTS, CHANNELS) * scale
    inside = (np.unpackbits(np.frombuffer(data, dtype=np.uint8, offset=mask_at), bitorder='little')[:count].astype(bool)
              if flags & SH_PROBES_FLAG_INSIDE_MASK else np.zeros(count, dtype=bool))
    return origin, spacing, dims, scale, coefficients, inside
