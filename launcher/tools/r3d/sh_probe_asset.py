"""SHPR incident-radiance grid encoding and wire layout."""

import math
import pathlib
import struct

from build.c_header import read_define

OWNER = pathlib.Path(__file__).resolve().parents[2] / 'main/render/probe/sh_probes.h'
TYPE = b'SHPR'
SUFFIX = read_define(OWNER, 'SH_PROBES_NAME_SUFFIX')
VERSION = 1
RESERVED_FLAGS = 0
CHANNELS = 3
COEFFICIENTS = 4
L1_AXES = (1, 2, 0)
HEADER = struct.Struct('<HH3f3ff3H2x')
U16_BITS = 16
DIM_MAX = (1 << U16_BITS) - 1
QUANT_MAX = DIM_MAX >> 1


def encode(origin, spacing, dims, coefficients):
    import numpy as np

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
    return HEADER.pack(VERSION, RESERVED_FLAGS, *origin, *spacing, scale, *dims) + quantized.tobytes()


def decode(data):
    import numpy as np

    if len(data) < HEADER.size:
        raise ValueError('truncated SHPR header')
    version, flags, *fields = HEADER.unpack_from(data)
    origin, spacing, dims, scale = fields[:3], fields[3:6], fields[7:10], fields[6]
    expected = HEADER.size + math.prod(dims) * COEFFICIENTS * CHANNELS * np.dtype('<i2').itemsize
    if version != VERSION or flags != RESERVED_FLAGS or not all(dims) or len(data) != expected:
        raise ValueError('invalid SHPR version, flags, dimensions or length')
    if not np.isfinite([*origin, *spacing, scale]).all() or min(*spacing, scale) <= 0:
        raise ValueError('invalid SHPR origin, spacing or scale')
    coefficients = np.frombuffer(data, dtype='<i2', offset=HEADER.size).reshape(-1, COEFFICIENTS, CHANNELS) * scale
    return origin, spacing, dims, scale, coefficients
