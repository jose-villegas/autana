"""How well a forward-warped frame stands in for a rendered one along a
scene's camera path: the host renderer draws the path at one render size
with --dump, then every SAMPLE_EVERY-th frame is warped AHEAD display steps
on (warp.py) and scored against the frame really drawn there with the
reference scorer (render_compare.reference_measure). Repeating the last
frame, what the panel shows today between renders, is scored beside it.

    framegen_score.py --renderer EXE --scene KEY --scene-file SCENE.toml
        --camera NAME --size WxH --csv OUT.csv
        [--video OUT.mp4] [--still OUT.png] [--clip OUT.gif] [--work DIR]

--video writes every frame of the path: the true frame, the warp of the one
before it with its holes filled, and the holes in red over the warp.
--still writes that picture for the sampled frame whose one-step warp opens
the most holes that are not pinholes (disocclusions and screen edges), and
--clip CLIP_FRAMES of them around it as a GIF at the display rate.
Needs Pillow and numpy (tools/r3d/requirements.txt).
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parents[1]
sys.path[:0] = [str(TOOLS), str(TOOLS / "render"), str(TOOLS / "device"), str(HERE)]
import render_compare  # noqa: E402
import warp  # noqa: E402
from build.c_header import read_define  # noqa: E402
from panel_size import PANEL_HEIGHT, PANEL_WIDTH  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.poses import sample_camera_rows  # noqa: E402

# The panel the raster stretches to, portrait (the dynres ladder's shape).
PANEL = (PANEL_WIDTH, PANEL_HEIGHT)
# One displayed frame: the render lab's frame target.
DISPLAY_STEP_MS = read_define(TOOLS.parent / "main/apps/render_lab/render_lab_dynres.h",  # magic: a header path, not a name
                              "RENDER_LAB_FRAME_TARGET_MS")
# Display steps a generated frame looks ahead of the frame it warps: one for
# 1 rendered + 1 generated, two for the second of 1 + 2.
AHEAD = (1, 2)
SAMPLE_EVERY = 20
# Host frame i is drawn after its frame() advanced the clock by one step,
# at (i + 1) steps: the convention dynres_quality.sh reads poses by.
FRAME_POSE_LAG = 1
HOLE_RED = (255, 0, 0)
VIDEO_ZOOM = 2
# A GIF's frames at the raster's own size, so a clip stays a docs-sized file.
CLIP_ZOOM = 1
CLIP_COLOURS = 64
CLIP_FRAMES = 60
MS_PER_SECOND = 1000
FILLS = ("row", "previous")
CSV_HEADER = ("scene,width,height,frame,ahead,holes_pct,pinholes_pct,fill,mean_delta_e,p95_delta_e,ssim")


def camera_poses(scene_file, camera_name):
    """(lens, near, poses) of the camera's path, one per display step."""
    scene = load_scene(scene_file)
    camera = next(obj.component for obj in scene.objects if obj.kind == "camera" and obj.name == camera_name)
    _, _, lens, near, rows = sample_camera_rows(camera.path.animation, camera.path.node, DISPLAY_STEP_MS, *PANEL,
                                                camera.half_fov_short_tan, camera.near_z)
    return lens, near, np.array(rows)


def render_path(renderer, scene_key, camera_name, size, frames, out):
    subprocess.run([str(renderer), "--quarter", "0", "--no-hud", "--scene", scene_key, "--camera", camera_name,
                    "--size", size, "--frames", str(frames), "--dt", str(DISPLAY_STEP_MS), "-o", str(out / "last.bmp"),
                    "--dump", str(out)], check=True, capture_output=True)


def filled(fill, colour, depth, written, previous_colour):
    if fill == "row":
        return warp.fill_row(colour, depth, written)
    return warp.fill_previous(colour, written, previous_colour)


def score(frame_path, lens, poses, frame, ahead):
    """Rows (holes %, fill, stats) for frame `frame` warped `ahead` steps."""
    colour, depth = warp.read_dump(frame_path(frame))
    truth = Image.fromarray(warp.rgb_of(warp.read_dump(frame_path(frame + ahead))[0]))
    pose = poses[frame + FRAME_POSE_LAG]
    moved, moved_depth, written = warp.warp(colour, depth, lens, pose, poses[frame + ahead + FRAME_POSE_LAG])
    holes = (100.0 * float((~written).mean()), 100.0 * float(warp.pinholes(written).mean()))
    rows = [(holes, "repeat", render_compare.reference_measure(Image.fromarray(warp.rgb_of(colour)), truth))]
    for fill in FILLS:
        out = filled(fill, moved, moved_depth, written, colour)
        rows.append((holes, fill, render_compare.reference_measure(Image.fromarray(warp.rgb_of(out)), truth)))
    return rows


