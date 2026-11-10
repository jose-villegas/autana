#!/usr/bin/env python3
"""Replay runtime cluster LOD over a fitted renderer's camera path, host only.

    lod_replay.py SCENE.scene.toml --object NAME --out DIR
        [--bundle BUNDLE] [--budgets N,...] [--every-ms N] [--size W H] [--segment T0 T1] [--drops NAME[=PERMILLE] [NAME[=PERMILLE] ...]]

Builds the cluster levels (levels.py) of the locked fit's rest mesh, welded
and position-only, and of each copy shape, which every copy of it shares.
Then, at every pose of the path, it counts what pick.py draws:

- the finest level in view, and what fixed tolerances draw;
- per budget, the tolerance the run-time histogram picks against the exact
  one, and the triangles each draws;
- the terminal share: drawn triangles whose group cannot be simplified any
  further, so no budget can take them away.

Culling is by cluster sphere against the frustum, with no occlusion. Writes
poses.csv, summary.md and lod.png to --out, which must be outside the
repository. --segment marks a stretch of the path (seconds) for its own row
in the summary and a band in the figure.
"""

import argparse
import csv
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from docs import plot_style  # noqa: E402
from r3d import mesh_bundle, lod_asset  # noqa: E402
from r3d.appearance_simplify import projection  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.lit_mesh import read_lit_mesh  # noqa: E402
from r3d.lod import pick  # noqa: E402
from r3d.geometry import weld_keeping  # noqa: E402
from r3d.lod.levels import build_levels  # noqa: E402
from r3d.poses import focal_px, sample_camera_path  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[4]
# Tolerances the summary reports without a budget, in pixels.
FIXED_TAUS_PX = (1.0, 2.0, 4.0)
DEFAULT_EVERY_MS = 100
# Default budgets: these shares of the path's most finest-level triangles in view.
DEFAULT_BUDGET_SHARES = (0.5, 0.75)


def mesh_levels(blob, levels=None):
    """The levels of an LMSH entry, in its model units, at its own median cluster size."""
    from r3d.lit_mesh import finest_triangles
    mesh = read_lit_mesh(blob, levels)
    mesh.pos, _, mesh.tris, _, _ = finest_triangles(mesh)
    ticks, triangles = weld_keeping(mesh.pos, mesh.tris, tolerance=1)
    return build_levels(ticks / float(mesh.position_scale), triangles, int(np.median([c[3] for c in mesh.clusters])))


def placements(bundle, rest_name, matrix, position):
    """Every placed mesh of a fit bundle; each copy shape's levels are built once."""
    entries, rows = mesh_bundle.decode(pathlib.Path(bundle).read_bytes())
    data = {name: blob for name, _kind, blob in entries}
    shapes = {name: mesh_levels(data[name], data.get(lod_asset.name_for_mesh(name))) for name in [rest_name, *dict.fromkeys(row["mesh"] for row in rows)]}
    return placed_meshes(shapes, rows, rest_name, matrix, position)


def placed_meshes(shapes, rows, rest_name, matrix, position):
    """The rest, then each copy row's shape under the row's transform; all under the renderer's
    (`matrix`, `position`), applied after the row's."""
    placed = [pick.Placed(shapes[rest_name], matrix, position, (rest_name,))]
    for row in rows:
        local = np.asarray(row["matrix"], dtype=np.float64)
        placed.append(pick.Placed(shapes[row["mesh"]], matrix @ local, matrix @ np.asarray(row["position"]) + position,
                                  tuple(row[key] for key in ("mesh", "name", "first", "source") if row.get(key))))
    return placed


def drop_indices(placed, names):
    """Placement indices selected by bundle mesh, copy or source shape name."""
    requested = {}
    for entry in names:
        name, separator, value = entry.partition("=")
        permille = int(value) if separator else pick.DROP_PERMILLE_MAX
        if not 0 <= permille <= pick.DROP_PERMILLE_MAX:
            raise ValueError("drop permille outside placement range")
        requested[name] = permille
    unknown = set(requested).difference(name for placement in placed for name in placement.names)
    if unknown:
        raise ValueError("unknown --drops names: " + ", ".join(sorted(unknown)))
    return {i: requested[name] for i, placement in enumerate(placed)
            for name in requested if name in placement.names}


