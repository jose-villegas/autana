"""Icons as image pack entries: a NAME.icons.toml describes a set, and each
icon in it bakes to its own one-bit image entry (gfx/image_asset.py's
encode_mono()), named by the icon. This module cuts the icons out of their
sources; main/gfx/draw/icon.h draws them.

A NAME.icons.toml names a PNG atlas of equal cells and lists the icons, each
a cell of the atlas or a pixelarticons SVG beside it:

    atlas = "system.png"
    cell_size = [16, 16]

    [[icon]]
    name = "check"
    at = [0, 0]                 # column, row of the atlas

    [[icon]]
    name = "close"
    svg = "system/close.svg"
    upstream = "close"          # the pixelarticons icon it came from
    commit = "8275e0af..."      # and the commit it was copied at

Every pixel is opaque black (ink) or opaque white: anything between is
refused with its coordinates, never thresholded. An SVG is one <path> of
closed axis-aligned rectangles on an integer grid, read exactly; a curve or
a fraction is refused. Every named cell has ink and every inked cell is
named. Standard library only.
"""

import pathlib
import re
import sys
import tomllib

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "render"))

from gfx import image_asset  # noqa: E402
from render_diff import read_png  # noqa: E402

SUFFIX = ".icons.toml"
NAME_MAX = 31  # an entry name's characters, as the pack table holds them
NAME = re.compile(r"^[a-z][a-z0-9_]*$")
INK, PAPER = (0, 0, 0), (255, 255, 255)
ICON_KEYS = {"name", "at", "svg", "upstream", "commit"}


class IconsError(ValueError):
    """An icon set that cannot be baked."""


# Every character a pixelarticons path may hold; anything else (a curve
# command, stray punctuation) is refused by name before tokenizing.
_D_ALLOWED_CHARS = set("MmHhVvZz0123456789.,+- \t\r\n")

# One command letter or one number, permissive about shape so a fraction is
# tokenized whole and refused with its own text.
_D_TOKEN_RE = re.compile(r"[A-Za-z]|-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?")


def _svg_path_rects(d, path):
    """Path data -> [(x0, y0, x1, y1)], each filling columns [x0, x1) of rows
    [y0, y1). Every M...Z subpath must close into one axis-aligned rectangle."""
    for ch in d:
        if ch not in _D_ALLOWED_CHARS:
            raise IconsError("%s: path data contains %r, which is not one of M/H/V/Z (any case) or an integer "
                             "coordinate; curves and other commands are not read" % (path, ch))
    tokens = _D_TOKEN_RE.findall(d)
    n = len(tokens)
    i = 0

    def next_number(ctx):
        nonlocal i
        if i >= n or tokens[i][:1].isalpha():
            raise IconsError("%s: path data: %s expects a coordinate" % (path, ctx))
        tok = tokens[i]
        if "." in tok or "e" in tok or "E" in tok:
            raise IconsError("%s: path data contains a non-integer coordinate %r" % (path, tok))
        i += 1
        return int(tok)

    rects = []
    cx = cy = 0
    while i < n:
        cmd = tokens[i]
        if not cmd[:1].isalpha():
            raise IconsError("%s: path data has coordinate %r with no command before it; implicit repeated "
                             "commands are not read" % (path, cmd))
        if cmd not in ("M", "m"):
            raise IconsError("%s: path data has %r outside a subpath (subpaths start with M/m)" % (path, cmd))
        i += 1
        x, y = next_number("M/m"), next_number("M/m")
        # cx/cy hold the previous subpath's start (Z resets the pen there),
        # and (0, 0) before the first: SVG's rule for a relative m.
        cx, cy = (cx + x, cy + y) if cmd == "m" else (x, y)
        sx, sy = cx, cy
        corners = [(cx, cy)]
        while True:
            if i >= n:
                raise IconsError("%s: subpath starting at (%d,%d) is never closed with Z" % (path, sx, sy))
            nxt = tokens[i]
            if nxt in ("Z", "z"):
                i += 1
                break
            if nxt in ("M", "m"):
                raise IconsError("%s: subpath starting at (%d,%d) is not closed with Z before the next begins"
                                 % (path, sx, sy))
            if nxt not in ("H", "h", "V", "v"):
                raise IconsError("%s: path data uses command %r; only M/H/V/Z (any case) are read" % (path, nxt))
            i += 1
            val = next_number(nxt)
            if nxt in ("H", "h"):
                cx = val if nxt == "H" else cx + val
            else:
                cy = val if nxt == "V" else cy + val
            corners.append((cx, cy))
        cx, cy = sx, sy
        # M + 3 H/V leaves Z the fourth edge; M + 4 H/V returns to the start
        # itself, so a last corner equal to the first is a duplicate.
        if len(corners) > 1 and corners[-1] == corners[0]:
            corners = corners[:-1]
        if len(corners) != 4:
            raise IconsError("%s: subpath starting at (%d,%d) has %d corners after closing (%r), not a rectangle's 4"
                             % (path, sx, sy, len(corners), corners))
        xs = sorted(set(p[0] for p in corners))
        ys = sorted(set(p[1] for p in corners))
        if len(xs) != 2 or len(ys) != 2:
            raise IconsError("%s: subpath starting at (%d,%d) is degenerate (corners %r)" % (path, sx, sy, corners))
        axes = []
        for k in range(4):
            p0, p1 = corners[k], corners[(k + 1) % 4]
            if p0[0] == p1[0] and p0[1] != p1[1]:
                axes.append("V")
            elif p0[1] == p1[1] and p0[0] != p1[0]:
                axes.append("H")
            else:
                raise IconsError("%s: subpath starting at (%d,%d) has a non-axis-aligned edge %r-%r"
                                 % (path, sx, sy, p0, p1))
        if any(axes[k] == axes[(k + 1) % 4] for k in range(4)):
            raise IconsError("%s: subpath starting at (%d,%d) is not a rectangle: its edges (%r) do not alternate"
                             % (path, sx, sy, axes))
        rects.append((xs[0], ys[0], xs[1], ys[1]))
    if not rects:
        raise IconsError("%s: path data has no subpaths" % path)
    return rects