def video_frame(frame_path, lens, poses, frame, zoom=VIDEO_ZOOM):
    """True | warped from the frame before, row-filled | holes in red."""
    colour, depth = warp.read_dump(frame_path(frame - 1))
    truth = warp.rgb_of(warp.read_dump(frame_path(frame))[0])
    moved, moved_depth, written = warp.warp(colour, depth, lens, poses[frame - 1 + FRAME_POSE_LAG],
                                            poses[frame + FRAME_POSE_LAG])
    shown = warp.rgb_of(warp.fill_row(moved, moved_depth, written))
    holes = np.where(written[..., None], warp.rgb_of(moved), np.array(HOLE_RED, dtype=np.uint8))
    panels = [("true", truth), ("warped + row fill", shown), ("holes %.1f%%" % (100.0 * (~written).mean()), holes)]
    tiles = []
    for label, rgb in panels:
        big = Image.fromarray(rgb).resize((rgb.shape[1] * zoom, rgb.shape[0] * zoom), Image.NEAREST)
        tiles.append(np.asarray(render_compare.captioned(big, label)))
    return np.concatenate(tiles, axis=1)


def write_video(frame_path, lens, poses, frames, out):
    video = render_compare.VideoWriter(str(out), MS_PER_SECOND / DISPLAY_STEP_MS)
    for frame in range(1, frames):  # from the first frame with one before it
        video.add(Image.fromarray(video_frame(frame_path, lens, poses, frame)))
    video.close()


def write_clip(frame_path, lens, poses, frames, out):
    """One palette for every frame, taken from all of them, so the clip does not flicker."""
    shots = [Image.fromarray(video_frame(frame_path, lens, poses, frame, CLIP_ZOOM)) for frame in frames]
    strip = Image.fromarray(np.concatenate([np.asarray(shot) for shot in shots], axis=0))
    palette = strip.quantize(colors=CLIP_COLOURS)
    quantized = [shot.quantize(palette=palette, dither=Image.Dither.NONE) for shot in shots]
    quantized[0].save(out, save_all=True, append_images=quantized[1:], duration=DISPLAY_STEP_MS, loop=0,
                      optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--renderer", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--scene-file", required=True)
    parser.add_argument("--camera", required=True)
    parser.add_argument("--size", required=True)
    parser.add_argument("--csv", required=True)
    parser.add_argument("--video")
    parser.add_argument("--still")
    parser.add_argument("--clip")
    parser.add_argument("--work")
    args = parser.parse_args()
    width, height = map(int, args.size.split("x"))
    lens_tan, near, poses = camera_poses(args.scene_file, args.camera)
    lens = warp.Lens(*PANEL, lens_tan, near, width, height)
    frames = len(poses) - FRAME_POSE_LAG
    work = pathlib.Path(args.work or tempfile.mkdtemp(prefix="framegen_"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        render_path(args.renderer, args.scene, args.camera, args.size, frames, work)

        def frame_path(index):
            return work / ("%04d.raw" % index)

        gaps = {}
        with open(args.csv, "w", newline="\n") as csv:
            csv.write(CSV_HEADER + "\n")
            for frame in range(0, frames - max(AHEAD), SAMPLE_EVERY):
                for ahead in AHEAD:
                    for holes, fill, stats in score(frame_path, lens, poses, frame, ahead):
                        csv.write("%s,%d,%d,%d,%d,%.3f,%.3f,%s,%.3f,%.3f,%.4f\n" % (
                            args.scene, width, height, frame, ahead, *holes, fill, stats.mean_delta_e,
                            stats.p95_delta_e, stats.ssim_luma))
                        if ahead == AHEAD[0]:
                            gaps[frame] = holes[0] - holes[1]
        shown = max(gaps, key=gaps.get) + AHEAD[0]
        if args.still:
            Image.fromarray(video_frame(frame_path, lens, poses, shown)).save(args.still)
        if args.clip:
            first = min(max(shown - CLIP_FRAMES // 2, 1), frames - CLIP_FRAMES)
            write_clip(frame_path, lens, poses, range(first, first + CLIP_FRAMES), args.clip)
        if args.video:
            write_video(frame_path, lens, poses, frames, args.video)
    finally:
        if not args.work:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