def terminal_triangles(v, tau):
    return int(v.triangles[pick.drawn(v, tau) & np.isinf(v.group_px)].sum())


def pose_row(v, time_ms, budgets):
    finest = int(v.triangles[v.finest].sum())
    row = dict(time_ms=time_ms, finest=finest, coarsest=pick.triangles_at(v, np.inf))
    for tau in FIXED_TAUS_PX:
        row[f"at_{tau:g}px"] = pick.triangles_at(v, tau)
    for budget in budgets:
        exact, histogram = pick.tau_exact(v, budget), pick.tau_from_histogram(v, budget)
        row[f"b{budget}_tau_exact"] = exact
        row[f"b{budget}_tau_histogram"] = histogram
        row[f"b{budget}_exact"] = pick.triangles_at(v, exact)
        row[f"b{budget}_histogram"] = pick.triangles_at(v, histogram)
        row[f"b{budget}_terminal"] = terminal_triangles(v, histogram)
    if v.drop_px:
        for tau in FIXED_TAUS_PX:
            row[f"at_{tau:g}px_dropped"] = int(np.count_nonzero(
                (np.asarray(v.drop_px) <= tau) & np.isfinite(v.drop_px)))
        for budget in budgets:
            for method in ("exact", "histogram"):
                row[f"b{budget}_{method}_dropped"] = int(np.count_nonzero(
                    (np.asarray(v.drop_px) <= row[f"b{budget}_tau_{method}"]) & np.isfinite(v.drop_px)))
    return row


def summary_rows(rows, budgets):
    """(label, value) per measure, over `rows`."""
    finest = np.array([r["finest"] for r in rows], dtype=float)
    seen = finest > 0
    out = [("poses", f"{len(rows)}"), ("finest in view, mean / max", f"{finest.mean():.0f} / {finest.max():.0f}"),
           ("coarsest in view / finest, mean", f"{np.mean([r['coarsest'] for r in rows]) / max(finest.mean(), 1):.1%}")]
    for tau in FIXED_TAUS_PX:
        drawn = np.array([r[f"at_{tau:g}px"] for r in rows], dtype=float)
        out.append((f"saved at {tau:g} px, mean / p95 pose", f"{1 - drawn.sum() / max(finest.sum(), 1):.1%} / "
                    f"{np.percentile(1 - drawn[seen] / finest[seen], 95) if seen.any() else 0:.1%}"))
    for budget in budgets:
        drawn = np.array([r[f"b{budget}_histogram"] for r in rows], dtype=float)
        exact = np.array([r[f"b{budget}_exact"] for r in rows], dtype=float)
        tau = np.array([r[f"b{budget}_tau_exact"] for r in rows], dtype=float)
        # Poses the budget binds and the histogram can steer to: over it at the finest level, within the ceiling.
        steered = (finest > budget) & (tau <= pick.TAU_CEILING_PX)
        unspent = (exact[steered] - drawn[steered]) / budget
        bound_tau = tau[finest > budget]
        terminal = np.array([r[f"b{budget}_terminal"] for r in rows], dtype=float)
        out += [(f"budget {budget}: poses over it at the finest level", f"{(finest > budget).mean():.1%}"),
                (f"budget {budget}: exact tau on those, median / p95 px",
                 f"{np.median(bound_tau):.2g} / {np.percentile(bound_tau, 95):.2g}" if len(bound_tau) else "-"),
                (f"budget {budget}: poses past the {pick.TAU_CEILING_PX:g} px ceiling", f"{np.sum(tau > pick.TAU_CEILING_PX)}"),
                (f"budget {budget}: histogram short of exact, mean / max",
                 f"{unspent.mean():.1%} / {unspent.max():.1%}" if steered.any() else "-"),
                (f"budget {budget}: terminal share of drawn, mean / max",
                 f"{terminal.sum() / max(drawn.sum(), 1):.1%} / {np.max(terminal / np.maximum(drawn, 1)):.1%}")]
    for key in rows[0]:
        if key.endswith("_dropped"):
            counts = np.array([row[key] for row in rows])
            out.append((f"{key}: copies per frame, mean / max", f"{counts.mean():.1f} / {counts.max()}"))
    return out


