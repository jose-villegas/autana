"""A render scored against reference frames must stand where they were traced.

render_compare.sh keeps beside each traced reference set the poses its frames
were traced at (REFERENCE_POSES, a tools/anim/track_host.py poses file, frame i
at pose i). The render harness writes the pose of every frame it saves to a
video beside that video (VIDEO + VIDEO_POSES_SUFFIX, render_host.c: the active
camera's engine position and rotation). checker() refuses a scored frame that
has no row or stands anywhere else than its reference frame, so a figure drawn
from the wrong camera (#1597) fails rather than publishing its error as the
bake's.
"""
import csv
import json
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.engine_frame import AXIS_SIGNS  # noqa: E402
from r3d.poses import read_poses  # noqa: E402

REFERENCE_POSES = "poses.txt"
VIDEO_POSES_SUFFIX = ".poses.csv"
# Both sides print float32 poses to nine digits from the same clip, so they
# agree to rounding; a hundredth of a world unit, or of a unit forward
# component, is far below any step a camera takes between frames.
POSE_TOLERANCE = 0.01
# The engine camera looks along its local +Z.
ENGINE_FORWARD = np.array([0.0, 0.0, 1.0])


def rotate(quaternion, vector):
    """`vector` turned by the unit quaternion (x, y, z, w)."""
    axis, w = np.asarray(quaternion[:3]), quaternion[3]
    return vector + 2.0 * np.cross(axis, np.cross(axis, vector) + w * vector)


def _pose_rows(video):
    path = pathlib.Path(str(video) + VIDEO_POSES_SUFFIX)
    if not path.is_file():
        return None
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def render_poses(video):
    """{frame: (position, forward)} of each frame the harness saved to
    `video`, in the source's axes, or None when it left no poses."""
    rows = _pose_rows(video)
    if rows is None:
        return None
    signs = np.asarray(AXIS_SIGNS, dtype=float)
    return {int(row["frame"]): (np.array([float(row[axis]) for axis in ("px", "py", "pz")]) * signs,
                                rotate([float(row[axis]) for axis in ("qx", "qy", "qz", "qw")], ENGINE_FORWARD) * signs)
            for row in rows}


def capture_poses(path, size=None):
    """An id capture's logged source-frame poses and its recorded raster lens and size."""
    path = pathlib.Path(path)
    if not str(path).endswith(VIDEO_POSES_SUFFIX):
        raise ValueError(f'{path}: expected a {VIDEO_POSES_SUFFIX} pose log')
    video = str(path).removesuffix(VIDEO_POSES_SUFFIX)
    drawn = render_poses(video)
    if drawn is None or sorted(drawn) != list(range(len(drawn))):
        raise ValueError(f'{path}: poses must cover every frame in order')
    metadata = json.loads((path.parent / 'entries.json').read_text())
    width, height = size or (metadata['width'], metadata['height'])
    return width, height, metadata['lens'], metadata['near'], [np.concatenate(drawn[i]) for i in range(len(drawn))]


def capture_views(path, size=None):
    """Clip matrices and upright sizes of captured frames, including the logged camera roll."""
    from r3d.appearance_simplify import projection

    path = pathlib.Path(path)

    width, height, lens, near, poses = capture_poses(path, size)
    rows = _pose_rows(str(path).removesuffix(VIDEO_POSES_SUFFIX))
    by_frame = {int(row['frame']): row for row in rows}
    signs = np.asarray(AXIS_SIGNS, dtype=float)
    axes = len(signs)
    metadata = json.loads((path.parent / 'entries.json').read_text())
    viewport = metadata.get('viewport_width', width), metadata.get('viewport_height', height)
    canonical = projection(np.zeros(axes), ENGINE_FORWARD * signs, *viewport, lens, near)
    result = []
    for frame, pose in enumerate(poses):
        quaternion = [float(by_frame[frame][axis]) for axis in ('qx', 'qy', 'qz', 'qw')]
        right = rotate(quaternion, np.eye(axes)[0]) * signs
        up = rotate(quaternion, np.eye(axes)[1]) * signs
        camera = np.eye(axes + 1)
        camera[:axes, :axes] = np.stack([right, up, -pose[axes:]])
        camera[:axes, axes] = -camera[:axes, :axes] @ pose[:axes]
        result.append((canonical @ camera, (width, height)))
    return result


def checker(video, reference_dir, poses_path=None):
    """A check(frame, index) that raises ValueError unless frame `frame` of
    `video` stands at the pose frame `index` of `reference_dir` was traced at,
    or None when the reference set records no poses."""
    recorded = pathlib.Path(poses_path) if poses_path is not None else pathlib.Path(reference_dir) / REFERENCE_POSES
    if not recorded.is_file():
        return None
    drawn = render_poses(video)
    if drawn is None:
        raise ValueError(f"{video}: no {VIDEO_POSES_SUFFIX} beside it, so it cannot be scored against "
                         f"{reference_dir}, whose frames record their poses: render it through the render harness")
    traced = read_poses(recorded)[4]

    def check(frame, index):
        if frame not in drawn:
            raise ValueError(f"{video} frame {frame} has no row in its {VIDEO_POSES_SUFFIX}: the renderer predates "
                             "the pose log or wrote no camera pose, so it cannot be scored against a posed reference")
        if index >= len(traced):
            raise ValueError(f"{video} frame {frame} has no pose in {recorded}, which traces {len(traced)}")
        eye, forward = drawn[frame]
        expected = traced[index]
        want_forward = expected[3:] / np.linalg.norm(expected[3:])
        if (np.abs(eye - expected[:3]).max() > POSE_TOLERANCE
                or np.abs(forward - want_forward).max() > POSE_TOLERANCE):
            raise ValueError(f"{video} frame {frame} stands at {np.round(eye, 3).tolist()} looking "
                             f"{np.round(forward, 3).tolist()}, its reference frame {index} at "
                             f"{np.round(expected[:3], 3).tolist()} looking {np.round(want_forward, 3).tolist()}: "
                             "the render and its reference are not at the same pose")

    return check
