"""Temporal instruments for synchronized host renders, alongside render_compare.

The 4x4 box blur measures triangle-scale pops; ordered dither reads as noise,
so judge dither by eye. --video-column composes a labelled look-range clip.
"""
import argparse
import csv
import pathlib
import subprocess

import numpy as np
from PIL import Image

from render_compare import _box_mean, _ffmpeg_command, captioned, read_video

TEMPORAL_BLOCK_SIDE = 4
TEMPORAL_POP_THRESHOLD = 8
CHANNEL_MAX = 255
TEMPORAL_COLUMNS = ("extra_change", "blocks_popping_pct")


def temporal_change(previous_a, a, previous_b, b):
    """Extra frame-to-frame change after a sliding box mean of mean RGB luma.

    Returns mean absolute change /255 and percent of windows above the pop threshold.
    """
    frames = [np.asarray(frame, dtype=float) for frame in (previous_a, a, previous_b, b)]
    if any(frame.shape != frames[0].shape for frame in frames):
        raise ValueError("temporal frames must have the same shape")
    size = min(TEMPORAL_BLOCK_SIDE, *frames[0].shape[:2])
    pa, ca, pb, cb = [_box_mean(frame.mean(axis=2), size) for frame in frames]
    difference = np.abs((cb - pb) - (ca - pa))
    return float(difference.mean() / CHANNEL_MAX), float(100 * np.mean(difference > TEMPORAL_POP_THRESHOLD))


def video_columns(columns, out):
    """Compose synchronized host videos under their labels."""
    videos = [read_video(path) for _, path in columns]
    if len({rate for rate, _ in videos}) != 1:
        raise ValueError("video columns need equal frame rates")
    process = None
    try:
        for frames in zip(*(frames for _, frames in videos), strict=True):
            panels = [captioned(Image.fromarray(frame), label) for (label, _), frame in zip(columns, frames)]
            picture = Image.fromarray(np.concatenate([np.asarray(panel) for panel in panels], axis=1))
            if process is None:
                process = subprocess.Popen(_ffmpeg_command("ffmpeg", picture.size, videos[0][0], out),
                                           stdin=subprocess.PIPE)
            process.stdin.write(picture.tobytes())
    finally:
        if process is not None:
            process.stdin.close()
            if process.wait():
                raise RuntimeError("ffmpeg failed writing %s" % out)
    if process is None:
        raise ValueError("no video frames")


def compare_temporal(path_a, path_b, csv_path):
    """Append temporal columns to render_compare's CSV and return a summary."""
    fps_a, frames_a = read_video(path_a)
    fps_b, frames_b = read_video(path_b)
    if fps_a != fps_b:
        raise ValueError("temporal videos need equal frame rates")
    with pathlib.Path(csv_path).open(newline="") as handle:
        reader = csv.DictReader(handle)
        fields = [name for name in reader.fieldnames if name not in TEMPORAL_COLUMNS]
        rows = list(reader)
    previous = None
    changes = []
    count = 0
    for index, (a, b) in enumerate(zip(frames_a, frames_b, strict=True)):
        if index >= len(rows):
            raise ValueError("CSV has fewer rows than the videos")
        change = temporal_change(previous[0], a, previous[1], b) if previous else (0.0, 0.0)
        if previous is not None:
            changes.append(change)
        previous = (a, b)
        rows[index].update(zip(TEMPORAL_COLUMNS, (f"{change[0]:.8f}", f"{change[1]:.4f}")))
        count += 1
    if count != len(rows) or not count:
        raise ValueError("CSV and videos need equal nonzero frame counts")
    with pathlib.Path(csv_path).open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields + list(TEMPORAL_COLUMNS))
        writer.writeheader()
        writer.writerows(rows)
    extra, popping = np.mean(changes, axis=0) if changes else (0.0, 0.0)
    return ("extra frame-to-frame change %.8f (mean /255), blocks popping %.4f%%"
            % (extra, popping))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", nargs=2, metavar=("A.avi", "B.avi"))
    parser.add_argument("--csv", help="existing render_compare per-frame CSV to extend")
    parser.add_argument("--summary", help="append the temporal summary here too")
    parser.add_argument("--video-column", nargs=2, action="append", metavar=("LABEL", "AVI"))
    parser.add_argument("--out", help="labelled look-range video")
    args = parser.parse_args()
    if args.video_column:
        if not args.out:
            parser.error("--video-column needs --out")
        video_columns(args.video_column, args.out)
    elif args.video and args.csv:
        summary = compare_temporal(*args.video, args.csv)
        print(summary)
        if args.summary:
            with open(args.summary, "a") as handle:
                handle.write(summary + "\n")
    else:
        parser.error("needs --video A B --csv FILE, or --video-column LABEL AVI --out FILE")


if __name__ == "__main__":
    main()
