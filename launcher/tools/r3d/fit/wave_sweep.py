"""How the appearance fit fares on folds: coarse wave sheets fitted to fine ones over amplitude x frequency, for
each wave shape, and what goes wrong measured against the formula the sheets sample.

    python launcher/tools/r3d/fit/wave_sweep.py --out DIR [--steps N] [--modes ...] [--shapes ...]
    python launcher/tools/r3d/fit/wave_sweep.py --plot DIR/wave_sweep.csv

For every (mode, shape, amplitude, frequency) the truth is a fine sheet (r3d/procedural/wave_sheet.py) drawn
by the fit's own renderer from views on a ring above it, which are the references; the start is the same
formula sampled coarsely and written as a start bundle; optimise() fits it. A mode is the sheet's build and
sidedness: one skin, or two skins closed at the hem (a cloth) drawn double-sided or culled.

Measures, per fitted mesh (`measure`):
- flipped: skin triangles whose face normal points against the formula's (crease triangles left out);
  flipped_start: triangles turned over against their start.
- normal_p50/p95/max: degrees between the fit's own vertex normals and the formula's at each skin point;
  points beside a crease or a drop are left out and their p95 is normal_kink_p95; with none left a
  measure is NaN, drawn n/a (kink).
- back_px: held-out pixels showing a triangle from behind, which only a double-sided triangle can.
- dark_px: held-out pixels near black (within DARK_DE76 of it) where the truth is not.
- holes: hole_pixels(), pixels showing nothing where the truth shows surface.

Writes DIR/wave_sweep.csv, its arguments as DIR/wave_sweep.json, and DIR/wave_sweep.<mode>.png drawn from the
CSV alone: one heat map per measure and shape, amplitude down, frequency across. Needs CUDA, PyTorch and
nvdiffrast.
"""

import argparse
import csv
import json
import pathlib
import sys
import tempfile
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from r3d import log  # noqa: E402
from r3d.geometry import face_normals  # noqa: E402
from r3d.procedural import wave_sheet  # noqa: E402

AMPLITUDES = (0.02, 0.05, 0.1, 0.2, 0.4)  # sheet widths
FREQUENCIES = (1, 2, 4, 8, 16)  # cycles across the sheet
SHAPES = ("sine", "zigzag", "sawtooth")
# mode: (skins, drawn double-sided)
MODES = {"one-skin": (1, False), "two-skin-double": (2, True), "two-skin-culled": (2, False)}
SIZE = 100.0  # the sheet's width, model units
POSITION_SCALE = 8  # ticks per unit, as the scenes bake
TRUTH_COUNT, START_COUNT = 64, 8  # quads a side
# The two skins' gap in sheet widths: a drape's measured gap over its width (3.27 cm over 280 cm).
THICKNESS = 0.012
PATTERN, CELLS = "checker", 4
VIEW_SIZE = (96, 96)
LENS, NEAR = 0.62, 1.0
TRAIN_VIEWS, HELD_OUT_VIEWS = 12, 4
VIEW_DISTANCE = 2.0  # sheet widths from its centre
VIEW_TILT_DEG = (0.0, 40.0)  # from straight above
CLEAR = (0.6, 0.75, 0.9)  # a sky, so a hole is not mistaken for black
DARK_DE76 = 15.0
FIT_DEFAULTS = dict(steps=300, batch=8, lr_position=2e-4, lr_colour=0.01, laplacian=10.0, normal_weight=1.0,
                    surface_weight=10.0, surface_margin=0.25)
MEASURES = ("flipped", "flipped_start", "normal_p50", "normal_p95", "normal_max", "normal_kink_p95", "back_px",
            "dark_px", "holes")
PLOTTED = ("flipped", "normal_p95", "back_px", "dark_px", "holes")
FIELDS = ("mode", "shape", "amplitude", "frequency") + MEASURES