def read_svg_icon(path):
    """A pixelarticons SVG -> rows of bools, ink True. Exactly one <path> and
    an integer viewBox."""
    text = pathlib.Path(path).read_text()
    path_tags = re.findall(r"<path\b[^>]*/?>", text)
    if len(path_tags) != 1:
        raise IconsError("%s: expected exactly one <path> element, found %d" % (path, len(path_tags)))
    d_match = re.search(r'\bd\s*=\s*"([^"]*)"', path_tags[0])
    if not d_match:
        raise IconsError("%s: the <path> element has no d attribute" % path)
    vb_match = re.search(r'\bviewBox\s*=\s*"([^"]*)"', text)
    if not vb_match:
        raise IconsError("%s: <svg> has no viewBox attribute" % path)
    try:
        min_x, min_y, width, height = (int(p) for p in vb_match.group(1).replace(",", " ").split())
    except ValueError:
        raise IconsError("%s: viewBox %r is not four integers" % (path, vb_match.group(1))) from None
    if width <= 0 or height <= 0:
        raise IconsError("%s: viewBox %r has no area" % (path, vb_match.group(1)))
    bits = [[False] * width for _ in range(height)]
    for x0, y0, x1, y1 in _svg_path_rects(d_match.group(1), path):
        rx0, ry0, rx1, ry1 = x0 - min_x, y0 - min_y, x1 - min_x, y1 - min_y
        if rx0 < 0 or ry0 < 0 or rx1 > width or ry1 > height:
            raise IconsError("%s: a rectangle at (%d,%d)-(%d,%d) falls outside the %dx%d viewBox"
                             % (path, x0, y0, x1, y1, width, height))
        for y in range(ry0, ry1):
            for x in range(rx0, rx1):
                bits[y][x] = True
    if not any(any(row) for row in bits):
        raise IconsError("%s: draws nothing" % path)
    return bits


