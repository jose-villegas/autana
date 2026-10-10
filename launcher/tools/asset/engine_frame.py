"""Mirror entries from the source frame into the engine frame at the asset-pack boundary.

The frame split and conversion are defined in docs/render/Mesh-Import.md,
under "The offline tools".
"""

import struct

from anim import tracks_asset
from r3d import mesh_asset, scene_asset

AXIS_SIGNS = (1, 1, -1)
ROTATION_SIGNS = (-1, -1, 1, 1)
Z_AXIS = 2
AXIS_COUNT = len(AXIS_SIGNS)
POSITION = struct.Struct(f"<{AXIS_COUNT}h")
INT16_MIN = -(1 << 15)


def _mirror_position(position):
    if position[Z_AXIS] == INT16_MIN:
        raise ValueError("mirrored position does not fit int16")
    return tuple(value * sign for value, sign in zip(position, AXIS_SIGNS))


def _mirror_box(box):
    low, high = box[:AXIS_COUNT], box[AXIS_COUNT:]
    low, high = _mirror_position(low), _mirror_position(high)
    return (*low[:Z_AXIS], high[Z_AXIS], *high[:Z_AXIS], low[Z_AXIS])


def to_engine(kind, entry):
    """An LMSH, SCNE or TRCK source entry mirrored once, preserving its layout."""
    out = bytearray(entry)
    if kind == mesh_asset.TYPE:
        vertices, triangles, clusters, nodes, scale, pos_at, col_at, tri_at, cl_at, node_at, face_at = (
            mesh_asset.BLOB_HEADER.unpack_from(entry))
        for index in range(vertices):
            at = pos_at + index * POSITION.size
            POSITION.pack_into(out, at, *_mirror_position(POSITION.unpack_from(entry, at)))
        for index in range(clusters):
            at = cl_at + index * mesh_asset.CLUSTER.size
            vb, vc, tb, tc, *box, double = mesh_asset.CLUSTER.unpack_from(entry, at)
            mesh_asset.CLUSTER.pack_into(out, at, vb, vc, tb, tc, *_mirror_box(box), double)
        for index in range(nodes):
            at = node_at + index * mesh_asset.NODE.size
            *box, first, count, leaf = mesh_asset.NODE.unpack_from(entry, at)
            mesh_asset.NODE.pack_into(out, at, *_mirror_box(box), first, count, leaf)
    elif kind == scene_asset.TYPE:
        version, entities, renderers, cameras, names_at, transforms_at, renderers_at, cameras_at = (
            scene_asset.HEADER.unpack_from(entry))
        for index in range(entities):
            at = transforms_at + index * scene_asset.TRANSFORM.size
            values = scene_asset.TRANSFORM.unpack_from(entry, at)
            matrix_width = AXIS_COUNT * AXIS_COUNT
            matrix = [values[row * AXIS_COUNT + column] * AXIS_SIGNS[row] * AXIS_SIGNS[column]
                      for row in range(AXIS_COUNT) for column in range(AXIS_COUNT)]
            position = [value * sign for value, sign in zip(values[matrix_width:], AXIS_SIGNS)]
            scene_asset.TRANSFORM.pack_into(out, at, *matrix, *position)
    elif kind == tracks_asset.TYPE:
        tracks, duration_ms = tracks_asset.decode(entry)
        for track in tracks:
            position = track["component"] == tracks_asset.TRANSFORM and track["field"] == "position"
            signs = ROTATION_SIGNS if track["quaternion"] else AXIS_SIGNS if position else None
            if signs is not None:
                if len(track["values"][0]) != len(signs):
                    raise ValueError("coordinate track has an invalid width")
                track["values"] = [tuple(value * sign for value, sign in zip(row, signs)) for row in track["values"]]
        return tracks_asset.encode(tracks, duration_ms, tracks[0]["root"] if tracks else tracks_asset.ROOT_SCENE)
    else:
        raise ValueError("entry has no source-to-engine conversion")
    return bytes(out)
