"""Writes a .png beside every .bmp in a directory, if Pillow and numpy are installed.

A convenience, never a gate: tools/render/render_scene.sh already checked that each
BMP has the size its scene declared, so this adds nothing to the verdict and
says so once when either is absent rather than failing the run that produced
the images.
"""
import os
import pathlib
import sys


def main(argv):
    if len(argv) != 1:
        raise SystemExit("usage: render_png.py <directory>")

    try:
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
        from docs.figures import save_png
    except ImportError:
        print("Pillow or numpy not installed - BMP only, no PNG written")
        return

    directory = argv[0]
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".bmp"):
            continue
        path = os.path.join(directory, name)
        png = path[:-4] + ".png"
        save_png(path, png)
        print("png %s" % png)


if __name__ == "__main__":
    main(sys.argv[1:])
