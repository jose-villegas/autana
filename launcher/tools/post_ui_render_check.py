"""Checks the BMPs post_ui_render_host.sh just wrote, and converts them.

Takes `<path>:<width>:<height>` arguments and fails if any file is missing,
is not a BMP, is empty of pixel data, or does not have the dimensions the
renderer was asked for - so a tool that has quietly stopped producing an
image fails the run that produced it rather than leaving a picture nobody
looks at twice.

Writes a .png beside each BMP when Pillow is installed. It is not a
dependency: without it the BMPs are the deliverable and this says so once.
"""
import struct
import sys

BMP_HEADER_SIZE = 54


def check(path, want_w, want_h):
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"BM":
        raise SystemExit("%s: not a BMP" % path)
    if len(data) <= BMP_HEADER_SIZE:
        raise SystemExit("%s: header only, no pixels" % path)

    width, height = struct.unpack("<ii", data[18:26])
    if (width, height) != (want_w, want_h):
        raise SystemExit("%s: got %dx%d, expected %dx%d"
                         % (path, width, height, want_w, want_h))
    return len(data)


def main(argv):
    if not argv:
        raise SystemExit("usage: post_ui_render_check.py <path>:<w>:<h> ...")

    try:
        from PIL import Image
    except ImportError:
        Image = None

    for spec in argv:
        path, want_w, want_h = spec.rsplit(":", 2)
        size = check(path, int(want_w), int(want_h))
        note = ""
        if Image is not None:
            png = path[:-4] + ".png" if path.endswith(".bmp") else path + ".png"
            Image.open(path).save(png)
            note = " -> %s" % png
        print("ok %s %sx%s %d bytes%s" % (path, want_w, want_h, size, note))

    if Image is None:
        print("Pillow not installed - BMP only, no PNG written")


if __name__ == "__main__":
    main(sys.argv[1:])
