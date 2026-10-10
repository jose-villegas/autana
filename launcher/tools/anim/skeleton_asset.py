"""SKEL v1 encoding and validation; layout: docs/render/Skeleton-and-Skin.md."""

import math
import struct

from gltf import gltf_read
from r3d import gltf_skin

TYPE = b'SKEL'
VERSION = 1
ROOT = 255
JOINT_MAX = 254
ALIGNMENT = 4
UNIT_TOLERANCE = 1e-4
ROTATION = 3
ROTATION_WIDTH = 4
MODEL_SPACE_TOLERANCE = 1e-5
HEADER = struct.Struct('<HBBIII')
ROW = struct.Struct('<HBB')
REST = struct.Struct('<10f')
STRING_MAX = 65535


def encode(joints):
    if not 1 <= len(joints) <= JOINT_MAX:
        raise ValueError('SKEL: invalid joint count')
    strings, rows, rests = bytearray(), bytearray(), bytearray()
    for path, parent, rest in joints:
        if not isinstance(path, str) or not path or '\0' in path:
            raise ValueError('SKEL: empty or invalid joint path')
        if not isinstance(parent, int) or not 0 <= parent <= ROOT or len(rest) != REST.size // struct.calcsize('<f'):
            raise ValueError('SKEL: invalid parent or rest width')
        offset = len(strings)
        strings.extend(path.encode('utf-8') + b'\0')
        if len(strings) > STRING_MAX:
            raise ValueError('SKEL: string table exceeds u16')
        try:
            rows.extend(ROW.pack(offset, parent, 0))
            rests.extend(REST.pack(*rest))
        except (struct.error, OverflowError) as error:
            raise ValueError(f'SKEL: invalid joint {path!r}: {error}') from error
    rest_off = HEADER.size + len(rows)
    strings_off = rest_off + len(rests)
    entry = HEADER.pack(VERSION, len(joints), 0, strings_off, len(strings), rest_off)
    entry += rows + rests + strings
    decode(entry)
    return bytes(entry)


def decode(entry):
    if len(entry) < HEADER.size:
        raise ValueError('SKEL: truncated header')
    version, count, pad, strings_off, strings_size, rest_off = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ValueError('SKEL: unknown version')
    table_end = HEADER.size + count * ROW.size
    if not 1 <= count <= JOINT_MAX or pad:
        raise ValueError('SKEL: joint count or padding')
    if (rest_off % ALIGNMENT or strings_off % ALIGNMENT or rest_off < table_end
            or strings_off < rest_off + count * REST.size
            or strings_off + strings_size != len(entry) or strings_size > STRING_MAX):
        raise ValueError('SKEL: bounds or alignment')
    if any(entry[table_end:rest_off]) or any(entry[rest_off + count * REST.size:strings_off]):
        raise ValueError('SKEL: nonzero padding')
    strings, paths, result = entry[strings_off:], set(), []
    starts = {0} | {i + 1 for i, value in enumerate(strings) if value == 0}
    for index in range(count):
        offset, parent, pad = ROW.unpack_from(entry, HEADER.size + index * ROW.size)
        if pad or (parent != ROOT and parent >= index):
            raise ValueError('SKEL: parent or padding')
        if offset >= strings_size or offset not in starts:
            raise ValueError('SKEL: path offset')
        end = strings.find(b'\0', offset)
        if end < 0:
            raise ValueError('SKEL: unterminated path')
        try:
            path = strings[offset:end].decode('utf-8')
        except UnicodeDecodeError as error:
            raise ValueError('SKEL: invalid path encoding') from error
        if not path or path in paths:
            raise ValueError('SKEL: empty or duplicate path')
        paths.add(path)
        rest = REST.unpack_from(entry, rest_off + index * REST.size)
        if (not all(map(math.isfinite, rest))
                or abs(1 - sum(q * q for q in rest[ROTATION:ROTATION + ROTATION_WIDTH])) > UNIT_TOLERANCE):
            raise ValueError('SKEL: nonfinite rest or nonunit rotation')
        result.append((path, parent, rest))
    return result


def rig(document, mesh_node):
    """Parents-first joint nodes, rows and rig id, requiring mesh-space roots."""
    nodes = document['nodes']
    skin = document['skins'][nodes[mesh_node]['skin']]
    joints = skin['joints']
    if not 1 <= len(joints) <= JOINT_MAX or len(set(joints)) != len(joints):
        raise ValueError('SKEL: invalid skin joints')
    paths = gltf_read.skin_joint_paths(document, skin)
    parents = gltf_skin.node_parents(nodes)
    roots = [j for j in joints if parents[j] not in joints]
    if not roots:
        raise ValueError('SKEL: no root joints')
    pose = [gltf_skin.node_trs(n) for n in nodes]
    relevant = set()
    for start in [mesh_node, *joints]:
        node, seen = start, set()
        while node is not None:
            if node in seen:
                raise ValueError('SKEL: node hierarchy contains a cycle')
            seen.add(node)
            relevant.add(node)
            node = parents[node]
    for index in sorted(relevant):
        if 'matrix' in nodes[index]:
            raise ValueError(f"SKEL: node {nodes[index].get('name', index)!r} uses matrix; TRS required")
    world = gltf_skin.world_matrices(parents, pose)
    identity = gltf_skin.mat_from_trs((0, 0, 0), (0, 0, 0, 1), (1, 1, 1))
    for joint in roots:
        parent_world = identity if parents[joint] is None else world[parents[joint]]
        if any(not math.isfinite(a) or not math.isfinite(b) or abs(a - b) > MODEL_SPACE_TOLERANCE
               for a, b in zip(parent_world, world[mesh_node])):
            raise ValueError(f"SKEL: root {nodes[joint].get('name', joint)!r} parent does not match "
                             f"mesh node {nodes[mesh_node].get('name', mesh_node)!r} model space")
    ordered = sorted(joints, key=lambda j: (paths[j].count('/'), paths[j]))
    indices = {node: i for i, node in enumerate(ordered)}
    rows = [(paths[j], indices.get(parents[j], ROOT),
             (*pose[j]['translation'], *pose[j]['rotation'], *pose[j]['scale'])) for j in ordered]
    name = skin.get('name')
    if not name:
        common = {parents[j] for j in roots}
        if len(common) != 1 or None in common:
            raise ValueError('SKEL: unnamed rig has no common non-joint parent')
        name = nodes[common.pop()].get('name')
    if not name:
        raise ValueError('SKEL: unnamed rig')
    encode(rows)
    return ordered, rows, name
