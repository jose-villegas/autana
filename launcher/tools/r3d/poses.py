"""The camera poses file tools/anim/sample_tracks.sh writes: `size W H`,
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


def tracks_file(settings, scene):
    """The C file of the scene camera's baked tracks, beside the import's
    output."""
    return settings.out_dir / f"{scene.camera.component.path.tracks}_tracks_generated.c"


SAMPLE_TRACKS = pathlib.Path(__file__).resolve().parents[1] / "anim" / "sample_tracks.sh"


def sample_camera_path(tracks_source, tracks_name, node, every_ms, width, height, lens, near):
    """The poses of a baked camera track, every `every_ms` over its clip, as
    read_poses returns them; built and run by tools/anim/sample_tracks.sh."""
    import subprocess

    done = subprocess.run(["sh", SAMPLE_TRACKS.as_posix(), "--tracks", f"{pathlib.Path(tracks_source).as_posix()}:{tracks_name}",
                           "--every", str(every_ms), "--poses", node, str(width), str(height), repr(lens), repr(near)],
                          capture_output=True, text=True, check=True)
    return parse_poses(done.stdout)
