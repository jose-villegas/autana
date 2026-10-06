"""The camera poses file tools/anim/track_host.py writes: `size W H`,
`lens TAN NEAR`, then one `pose EYE_XYZ FORWARD_XYZ` line per sample."""

import pathlib

import numpy as np


def read_poses(path):
    """Read a track sampler pose file as (width, height, lens, near, poses)."""
    return parse_poses(pathlib.Path(path).read_text())


def parse_poses(text):
    """read_poses for the file's text."""
    width = height = None
    lens = near = None
    poses = []
    for line in text.splitlines():
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


def camera_rays(width, height, lens, eye, forward, samples, margin=0):
    """One pinhole ray per subpixel, ordered in pixel-sized groups; `margin`
    widens the view by that many pixels on each side."""
    right, up, forward = camera_basis(forward)
    x, y = np.meshgrid(np.arange(-margin, width + margin), np.arange(-margin, height + margin))
    offsets = (np.arange(samples) + 0.5) / samples
    ox, oy = np.meshgrid(offsets, offsets)
    x = (x[..., None] + ox.ravel()).reshape(-1)
    y = (y[..., None] + oy.ravel()).reshape(-1)
    short = min(width, height)
    horizontal = lens * width / short
    vertical = lens * height / short
    direction = forward + right * ((2 * x / width - 1) * horizontal)[:, None]
    direction += up * ((1 - 2 * y / height) * vertical)[:, None]
    direction /= np.linalg.norm(direction, axis=1, keepdims=True)
    return np.repeat(np.asarray(eye, dtype=float)[None, :], len(direction), axis=0), direction


def either_way(width, height, lens, near, poses):
    """Poses as a square view as wide as the long side, which covers the
    panel held either way up."""
    side = max(width, height)
    return side, side, lens * side / min(width, height), near, poses


def sample_camera_path(animation, node, every_ms, width, height, lens, near):
    """The poses of camera node `node` in the clip a NAME.anim.toml names,
    every `every_ms` over the clip, as read_poses returns them; sampled by
    tools/anim/track_host.py, the device's sampler over the clip's TRCK entry."""
    from anim import track_host

    return parse_poses(track_host.poses(animation, node, every_ms, width, height, lens, near))
