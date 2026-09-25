"""Writes a .png beside every .bmp in a directory, if Pillow is installed.

A convenience, never a gate: tools/render/render_scene.sh already checked that each
BMP has the size its scene declared, so this adds nothing to the verdict and
says so once when Pillow is absent rather than failing the run that produced
the images.
"""
import os
import sys


def main(argv):
    if len(argv) != 1:
        raise SystemExit("usage: render_png.py <directory>")

    try:
        from PIL import Image
    except ImportError:
        print("Pillow not installed - BMP only, no PNG written")
        return

    directory = argv[0]
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".bmp"):
            continue
        path = os.path.join(directory, name)
        png = path[:-4] + ".png"
        Image.open(path).save(png)
        print("png %s" % png)


if __name__ == "__main__":
    main(sys.argv[1:])
