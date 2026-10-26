"""Bake a drawn curve from NAME.curve.toml into one named int16 table row."""

import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "device"))

from asset import table_asset
from build.c_header import read_define
from gfx.image_asset import load_source
from panel_size import PANEL_WIDTH, PANEL_HEIGHT
from render_diff import read_png

TYPE = table_asset.TYPE
SUFFIX = ".curve.toml"
ROW = "y"
VIEW_W, VIEW_H = PANEL_HEIGHT, PANEL_WIDTH
SPRING_LINE_HEADER = TOOLS.parent / "packages/physics/include/physics/spring/spring_line.h"
Q_SHIFT = read_define(SPRING_LINE_HEADER, "SPRING_LINE_OUT_SHIFT")
MAX_NEIGHBOUR_STEP_PX = 8
PEAK_DIVISOR = 2
GREY_RED = 19595
GREY_GREEN = 38470
GREY_BLUE = 7471
GREY_SHIFT = 16
GREY_ROUND = 1 << (GREY_SHIFT - 1)


def column_height(column, x):
    """Brightness-weighted centre of a column that crosses the curve once."""
    peak = max(column)
    lit = [(y, value) for y, value in enumerate(column) if value * PEAK_DIVISOR >= peak]
    if lit[-1][0] - lit[0][0] + 1 != len(lit):
        raise ValueError(f"column {x} crosses the line more than once")
    return sum(y * value for y, value in lit) / sum(value for _, value in lit)


def heights(image):
    """Subpixel heights of one continuous line across the panel's landscape frame."""
    if (image.width, image.height) != (VIEW_W, VIEW_H):
        raise ValueError(f"image is {image.width}x{image.height}, expected {VIEW_W}x{VIEW_H}")
    grey = []
    for y in range(VIEW_H):
        for x in range(VIEW_W):
            red, green, blue = image.pixel(x, y)
            grey.append((red * GREY_RED + green * GREY_GREEN + blue * GREY_BLUE + GREY_ROUND) >> GREY_SHIFT)
    brightest = max(grey)
    result = []
    for x in range(VIEW_W):
        column = grey[x::VIEW_W]
        if brightest == 0 or max(column) * PEAK_DIVISOR < brightest:
            raise ValueError(f"column {x} has no line")
        result.append(column_height(column, x))
    for x in range(1, VIEW_W):
        step = abs(result[x] - result[x - 1])
        if step > MAX_NEIGHBOUR_STEP_PX:
            raise ValueError(f"the line jumps {step:.1f} px between columns {x - 1} and {x}")
    return result


def curve_id(path):
    """The pack entry id of a curve root."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)


def bake(path):
    """The table entry of the PNG beside a curve root."""
    source, _ = load_source(path, allowed_keys=("source",))
    image = read_png(source.read_bytes())
    values = [round(height * (1 << Q_SHIFT)) for height in heights(image)]
    return table_asset.encode({ROW: values}, Q_SHIFT)