def in_segment(rows, segment):
    """The rows from segment[0] to segment[1] seconds, both included."""
    return [r for r in rows if segment[0] * 1000 <= r["time_ms"] <= segment[1] * 1000]


def write(out, rows, budgets, segment, title):
    with (out / "poses.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    groups = [("path", rows)]
    if segment:
        groups.append((f"{segment[0]:g}-{segment[1]:g} s", in_segment(rows, segment)))
    table = [summary_rows(g, budgets) for _, g in groups]
    lines = [f"# {title}", "", "| Measure | " + " | ".join(name for name, _ in groups) + " |",
             "|---|" + "---:|" * len(groups)]
    lines += [f"| {label} | " + " | ".join(t[i][1] for t in table) + " |" for i, (label, _) in enumerate(table[0])]
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    fig, ax = plot_style.subplots(1, 1, plot_style.FIGURE_SIZE)
    seconds = [r["time_ms"] / 1000 for r in rows]
    ax.plot(seconds, [r["finest"] for r in rows], label="Finest level in view")
    for tau in FIXED_TAUS_PX:
        ax.plot(seconds, [r[f"at_{tau:g}px"] for r in rows], label=f"At {tau:g} px")
    for budget in budgets:
        ax.plot(seconds, [r[f"b{budget}_histogram"] for r in rows], label=f"Budget {budget}, histogram tau")
        ax.axhline(budget, linestyle=":", color="0.5")
    if segment:
        ax.axvspan(*segment, color="0.9")
    ax.set(xlabel="Path time (s)", ylabel="Triangles in view", ylim=(0, None))
    plot_style.save_plot(fig, ax, out / "lod.png", title)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("scene", type=pathlib.Path)
    parser.add_argument("--object", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--bundle", type=pathlib.Path)
    parser.add_argument("--budgets", type=lambda text: [int(n) for n in text.split(",")])
    parser.add_argument("--every-ms", type=int, default=DEFAULT_EVERY_MS)
    parser.add_argument("--size", type=int, nargs=2)
    parser.add_argument("--segment", type=float, nargs=2)
    parser.add_argument("--drops", nargs="+", default=[], help="placed mesh or carved shape names that may drop")
    args = parser.parse_args(argv)
    if args.every_ms <= 0:
        parser.error("--every-ms must be positive")
    out = args.out.resolve()
    if out.is_relative_to(REPO):
        parser.error("--out must be outside the repository")
    scene = load_scene(args.scene)
    job = next((j for j in scene.renderers if j.object.name == args.object), None)
    if job is None or job.renderer.fit is None:
        parser.error("--object must name a fitted mesh renderer")
    if scene.camera is None or scene.camera.component.path is None:
        parser.error("scene needs a camera path")
    camera = scene.camera.component
    width, height, lens, near, poses = sample_camera_path(camera.path.animation, camera.path.node, args.every_ms,
                                                          *(args.size or job.renderer.fit.size),
                                                          camera.half_fov_short_tan, camera.near_z)
    if args.bundle is None:
        from bake import bake

        bundle = bake.fetch(bake.locked_fit(args.scene, args.object))
    else:
        bundle = args.bundle
    placed = placements(bundle, job.asset_name, np.asarray(job.object.matrix, dtype=np.float64),
                           np.asarray(job.object.position, dtype=np.float64))
    placed[0] = placed[0]._replace(names=(*placed[0].names, job.object.name))
    try:
        drops = drop_indices(placed, args.drops)
    except ValueError as error:
        parser.error(str(error))
    focal = focal_px(width, height, lens)
    views = [pick.view(placed, pose[:3], projection(pose[:3], pose[3:], width, height, lens, near),
                       focal, near, drops, height=height)
             for pose in poses]
    budgets = args.budgets or sorted({int(share * max(int(v.triangles[v.finest].sum()) for v in views))
                                      for share in DEFAULT_BUDGET_SHARES})
    rows = [pose_row(v, i * args.every_ms, budgets) for i, v in enumerate(views)]
    if args.segment and not in_segment(rows, args.segment):
        parser.error("--segment holds no pose of the path")
    out.mkdir(parents=True, exist_ok=True)
    write(out, rows, budgets, args.segment, f"{args.scene.name.split('.')[0]} {args.object}: runtime LOD replay")
    print((out / "summary.md").read_text())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
