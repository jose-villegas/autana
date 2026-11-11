"""SCNE batches: a renderer's copies packed as Morton-ordered chunks of places and turns.

The layout is docs/assets/formats/SCNE.md#batches; render/batch/r3d_batch.h owns the
constants and decodes what pack() writes. Copies are rigid, as the matcher that finds
them makes them (r3d/objects/instances.py): a turn and a position, never a scale.
"""

import math
import pathlib
import struct
import sys
from array import array

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from build.c_header import read_define
from r3d.scene_rows import SceneError

OWNER = pathlib.Path(__file__).resolve().parents[3] / "main/render/batch/r3d_batch.h"
CHUNK_COPIES = read_define(OWNER, "R3D_BATCH_CHUNK_COPIES")
STEPS = read_define(OWNER, "R3D_COPY_STEPS")
TURN_RANGE = read_define(OWNER, "R3D_COPY_TURN_RANGE")
BATCH = struct.Struct("<HHII")  # renderer, copy_count, first_copy, first_chunk
CHUNK = struct.Struct("<4f")    # centre, reach
PLACE = struct.Struct("<3hH")   # offset, colour_set
TURN = struct.Struct("<3hH")    # rotation, largest
AXES = 3
QUATERNION = 4
# Bits of each axis in the Morton key: the order needs no finer grid than a chunk's few copies.
MORTON_BITS = 10
# How far a copy's 3x3 may be from a rotation and still be one, as a share of 1.
RIGID_TOLERANCE = 1e-4


def f32(value):
    """`value` rounded to the nearest float32."""
    return array("f", [value])[0]


def f32_up(value):
    """The least float32 at or above `value`."""
    rounded = f32(value)
    if rounded >= value:
        return rounded
    bits = struct.unpack("<i", struct.pack("<f", rounded))[0]
    return struct.unpack("<f", struct.pack("<i", bits + (1 if rounded >= 0 else -1)))[0]


def chunk_count(copies):
    return -(-copies // CHUNK_COPIES)


def check_rigid(matrix, name):
    """Refuse a copy whose 3x3 is not a rotation: no scale and no reflection."""
    for a in range(AXES):
        for b in range(AXES):
            dot = sum(matrix[r][a] * matrix[r][b] for r in range(AXES))
            if abs(dot - (1.0 if a == b else 0.0)) > RIGID_TOLERANCE:
                raise SceneError(f"copy {name}: its placement scales or shears; a copy turns and moves only")
    det = (matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1])
           - matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0])
           + matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]))
    if det < 0:
        raise SceneError(f"copy {name}: its placement reflects; a copy turns and moves only")


def quaternion(matrix):
    """The unit quaternion (x, y, z, w) of rotation `matrix`, as mat4f_from_trs() reads one back."""
    m = matrix
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = 2.0 * math.sqrt(1.0 + trace)
        q = ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4)
    elif m[0][0] >= m[1][1] and m[0][0] >= m[2][2]:
        s = 2.0 * math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2])
        q = (s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s)
    elif m[1][1] >= m[2][2]:
        s = 2.0 * math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2])
        q = ((m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s)
    else:
        s = 2.0 * math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1])
        q = ((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4, (m[1][0] - m[0][1]) / s)
    norm = math.sqrt(sum(c * c for c in q))
    return tuple(c / norm for c in q)


def step(value, unit):
    """`value` in `unit`s, rounded and held to what a row stores."""
    return 0 if unit == 0 else max(-STEPS, min(STEPS, round(value / unit)))


def turn_row(matrix):
    """The smallest three of the rotation's quaternion, the largest made positive and named."""
    q = quaternion(matrix)
    largest = max(range(QUATERNION), key=lambda i: abs(q[i]))
    sign = 1.0 if q[largest] >= 0 else -1.0
    rest = [sign * q[i] for i in range(QUATERNION) if i != largest]
    return (*[step(c, TURN_RANGE / STEPS) for c in rest], largest)


def morton(cell):
    key = 0
    for bit in range(MORTON_BITS):
        for axis in range(AXES):
            key |= ((cell[axis] >> bit) & 1) << (bit * AXES + axis)
    return key


def ordered(copies):
    """`copies` in Morton order of their positions over the batch's bounds, ties in their own order."""
    lo = [min(c["position"][a] for c in copies) for a in range(AXES)]
    hi = [max(c["position"][a] for c in copies) for a in range(AXES)]
    top = (1 << MORTON_BITS) - 1

    def cell(position):
        return [0 if hi[a] == lo[a] else round((position[a] - lo[a]) / (hi[a] - lo[a]) * top) for a in range(AXES)]

    return [c for _, _, c in sorted((morton(cell(c["position"])), i, c) for i, c in enumerate(copies))]


def pack(copies):
    """The rows of one batch: its chunks, and each copy's place and turn, in Morton order.

    Each copy is a dict with `name`, `matrix` (rows), `position` and `colour_set`.
    """
    chunks, places, turns = [], [], []
    for copy in copies:
        check_rigid(copy["matrix"], copy["name"])
    copies = ordered(copies)
    for first in range(0, len(copies), CHUNK_COPIES):
        run = copies[first:first + CHUNK_COPIES]
        lo = [min(c["position"][a] for c in run) for a in range(AXES)]
        hi = [max(c["position"][a] for c in run) for a in range(AXES)]
        centre = [f32((lo[a] + hi[a]) / 2) for a in range(AXES)]
        reach = f32_up(max(math.dist(centre, c["position"]) for c in run))
        chunks.append((*centre, reach))
        for c in run:
            places.append((*[step(c["position"][a] - centre[a], reach / STEPS) for a in range(AXES)],
                           c["colour_set"]))
            turns.append(turn_row(c["matrix"]))
    return chunks, places, turns


def copy_placement(chunk, place, turn):
    """A copy's (matrix rows, position) from its rows, as r3d_batch_copy_model() decodes them."""
    *centre, reach = chunk
    unit = f32(reach / STEPS)
    position = [f32(centre[a] + place[a] * unit) for a in range(AXES)]
    *rotation, largest = turn
    q, rest, values = [0.0] * QUATERNION, 1.0, iter(rotation)
    for i in range(QUATERNION):
        if i != largest:
            q[i] = next(values) * f32(TURN_RANGE / STEPS)
            rest -= q[i] * q[i]
    q[largest] = math.sqrt(max(rest, 0.0))
    x, y, z, w = q
    matrix = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
              [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
              [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return matrix, position


def unpack(chunks, places, turns):
    """Every copy's (matrix rows, position, colour_set) of one batch, in its row order."""
    return [(*copy_placement(chunks[i // CHUNK_COPIES], place, turn), place[AXES])
            for i, (place, turn) in enumerate(zip(places, turns))]
