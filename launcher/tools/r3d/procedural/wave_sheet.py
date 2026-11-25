"""A coloured sheet bent by a wave, with the formula's own normals: a mesh any test, bake or figure can make
from a few numbers, and whose true shape is known everywhere.

    sheet = wave_sheet.sheet(32, shape="zigzag", amplitude=0.1, frequency=(4, 0), pattern="stripes", cells=4)
    wave_sheet.write_source(sheet, out_dir, "zigzag")   # zigzag.obj + .mtl, what mesh_import reads
    wave_sheet.write_start(sheet, out_dir, "zigzag")    # zigzag.bundle, what the appearance fit starts from

The sheet lies in x and y over `size` around `centre`, facing +z, `count` quads a side, two triangles each,
counter-clockwise seen from the side it faces. Its height (the `Surface`) is

    z = amplitude * w(fx * u + phase_x) * w(fy * v + phase_y)

with u and v running 0 to 1 across the sheet and w the wave `shape` at one cycle per unit (a frequency of 0
leaves that axis's factor at 1). A sine is smooth; a zigzag (triangle wave) has a crease at each peak and
trough, a sawtooth a ramp and then a drop one quad wide: the folds where normals swing hardest over a few
vertices. Vertices beside a crease or a drop are marked `kink`: there the formula has no one normal.

Colour is one material per triangle (`pattern`), so a colour edge duplicates its vertices and a colour seam
cannot open into a crack. With `layers=2` a back skin follows the front at `thickness` behind it, along the
formula's normal, facing the other way in `back_colour`, and a hem along the border joins the two into one
closed shell: a sheet built as two skins, as cloth often is.
"""

from typing import NamedTuple

import numpy as np

from r3d.geometry import corner_edges, edge_uses

WAVES = ("flat", "sine", "zigzag", "sawtooth")
PATTERNS = ("solid", "stripes", "checker", "bands")
# Distinct colours (sRGB 0..255) for the patterns, in order.
PALETTE = ((200, 60, 40), (40, 90, 200), (230, 200, 50), (60, 170, 90), (170, 70, 180), (240, 140, 60))
# The back skin's colour: the shadowed far side of a cloth.
BACK_COLOUR = (20, 24, 32)
# Sheet.layer of the front skin, the back skin and the hem joining them.
FRONT, BACK, HEM = 0, 1, 2


def wave(shape, phase):
    """The wave `shape`'s height (-1 to 1) and slope per unit of phase at `phase`, one cycle per unit, every
    shape starting at 0 and rising: sine, zigzag (a triangle wave in step with the sine) and sawtooth (rising
    from -1 to 1 over a cycle centred on 0, then dropping)."""
    phase = np.asarray(phase, dtype=float)
    if shape == "flat":
        return np.ones_like(phase), np.zeros_like(phase)
    if shape == "sine":
        return np.sin(2 * np.pi * phase), 2 * np.pi * np.cos(2 * np.pi * phase)
    if shape == "zigzag":
        cycle = np.mod(phase + 0.25, 1.0)
        return 1.0 - 4.0 * np.abs(cycle - 0.5), np.where(cycle < 0.5, 4.0, -4.0)
    if shape == "sawtooth":
        return 2.0 * np.mod(phase + 0.5, 1.0) - 1.0, np.full_like(phase, 2.0)
    raise ValueError(f"wave shape {shape!r} is not one of {', '.join(WAVES)}")


def piece(shape, phase):
    """Which straight piece of a zigzag or sawtooth `phase` falls on; a sine or flat sheet is one piece."""
    phase = np.asarray(phase, dtype=float)
    if shape == "zigzag":
        return np.floor(2.0 * (phase + 0.25)).astype(np.int64)
    if shape == "sawtooth":
        return np.floor(phase + 0.5).astype(np.int64)
    return np.zeros(phase.shape, dtype=np.int64)


