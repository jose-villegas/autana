"""Say whether two image files show the same pixels.

    compare_images.py <a> <b> [--tolerance N]

Encoders write different bytes for the same picture (Pillow, ffmpeg and their
palette quantizers all change between versions), so files are never compared as
bytes: every frame is decoded to RGB and the frames are compared one by one.
Size and frame count are compared first; a frame counts as different when any
channel of any pixel differs by more than --tolerance (default 0).

Exit status: 0 the images show the same pixels, 1 they differ, 2 a file could
not be read. One line is printed either way, naming the first difference.

Needs Pillow.
"""

import argparse
import sys

from PIL import Image, ImageChops, ImageSequence


def load_frames(path):
    """Every frame as an RGB image, animated or not."""
    with Image.open(path) as image:
        return [frame.convert("RGB") for frame in ImageSequence.Iterator(image)]


def frame_difference(a, b):
    """The largest channel difference between two same-sized frames, and how many pixels differ."""
    diff = ImageChops.difference(a, b)
    peak = max(high for _, high in diff.getextrema())
    if peak == 0:
        return 0, 0
    mask = diff.convert("L").point(lambda level: 255 if level else 0)
    return peak, sum(mask.histogram()[1:])


def compare(a_frames, b_frames, tolerance=0):
    """None when the images show the same pixels, else a sentence saying how they differ."""
    if a_frames[0].size != b_frames[0].size:
        return "size %dx%d vs %dx%d" % (*a_frames[0].size, *b_frames[0].size)
    if len(a_frames) != len(b_frames):
        return "%d frames vs %d frames" % (len(a_frames), len(b_frames))
    for index, (a, b) in enumerate(zip(a_frames, b_frames)):
        peak, pixels = frame_difference(a, b)
        if peak > tolerance:
            return "frame %d: %d pixels differ, by up to %d" % (index, pixels, peak)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--tolerance", type=int, default=0, help="largest per-channel difference still counted equal")
    args = parser.parse_args()
    try:
        a_frames = load_frames(args.a)
        b_frames = load_frames(args.b)
    except OSError as error:
        print("unreadable: %s" % error)
        return 2
    difference = compare(a_frames, b_frames, args.tolerance)
    if difference is None:
        print("same pixels (%d frame%s)" % (len(a_frames), "" if len(a_frames) == 1 else "s"))
        return 0
    print("different: %s" % difference)
    return 1


if __name__ == "__main__":
    sys.exit(main())