def views(count, offset, size=VIEW_SIZE):
    """`count` (clip matrix, eye) pairs on a ring above a sheet of width SIZE at the origin, tilted from
    straight above through VIEW_TILT_DEG, `offset` turning the ring so held-out views fall between."""
    from r3d.appearance_simplify import projection

    out = []
    for k in range(count):
        turn = 2 * np.pi * (k + offset) / count
        tilt = np.radians(np.interp(k % 4, (0, 3), VIEW_TILT_DEG))
        eye = VIEW_DISTANCE * SIZE * np.array([np.sin(tilt) * np.cos(turn), np.sin(tilt) * np.sin(turn), np.cos(tilt)])
        out.append((projection(eye, -eye, size[0], size[1], LENS, NEAR), eye))
    return out


def built(mode, shape, amplitude, frequency, count):
    """The sheet a mode builds at `count` quads a side."""
    skins, _double = MODES[mode]
    return wave_sheet.sheet(count, size=SIZE, shape=shape, amplitude=amplitude * SIZE, frequency=(frequency, 0),
                            pattern=PATTERN, cells=CELLS, layers=skins, thickness=THICKNESS * SIZE)


def references(truth, double, pairs, device):
    """[(matrix, target image, target normals, eye)]: the truth drawn and its normals, at each view."""
    import torch

    from r3d.appearance_simplify import Renderer

    render = Renderer(truth.tris, np.full(len(truth.tris), double), np.arange(len(truth.positions)), VIEW_SIZE, device,
                      CLEAR)
    points = torch.as_tensor(truth.positions, dtype=torch.float32, device=device)
    colours = torch.as_tensor(truth.rgb / 255.0, dtype=torch.float32, device=device)
    out = []
    with torch.no_grad():
        for matrix, eye in pairs:
            m = torch.as_tensor(matrix, dtype=torch.float32, device=device)
            normal, _shown = render.normals(points, m, eye)
            out.append((matrix, render(points, colours, m).cpu().numpy(), normal.cpu().numpy(), eye))
    return out


def skins(start_normals):
    """Each start triangle's skin: +1 front (facing +z), -1 back, 0 a hem or anything edge-on."""
    return np.where(start_normals[:, 2] > 0.5, 1, np.where(start_normals[:, 2] < -0.5, -1, 0))


def shape_measures(surface, start_points, points, tris):
    """flipped, flipped_start and the normal angles of a fitted mesh's welded `points` over `tris`, whose
    `start_points` it was fitted from, against `surface`."""
    start = face_normals(start_points, tris)[0]
    fitted = face_normals(points, tris)[0]
    side = skins(start)
    centres = points[tris].mean(axis=1)
    formula = surface.normal(centres[:, 0], centres[:, 1]) * side[:, None]
    corners = start_points[tris]
    crease = (surface.piece(corners[..., 0], corners[..., 1]) != surface.piece(corners[:, :1, 0], corners[:, :1, 1])).any(1)
    skin = side != 0
    out = {"flipped": int(((fitted * formula).sum(1) < 0)[skin & ~crease].sum()),
           "flipped_start": int(((fitted * start).sum(1) < 0).sum())}
    # The fit's own vertex normals: area-weighted, around each welded point.
    area = np.cross(points[tris[:, 1]] - points[tris[:, 0]], points[tris[:, 2]] - points[tris[:, 0]])
    around = np.zeros_like(points)
    np.add.at(around, tris.reshape(-1), np.repeat(area, 3, axis=0))
    around /= np.maximum(np.linalg.norm(around, axis=1, keepdims=True), 1e-12)
    # A point takes the skin of the triangles around it; one beside two skins, or only hems, is mixed.
    corner, corner_side = tris.reshape(-1), np.repeat(side, 3)
    lowest, highest = np.full(len(points), 2), np.full(len(points), -2)
    np.minimum.at(lowest, corner, np.where(corner_side == 0, 2, corner_side))
    np.maximum.at(highest, corner, np.where(corner_side == 0, -2, corner_side))
    mixed, point_side = lowest != highest, np.where(lowest == highest, lowest, 0)
    point_crease = np.zeros(len(points), dtype=bool)
    np.logical_or.at(point_crease, corner, np.repeat(crease, 3))
    expected = surface.normal(points[:, 0], points[:, 1]) * point_side[:, None]
    degrees = np.degrees(np.arccos(np.clip((around * expected).sum(1), -1, 1)))
    plain, kinked = degrees[~mixed & ~point_crease], degrees[~mixed & point_crease]
    # NaN where no point is left to measure: every point of a start lies beside a crease once a wave's cycle
    # spans fewer than two of its quads.
    out.update(normal_p50=float(np.percentile(plain, 50)) if len(plain) else np.nan,
               normal_p95=float(np.percentile(plain, 95)) if len(plain) else np.nan,
               normal_max=float(plain.max()) if len(plain) else np.nan,
               normal_kink_p95=float(np.percentile(kinked, 95)) if len(kinked) else np.nan)
    return out