class Surface(NamedTuple):
    """The height field a sheet samples, in model units: `size` (width, height) around `centre`, waving about
    the plane `z`."""
    shape: str = "flat"
    amplitude: float = 0.0
    frequency: tuple = (1.0, 0.0)
    phase: tuple = (0.0, 0.0)
    size: tuple = (1.0, 1.0)
    centre: tuple = (0.0, 0.0)
    z: float = 0.0

    def fractions(self, x, y):
        """u and v, 0 to 1 across the sheet, at x and y."""
        return ((np.asarray(x, dtype=float) - self.centre[0]) / self.size[0] + 0.5,
                (np.asarray(y, dtype=float) - self.centre[1]) / self.size[1] + 0.5)

    def factors(self, t, axis):
        """One axis's factor of the height at fraction t, and its slope per unit of t."""
        if self.frequency[axis] == 0:
            return np.ones_like(t), np.zeros_like(t)
        value, slope = wave(self.shape, self.frequency[axis] * t + self.phase[axis])
        return value, slope * self.frequency[axis]

    def height(self, x, y):
        u, v = self.fractions(x, y)
        return self.z + self.amplitude * self.factors(u, 0)[0] * self.factors(v, 1)[0]

    def normal(self, x, y):
        """The unit normal facing +z at x and y, from the slope of the piece the point lies on."""
        u, v = self.fractions(x, y)
        (wu, su), (wv, sv) = self.factors(u, 0), self.factors(v, 1)
        normal = np.stack([-self.amplitude * su * wv / self.size[0], -self.amplitude * wu * sv / self.size[1],
                           np.ones_like(u)], axis=-1)
        return normal / np.linalg.norm(normal, axis=-1, keepdims=True)

    def piece(self, x, y):
        """Which straight piece x and y fall on, one number for both axes: points on different pieces have a
        crease or a drop between them."""
        u, v = self.fractions(x, y)
        pu = piece(self.shape, self.frequency[0] * u + self.phase[0]) if self.frequency[0] else np.zeros(np.shape(u), np.int64)
        pv = piece(self.shape, self.frequency[1] * v + self.phase[1]) if self.frequency[1] else np.zeros(np.shape(v), np.int64)
        return pu * (2 * int(np.ceil(max(self.frequency[1], 1))) + 4) + pv


class Sheet(NamedTuple):
    """positions (V, 3), rgb (V, 3) 0..255, tris (T, 3), material (T,) into colours, normals (V, 3) the
    formula's unit normal on the side the vertex's layer faces, kink (V,) where the formula has no one normal,
    layer (T,) FRONT, BACK or HEM, colours the material colours, and the surface it samples; a hem's normals
    are its back skin's."""
    positions: np.ndarray
    rgb: np.ndarray
    tris: np.ndarray
    material: np.ndarray
    normals: np.ndarray
    kink: np.ndarray
    layer: np.ndarray
    colours: tuple
    surface: Surface


def material_of(pattern, cells, colours, u, v, height):
    """Each triangle's material from its centre's fractions u, v across the sheet and its height (-1 to 1)."""
    count = len(colours)
    if pattern == "solid":
        return np.zeros(len(u), dtype=np.int64)
    column = np.minimum((u * cells).astype(np.int64), cells - 1)
    row = np.minimum((v * cells).astype(np.int64), cells - 1)
    if pattern == "stripes":
        return column % count
    if pattern == "checker":
        return (column + row) % count
    if pattern == "bands":
        return np.clip(((height + 1.0) / 2.0 * count).astype(np.int64), 0, count - 1)
    raise ValueError(f"pattern {pattern!r} is not one of {', '.join(PATTERNS)}")


def hem(front, count):
    """The strip joining each border edge of the `front` skin's triangles to the same edge of the back skin,
    whose points follow the front's `count` points: facing out, so the two skins close into one shell."""
    _edges, slot, uses = edge_uses(front)
    p, q = corner_edges(front)[uses[slot] == 1].T
    return np.concatenate([np.stack([q, p, p + count], 1), np.stack([q, p + count, q + count], 1)])


