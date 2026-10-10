"""SKIN v1 encoding and LMSH position matching; docs/render/Skeleton-and-Skin.md."""

import math
import struct

from anim import skeleton_asset
from gltf import gltf_read
from r3d import gltf_skin, mesh_asset

TYPE = b'SKIN'
VERSION = 1
ALIGNMENT = 4
INFLUENCES = (2, 4)
DEFAULT_INFLUENCES = 2
WEIGHT_SUM = 255
NORMAL_SCALE = 127
HEADER = struct.Struct('<HBBIII')
# mesh_asset owns the LMSH position layout.
MESH_POSITION = struct.Struct('<3h')
MATRIX = struct.Struct('<12f')
NORMAL = struct.Struct('<3bx')
MATRIX_AFFINE_TOLERANCE = 1e-6


NORMAL_ATTRIBUTE = 'NORMAL'  # magic: the glTF 2.0 vertex attribute name, not an engine token

def encode(inverse_binds, vertices, influences=DEFAULT_INFLUENCES):
    if influences not in INFLUENCES:
        raise ValueError('SKIN: influences must be 2 or 4')
    if not 1 <= len(inverse_binds) <= skeleton_asset.JOINT_MAX:
        raise ValueError('SKIN: invalid joint count')
    if any(len(m) != MATRIX.size // struct.calcsize('<f') for m in inverse_binds):
        raise ValueError('SKIN: invalid matrix width')
    if any(len(j) != influences or len(w) != influences or len(n) != 3 for j, w, n in vertices):
        raise ValueError('SKIN: invalid vertex width')
    record = struct.Struct(f'<{influences}B{influences}B3bx')
    matrix_off = HEADER.size
    vertices_off = matrix_off + len(inverse_binds) * MATRIX.size
    try:
        entry = HEADER.pack(VERSION, len(inverse_binds), influences, len(vertices), matrix_off, vertices_off)
        entry += b''.join(MATRIX.pack(*matrix) for matrix in inverse_binds)
        entry += b''.join(record.pack(*joints, *weights, *normal) for joints, weights, normal in vertices)
    except (struct.error, OverflowError) as error:
        raise ValueError(f'SKIN: unrepresentable record: {error}') from error
    decode(entry)
    return entry


def decode(entry):
    if len(entry) < HEADER.size:
        raise ValueError('SKIN: truncated header')
    version, count, influences, vertices, matrix_off, vertices_off = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ValueError('SKIN: unknown version')
    if influences not in INFLUENCES or not 1 <= count <= skeleton_asset.JOINT_MAX:
        raise ValueError('SKIN: influences or joint count')
    record = struct.Struct(f'<{influences}B{influences}B3bB')
    if (matrix_off % ALIGNMENT or vertices_off % ALIGNMENT or matrix_off < HEADER.size
            or vertices_off < matrix_off + count * MATRIX.size
            or vertices_off + vertices * record.size != len(entry)):
        raise ValueError('SKIN: bounds or alignment')
    if any(entry[HEADER.size:matrix_off]) or any(entry[matrix_off + count * MATRIX.size:vertices_off]):
        raise ValueError('SKIN: nonzero padding')
    matrices = [MATRIX.unpack_from(entry, matrix_off + i * MATRIX.size) for i in range(count)]
    if not all(math.isfinite(v) for matrix in matrices for v in matrix):
        raise ValueError('SKIN: nonfinite inverse bind')
    rows = []
    for index in range(vertices):
        row = record.unpack_from(entry, vertices_off + index * record.size)
        joints, weights, normal = row[:influences], row[influences:2 * influences], row[-4:-1]
        if (any(j >= count for j in joints) or sum(weights) != WEIGHT_SUM or row[-1]
                or any(abs(n) > NORMAL_SCALE for n in normal) or not any(normal)):
            raise ValueError(f'SKIN: invalid vertex {index}')
        rows.append((joints, weights, normal))
    return matrices, rows, influences


def quantize_position(position, position_scale):
    # lit_mesh.bake_lit_mesh owns this rule; test_quantizer_matches_mesh_owner pins this adapter to its output.
    return tuple(round(float(value) * position_scale) for value in position)


def quantize_weights(joints, weights, influences=DEFAULT_INFLUENCES):
    if influences not in INFLUENCES:
        raise ValueError('SKIN: influences must be 2 or 4')
    if len(joints) != len(weights) or not weights or any(not math.isfinite(w) or w < 0 for w in weights):
        raise ValueError('SKIN: invalid source weights')
    combined = {}
    for joint, weight in zip(joints, weights):
        combined[joint] = combined.get(joint, 0) + weight
    chosen = sorted(combined.items(), key=lambda pair: (-pair[1], pair[0]))[:influences]
    total = sum(weight for _, weight in chosen)
    if total <= 0:
        raise ValueError('SKIN: zero source weights')
    scaled = [weight / total * WEIGHT_SUM for _, weight in chosen]
    quantized = [math.floor(w) for w in scaled]
    remainder = WEIGHT_SUM - sum(quantized)
    order = sorted(range(len(chosen)), key=lambda i: (-(scaled[i] - quantized[i]), chosen[i][0]))
    for i in order[:remainder]:
        quantized[i] += 1
    pairs = sorted((joint, weight) for (joint, _), weight in zip(chosen, quantized) if weight)
    pairs += [(0, 0)] * (influences - len(pairs))
    return tuple(j for j, _ in pairs), tuple(w for _, w in pairs)


def mesh_positions(entry):
    if len(entry) < mesh_asset.BLOB_HEADER.size:
        raise ValueError('SKIN: truncated LMSH')
    vertices, _, _, _, scale, positions_off, *_ = mesh_asset.BLOB_HEADER.unpack_from(entry)
    if not scale or positions_off < mesh_asset.BLOB_HEADER.size or positions_off + vertices * MESH_POSITION.size > len(entry):
        raise ValueError('SKIN: invalid LMSH positions')
    return [MESH_POSITION.unpack_from(entry, positions_off + i * MESH_POSITION.size) for i in range(vertices)], scale


def match_vertices(positions, position_scale, source_positions, source_normals, source_joints,
                   source_weights, influences=DEFAULT_INFLUENCES):
    if not (len(source_positions) == len(source_normals) == len(source_joints) == len(source_weights)):
        raise ValueError('SKIN: source attribute counts differ')
    matches = {}
    for index, position in enumerate(source_positions):
        if not all(map(math.isfinite, position)):
            raise ValueError(f'SKIN: nonfinite source position {index}')
        key = quantize_position(position, position_scale)
        matches.setdefault(key, []).append(index)
    vertices = []
    for index, position in enumerate(positions):
        found = matches.get(tuple(position), [])
        if not found:
            raise ValueError(f'SKIN: LMSH vertex {index} at {tuple(position)} has no source match')
        weights = [quantize_weights(source_joints[i], source_weights[i], influences) for i in found]
        if any(weight != weights[0] for weight in weights):
            raise ValueError(f'SKIN: weight disagreement at LMSH vertex {index} position {tuple(position)}')
        mean = [sum(source_normals[i][axis] for i in found) for axis in range(3)]
        length = math.sqrt(sum(value * value for value in mean))
        if not math.isfinite(length) or length == 0:
            raise ValueError(f'SKIN: zero or nonfinite mean normal at LMSH vertex {index}')
        normal = tuple(round(value / length * NORMAL_SCALE) for value in mean)
        vertices.append((*weights[0], normal))
    return vertices


def bake(document, binary, mesh_node, mesh_entry, influences=DEFAULT_INFLUENCES):
    ordered, rows, name = skeleton_asset.rig(document, mesh_node)
    node = document['nodes'][mesh_node]
    skin = document['skins'][node['skin']]
    remap = {i: ordered.index(j) for i, j in enumerate(skin['joints'])}
    positions, normals, joints, weights = [], [], [], []
    for primitive in document['meshes'][node['mesh']]['primitives']:
        attrs = primitive['attributes']
        if not all(key in attrs for key in ('POSITION', NORMAL_ATTRIBUTE, 'JOINTS_0', 'WEIGHTS_0')):
            raise ValueError('SKIN: primitive requires positions, normals, joints and weights')
        read = lambda key: gltf_read.read_accessor(document, binary, attrs[key])
        positions.extend(read('POSITION'))
        normals.extend(read(NORMAL_ATTRIBUTE))
        joint_rows, weight_rows = read('JOINTS_0'), read('WEIGHTS_0')
        for suffix in sorted(key.removeprefix('JOINTS_') for key in attrs if key.startswith('JOINTS_') and key != 'JOINTS_0'):
            more_joints, more_weights = read('JOINTS_' + suffix), read('WEIGHTS_' + suffix)
            joint_rows = [(*a, *b) for a, b in zip(joint_rows, more_joints)]
            weight_rows = [(*a, *b) for a, b in zip(weight_rows, more_weights)]
        try:
            joints.extend(tuple(remap[j] for j in row) for row in joint_rows)
        except KeyError as error:
            raise ValueError('SKIN: source joint outside skin') from error
        weights.extend(weight_rows)
    if 'inverseBindMatrices' in skin:
        matrices = [gltf_skin.mat_from_column_major(m) for m in
                    gltf_read.read_accessor(document, binary, skin['inverseBindMatrices'])]
        if len(matrices) != len(ordered):
            raise ValueError('SKIN: inverse bind count differs')
    else:
        identity = gltf_skin.mat_from_trs((0, 0, 0), (0, 0, 0, 1), (1, 1, 1))
        matrices = [identity] * len(ordered)
    if any(any(abs(a - b) > MATRIX_AFFINE_TOLERANCE for a, b in zip(m[12:], (0, 0, 0, 1))) for m in matrices):
        raise ValueError('SKIN: inverse bind is not affine')
    inverse = [tuple(matrices[skin['joints'].index(j)][:12]) for j in ordered]
    mesh, scale = mesh_positions(mesh_entry)
    vertices = match_vertices(mesh, scale, positions, normals, joints, weights, influences)
    return name, skeleton_asset.encode(rows), encode(inverse, vertices, influences)
