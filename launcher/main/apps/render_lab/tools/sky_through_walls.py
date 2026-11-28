"""Measure enclosed sky holes against the source over a rendered camera flight."""
import os
import pathlib
import subprocess
import sys

from render_host_build import run

LAUNCHER = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(LAUNCHER / "tools" / "render"))
sys.path.insert(0, str(LAUNCHER / "tools"))

import numpy as np
from scipy import ndimage

from r3d.poses import flight_camera, sample_camera_rows
from pinhole_rays import VIDEO_SCALE, enclosed_sky, flythrough_poses, grid_rays
from check_avi import read_video
from render_compare import expand_565
from see_through import SceneSources

DT_MS = 33
MIN_OPEN_SKY_PIXELS = 25
FRAME_COUNT_SIZE = (1, 1)
# A pixel and the eight around it.
BESIDE = np.ones((3, 3), dtype=bool)
# Rays per side of a render pixel when asking the source for sky. A notch in a jagged rim or the gap under a
# flagpole opens to the sky through a channel narrower than a pixel, so a ray through the centre can hit the
# wall beside it.
SUBPIXELS = 8
# Per scene, the most holed frames a bake may show: the holed frames of a clean bake plus one.
MOST_HOLED_FRAMES = {"sponza": 1}


def clear_colour(scene):
    """The scene's camera background as the 16-bit framebuffer shows it."""
    rgb = scene.camera.component.background
    return expand_565((rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF))


def sky_masks(frame, sky, source_sky=None):
    """The enclosed-hole and open-sky masks of an RGB frame. With `source_sky`, which answers
    whether the source shows sky at the (ys, xs) it is given,
    there, an enclosed pixel is no hole when the source shows sky in it or in an enclosed pixel beside it, by
    side or corner: the bake drew that sky at most a pixel wider than the source."""
    mask, enclosed = enclosed_sky(frame, sky)
    open_ = mask & ~enclosed
    if source_sky is None:
        return enclosed, open_
    shown = np.zeros_like(enclosed)
    ys, xs = np.nonzero(enclosed)
    if len(ys):
        shown[ys, xs] = source_sky(ys, xs)
    return enclosed & ~ndimage.binary_dilation(shown, BESIDE), open_


def sky_pixels(frame, sky, source_sky=None):
    """Count enclosed-hole and open-sky pixels in an RGB frame."""
    return tuple(int(mask.sum()) for mask in sky_masks(frame, sky, source_sky))


def pixel_sky(query, width, height, lens, near, pose, ys, xs, samples=SUBPIXELS):
    """Whether `query`'s mesh leaves any of `samples` x `samples` rays spread over each render pixel (ys, xs) of
    a width x height view from `pose` empty: sky the source shows in part of a pixel counts."""
    ox, oy = np.meshgrid(np.arange(samples), np.arange(samples))
    pixels = (xs[:, None] * samples + ox.ravel(), ys[:, None] * samples + oy.ravel())
    origins, directions = grid_rays(width, height, lens, near, pose, (width * samples, height * samples), pixels)
    return ~query.blocked(origins, directions).reshape(-1, samples * samples).all(axis=1)


class SourceSky:
    """Where the scene's source meshes show sky in a flythrough frame's render pixel (pixel_sky)."""

    def __init__(self, scene, frames, size, sources=None):
        sources = SceneSources(scene) if sources is None else sources
        self.query = sources.query(drop_alpha=True)
        self.width, self.height, self.lens, self.near, self.poses = flythrough_poses(flight_camera(scene), DT_MS,
                                                                                     size, frames)

    def of_frame(self, frame):
        """`source_sky` of sky_pixels for the flythrough's frame `frame`, drawn at poses[frame + 1]."""
        def source_sky(ys, xs):
            return pixel_sky(self.query, self.width, self.height, self.lens, self.near, self.poses[frame + 1], ys, xs)
        return source_sky



class SkyUnavailable(ValueError):
    """The scene lacks a prerequisite for the sky measure."""


def measure_sky(scene, key, out, binary=None, sources=None, assets=None):
    """Holed frame indices and total frames at the device flythrough step."""
    if not scene.renderers:
        raise SkyUnavailable(f"{key}: the scene draws no source meshes")
    camera = flight_camera(scene)
    frames = len(sample_camera_rows(camera.path.animation, camera.path.node, DT_MS, *FRAME_COUNT_SIZE,
                                    camera.half_fov_short_tan, camera.near_z)[4])
    if binary is None:
        built, binary = run(out, "--build-only")
        if built.returncode:
            raise RuntimeError((built.stdout + built.stderr)[-2000:])
    video = out / f"{key}.avi"
    env = None if assets is None else dict(os.environ, AUTANA_ASSET_DIR=str(assets))
    rendered = subprocess.run(
        [str(binary), "--quarter", "1", "--no-hud", "--scene", key, "--frames", str(frames), "--dt", str(DT_MS),
         "-o", str(out / f"{key}.bmp"), "--video", str(video)], capture_output=True, text=True, timeout=600, env=env)
    if rendered.returncode:
        raise RuntimeError(rendered.stderr[-2000:])
    sky = clear_colour(scene)
    counts, source = [], None
    for index, frame in enumerate(read_video(video)[1]):
        frame = np.ascontiguousarray(frame[::VIDEO_SCALE, ::VIDEO_SCALE])
        source = source or SourceSky(scene, frames, frame.shape[1::-1], sources)
        counts.append(sky_pixels(frame, sky, source.of_frame(index)))
    video.unlink()
    if len(counts) != frames:
        raise ValueError(f"{key}: expected {frames} frames, read {len(counts)}")
    if not any(open_ >= MIN_OPEN_SKY_PIXELS for _, open_ in counts):
        raise SkyUnavailable(f"{key}: no open sky found to confirm the clear colour")
    return [frame for frame, (holes, _) in enumerate(counts) if holes > 0], frames