def sheet(count, size=1.0, centre=(0.0, 0.0), shape="flat", amplitude=0.0, frequency=(1.0, 0.0), phase=(0.0, 0.0),
          pattern="solid", cells=2, colours=PALETTE, layers=1, thickness=0.0, back_colour=BACK_COLOUR, z=0.0):
    """The sheet described in the module docstring. `size` is a number or (width, height), `cells` the stripes
    or checker squares across it, `z` the plane the sheet waves about."""
    if shape not in WAVES:
        raise ValueError(f"wave shape {shape!r} is not one of {', '.join(WAVES)}")
    if layers not in (1, 2):
        raise ValueError("a sheet has one layer or two")
    width, height = (size, size) if np.isscalar(size) else size
    surface = Surface(shape, amplitude, tuple(frequency), tuple(phase), (width, height), tuple(centre), z)
    # The corners from linspace, so a grid of unit cells lands on whole numbers exactly.
    xs, ys = np.meshgrid(np.linspace(centre[0] - width / 2, centre[0] + width / 2, count + 1),
                         np.linspace(centre[1] - height / 2, centre[1] + height / 2, count + 1))
    x, y = xs.ravel(), ys.ravel()
    points = np.stack([x, y, surface.height(x, y)], axis=-1)
    # Beside a crease the formula has no one normal: a kink vertex takes the mean of its quads' secant normals.
    half = 0.5 / count
    du, dv = half * width, half * height
    sides = [(x + sx * du, y + sy * dv) for sx in (-1, 1) for sy in (-1, 1)]
    kink = np.zeros(len(x), dtype=bool)
    for sx, sy in sides:
        kink |= surface.piece(sx, sy) != surface.piece(*sides[0])
    secant = np.stack([-(surface.height(x + du, y) - surface.height(x - du, y)) / (2 * du),
                       -(surface.height(x, y + dv) - surface.height(x, y - dv)) / (2 * dv), np.ones_like(x)], axis=-1)
    secant /= np.linalg.norm(secant, axis=1, keepdims=True)
    normal = np.where(kink[:, None] & (shape != "sawtooth"), secant, surface.normal(x, y))

    index = np.arange(len(points)).reshape(count + 1, count + 1)
    a, b = index[:-1, :-1].ravel(), index[:-1, 1:].ravel()
    c, d = index[1:, 1:].ravel(), index[1:, :-1].ravel()
    # Every quad's first triangle, then every quad's second.
    point_tris = np.concatenate([np.stack([a, b, c], 1), np.stack([a, c, d], 1)])
    u, v = surface.fractions(*points[point_tris].mean(axis=1)[:, :2].T)
    level = (points[point_tris, 2].mean(axis=1) - z) / amplitude if amplitude else np.zeros(len(point_tris))
    material = material_of(pattern, cells, colours, u, v, level)
    layer = np.full(len(point_tris), FRONT, dtype=np.int64)
    point_normals, point_kink = normal, kink
    palette = tuple(tuple(c) for c in colours)
    if layers == 2:
        front = point_tris
        joined = hem(front, len(points))
        point_tris = np.concatenate([front, front[:, ::-1] + len(points), joined])
        points = np.concatenate([points, points - thickness * normal])
        point_normals = np.concatenate([normal, -normal])
        point_kink = np.concatenate([kink, kink])
        material = np.concatenate([material, np.full(len(front) + len(joined), len(palette))])
        layer = np.concatenate([layer, np.full(len(front), BACK), np.full(len(joined), HEM)])
        palette += (tuple(back_colour),)

    # One vertex per (position, material), in position order: a colour edge duplicates its vertices.
    keys = point_tris * len(palette) + material[:, None]
    unique, vertex_of = np.unique(keys.ravel(), return_inverse=True)
    vertex_point, vertex_material = unique // len(palette), unique % len(palette)
    return Sheet(points[vertex_point], np.asarray(palette, dtype=float)[vertex_material], vertex_of.reshape(-1, 3),
                 material, point_normals[vertex_point], point_kink[vertex_point], layer, palette, surface)


def material_name(index):
    return f"colour_{index}"


def write_source(sheet, directory, name):
    """`sheet` as `directory/name.obj` and its `.mtl`, one material per colour with Kd its colour (0..1): a
    source mesh_import reads. Returns the .obj path."""
    import pathlib

    from r3d.obj import write_obj

    directory = pathlib.Path(directory)
    # The OBJ indexes positions only, so a colour edge's duplicated vertices are written once each.
    positions, point_of = np.unique(sheet.positions, axis=0, return_inverse=True)
    point_tris = point_of.reshape(-1)[sheet.tris]
    groups = [(name, material_name(m), point_tris[sheet.material == m]) for m in range(len(sheet.colours))
              if (sheet.material == m).any()]
    write_obj(directory / f"{name}.obj", f"{name}.mtl", positions, groups, position_decimals=6)
    lines = []
    for m, colour in enumerate(sheet.colours):
        r, g, b = (channel / 255.0 for channel in colour)
        lines += [f"newmtl {material_name(m)}", f"Kd {r:.6f} {g:.6f} {b:.6f}", ""]
    (directory / f"{name}.mtl").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return directory / f"{name}.obj"


def write_start(sheet, directory, name, position_scale=64, double=None):
    """`sheet` as `directory/name.bundle` through write_lit_mesh, coloured per vertex; `double` (T,) marks
    double-sided triangles (none by default). Returns the bundle path."""
    import pathlib

    from r3d.lit_mesh import write_lit_mesh

    double = np.zeros(len(sheet.tris), dtype=int) if double is None else np.asarray(double, dtype=int)
    write_lit_mesh(directory, name, sheet.positions, sheet.rgb, sheet.tris, double, position_scale=position_scale)
    return pathlib.Path(directory) / f"{name}.bundle"