def picture_measures(mesh, held_out, device):
    """back_px, dark_px and holes of FitMesh `mesh` over the held-out references."""
    import torch

    from r3d.appearance_simplify import Renderer, delta_e76, hole_pixels

    render = Renderer(mesh.tris, mesh.double, mesh.vertex_point, VIEW_SIZE, device, CLEAR, mesh.flat)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device=device)
    colours = torch.as_tensor(mesh.rgb, dtype=torch.float32, device=device)
    corners = mesh.points[mesh.vertex_point][mesh.tris]
    normals = torch.as_tensor(np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), device=device)
    centres = torch.as_tensor(corners.mean(axis=1), device=device)
    black = torch.zeros(3, device=device)
    back = dark = 0
    with torch.no_grad():
        for matrix, target, _normal, eye in held_out:
            m = torch.as_tensor(matrix, dtype=torch.float32, device=device)
            ids = render.visible_ids(points, m).flip(0)
            shown = ids >= 0
            towards = torch.as_tensor(eye, device=device) - centres
            away = (normals * towards).sum(-1) < 0
            back += int((shown & away[ids.clamp_min(0)]).sum())
            image = render(points, colours, m)
            truth = torch.as_tensor(target, device=device)
            dark += int(((delta_e76(image, black.expand_as(image)) < DARK_DE76)
                         & (delta_e76(truth, black.expand_as(truth)) >= DARK_DE76)).sum())
    holes = hole_pixels(mesh, [(matrix, target) for matrix, target, *_ in held_out], VIEW_SIZE, CLEAR, device)
    return {"back_px": back, "dark_px": dark, "holes": holes}


def run_one(mode, shape, amplitude, frequency, fit, device="cuda"):
    """The measures of one fitted sheet."""
    from r3d.appearance_simplify import optimise, start_mesh

    _skins, double = MODES[mode]
    truth = built(mode, shape, amplitude, frequency, TRUTH_COUNT)
    coarse = built(mode, shape, amplitude, frequency, START_COUNT)
    train = references(truth, double, views(TRAIN_VIEWS, 0.0), device)
    held_out = references(truth, double, views(HELD_OUT_VIEWS, 0.5), device)
    with tempfile.TemporaryDirectory() as directory:
        path = wave_sheet.write_start(coarse, directory, "start", POSITION_SCALE,
                                      double=np.full(len(coarse.tris), int(double)))
        mesh = start_mesh(path)
    source = SimpleNamespace(p=truth.positions, tri_v=truth.tris)
    points, rgb, _history = optimise(mesh, train, VIEW_SIZE, report=0, device=device, clear=CLEAR, source=source, **fit)
    fitted = mesh._replace(points=points, rgb=rgb)
    row = {"mode": mode, "shape": shape, "amplitude": amplitude, "frequency": frequency}
    row.update(shape_measures(coarse.surface, mesh.points, points, mesh.vertex_point[mesh.tris]))
    row.update(picture_measures(fitted, held_out, device))
    return row