def atlas_bits(path):
    """The atlas as rows of bools, ink True; any other pixel is refused."""
    try:
        image = read_png(pathlib.Path(path).read_bytes())
    except (OSError, ValueError) as error:
        raise IconsError(f"{path}: {error}") from None
    if not image.opaque:
        raise IconsError(f"{path}: has transparency; an icon pixel is opaque black or opaque white")
    rows = []
    for y in range(image.height):
        row = []
        for x in range(image.width):
            rgb = tuple(image.pixel(x, y))
            if rgb not in (INK, PAPER):
                raise IconsError("%s: pixel (%d,%d) is RGB%r, neither black (ink) nor white; "
                                 "re-export without grey" % (path, x, y, rgb))
            row.append(rgb == INK)
        rows.append(row)
    return image.width, image.height, rows


def atlas_cells(path, manifest):
    """{(column, row): rows of bools} of the set's atlas; none without one."""
    if "atlas" not in manifest:
        return {}
    cell_w, cell_h = manifest.get("cell_size", (0, 0))
    if not (0 < cell_w <= image_asset.SIDE_MAX and 0 < cell_h <= image_asset.SIDE_MAX):
        raise IconsError(f"{path}: cell_size is two sizes of 1 to {image_asset.SIDE_MAX}")
    atlas = path.parent / manifest["atlas"]
    width, height, grid = atlas_bits(atlas)
    if width % cell_w or height % cell_h:
        raise IconsError(f"{atlas} is {width}x{height}, not whole {cell_w}x{cell_h} cells")
    return {(col, row): [line[col * cell_w:(col + 1) * cell_w] for line in grid[row * cell_h:(row + 1) * cell_h]]
            for row in range(height // cell_h) for col in range(width // cell_w)}


def icon_bits(path, icon, cells):
    """One [[icon]]'s rows of bools, from its atlas cell or its SVG."""
    name = icon["name"]
    if "at" in icon:
        at = tuple(icon["at"])
        if at not in cells:
            raise IconsError(f"{path}: icon {name!r} at {list(at)} is outside the atlas")
        if not any(any(line) for line in cells[at]):
            raise IconsError(f"{path}: icon {name!r} at {list(at)} is an empty cell")
        return cells[at]
    if "svg" in icon:
        if not icon.get("upstream") or not icon.get("commit"):
            raise IconsError(f"{path}: icon {name!r} needs the upstream icon and the commit it came from")
        svg = path.parent / icon["svg"]
        if not svg.is_file():
            raise IconsError(f"{path}: icon {name!r} names {svg}, which does not exist")
        return read_svg_icon(svg)
    raise IconsError(f"{path}: icon {name!r} has neither at nor svg")


def manifest_of(path):
    """The set's TOML, its keys and its icons' names checked."""
    path = pathlib.Path(path)
    try:
        manifest = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise IconsError(f"{path}: {error}") from None
    unknown = set(manifest) - {"atlas", "cell_size", "icon"}
    if unknown:
        raise IconsError(f"{path}: unknown keys {sorted(unknown)}")
    if not manifest.get("icon"):
        raise IconsError(f"{path}: no [[icon]]")
    seen = set()
    for icon in manifest["icon"]:
        name = icon.get("name", "")
        if set(icon) - ICON_KEYS:
            raise IconsError(f"{path}: icon {name!r} has unknown icon keys {sorted(set(icon) - ICON_KEYS)}")
        if not NAME.match(name) or len(name) > NAME_MAX:
            raise IconsError(f"{path}: icon name {name!r} is not lower_snake_case of at most {NAME_MAX} characters")
        if name in seen:
            raise IconsError(f"{path}: icon {name!r} is named twice")
        seen.add(name)
    return manifest


def names(path):
    """The set's icon names, in its order: the pack entries it makes."""
    return [icon["name"] for icon in manifest_of(path)["icon"]]


def load(path):
    """{name: rows of bools} of the set at `path`, in its order, checked."""
    path = pathlib.Path(path)
    manifest = manifest_of(path)
    cells = atlas_cells(path, manifest)
    out = {icon["name"]: icon_bits(path, icon, cells) for icon in manifest["icon"]}
    named = {tuple(icon["at"]) for icon in manifest["icon"] if "at" in icon}
    for at, bits in sorted(cells.items()):
        if at not in named and any(any(line) for line in bits):
            raise IconsError(f"{path}: cell {list(at)} has ink but no icon names it")
    return out


def bake(path, name):
    """The image entry of icon `name` of the set at `path`."""
    return image_asset.encode_mono(load(path)[name])
