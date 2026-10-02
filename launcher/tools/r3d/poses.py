"""The camera poses file tools/anim/sample_tracks.sh writes: `size W H`,
`lens TAN NEAR`, then one `pose EYE_XYZ FORWARD_XYZ` line per sample."""

import pathlib

import numpy as np


def read_poses(path):
    """Read a track sampler pose file as (width, height, lens, near, poses)."""
    width = height = None
    lens = near = None
    poses = []
    for line in pathlib.Path(path).read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "size" and len(fields) == 3:
            width, height = map(int, fields[1:])
        elif fields[0] == "lens" and len(fields) == 3:
            lens, near = map(float, fields[1:])
        elif fields[0] == "pose" and len(fields) == 7:
            poses.append(np.array([float(value) for value in fields[1:]], dtype=float))
        else:
            raise ValueError("invalid pose line: " + line)
    if width is None or lens is None or not poses:
        raise ValueError("poses need size, lens and at least one pose")
    return width, height, lens, near, poses


def camera_basis(forward):
    """(right, up, forward) unit vectors of a camera looking along `forward`
    with world +y up: the frame reference_render.py casts its rays in."""
    forward = np.asarray(forward, dtype=float)
    forward = forward / np.linalg.norm(forward)
    right = np.cross(forward, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    return right, np.cross(right, forward), forward