def plot(csv_path, out_dir):
    """One PNG per mode from the CSV: a heat map per plotted measure (rows) and shape (columns)."""
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
    from docs import plot_style

    with open(csv_path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    written = []
    for mode in dict.fromkeys(row["mode"] for row in rows):
        mine = [row for row in rows if row["mode"] == mode]
        shapes = list(dict.fromkeys(row["shape"] for row in mine))
        amplitudes = sorted({float(row["amplitude"]) for row in mine})
        frequencies = sorted({float(row["frequency"]) for row in mine})
        fig, axes = plot_style.subplots(len(PLOTTED), len(shapes), (3.0 * len(shapes), 2.6 * len(PLOTTED)), squeeze=False)
        for r, measure in enumerate(PLOTTED):
            for c, shape in enumerate(shapes):
                grid = np.full((len(amplitudes), len(frequencies)), np.nan)
                for row in mine:
                    if row["shape"] == shape:
                        grid[amplitudes.index(float(row["amplitude"])), frequencies.index(float(row["frequency"]))] = float(row[measure])
                axis = axes[r][c]
                axis.imshow(grid, cmap="viridis", origin="lower", aspect="auto")
                for (i, j), value in np.ndenumerate(grid):
                    text = "n/a (kink)" if np.isnan(value) else f"{value:.0f}" if measure != "normal_p95" else f"{value:.1f}"
                    axis.text(j, i, text, ha="center", va="center", fontsize=plot_style.LABEL_SIZE, color="white")
                axis.set_xticks(range(len(frequencies)), [f"{f:g}" for f in frequencies])
                axis.set_yticks(range(len(amplitudes)), [f"{a:g}" for a in amplitudes])
                axis.set_xlabel("frequency (cycles across)", fontsize=plot_style.LABEL_SIZE)
                axis.set_ylabel("amplitude (widths)", fontsize=plot_style.LABEL_SIZE)
                plot_style.style_axes(axis, f"{shape}: {measure}")
        fig.suptitle(f"wave sweep, {mode}", fontsize=plot_style.TITLE_SIZE)
        path = pathlib.Path(out_dir) / f"wave_sweep.{mode}.png"
        plot_style.save(fig, path)
        written.append(path)
    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", help="the directory the CSV, its arguments and the figures go to")
    parser.add_argument("--plot", help="redraw the figures from this CSV, beside it, fitting nothing")
    parser.add_argument("--modes", nargs="+", default=list(MODES), choices=list(MODES))
    parser.add_argument("--shapes", nargs="+", default=list(SHAPES), choices=list(SHAPES))
    parser.add_argument("--amplitudes", nargs="+", type=float, default=list(AMPLITUDES))
    parser.add_argument("--frequencies", nargs="+", type=float, default=list(FREQUENCIES))
    for name, value in FIT_DEFAULTS.items():
        parser.add_argument(f"--{name.replace('_', '-')}", type=type(value), default=value)
    args = parser.parse_args(argv)
    if args.plot:
        for path in plot(args.plot, pathlib.Path(args.plot).parent):
            print(path)
        return 0
    if not args.out:
        parser.error("--out or --plot is required")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "wave_sweep.json").write_text(json.dumps(vars(args), indent=1) + "\n", encoding="utf-8")
    fit = {name: getattr(args, name) for name in FIT_DEFAULTS}
    csv_path = out / "wave_sweep.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, FIELDS)
        writer.writeheader()
        for mode in args.modes:
            for shape in args.shapes:
                for amplitude in args.amplitudes:
                    for frequency in args.frequencies:
                        row = run_one(mode, shape, amplitude, frequency, fit)
                        writer.writerow(row)
                        f.flush()
                        log(" ".join(f"{key}={row[key]}" for key in FIELDS))
    for path in plot(csv_path, out):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
