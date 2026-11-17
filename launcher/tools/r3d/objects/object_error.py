"""Held-out error per source object part of a fitted scene renderer, ranked by its share of the path's error.

objects_report.py --held-out drives it. Parts are the source objects' connected
pieces (welded positions within one object). Each pixel of the
held-out frames counts toward the part the reference shows there, traced with
the reference's own primary rays at the render's size; the sky counts as its
own row. The error is the held-out score's CIE76 dE (render_compare), so the
mean over every part's pixels is the score; held_out_error fails when not.

    object_error.py DATA --figure PNG --table MD

remakes a page's figure and tables from the rows objects_report.py --docs
keeps: DATA.csv, every part, and DATA.json, the totals.
"""

import argparse
import csv
import json
import os
import pathlib
import sys
from typing import NamedTuple

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "render"))

from docs import plot_style  # noqa: E402
from r3d.geometry import face_normals, majority  # noqa: E402

SKY = "(sky)"
TOP_ROWS = 20
# The figure numbers this many rows of each table by rank; the rest stay faint dots.
FIGURE_LABELS = 8
FIGURE_INCHES = (plot_style.FIGURE_SIZE[0], 3.0)
# Shares and dE76 keep this many significant digits in error.csv.
CSV_DIGITS = 6
# The held-out score prints four decimals; the mean over the parts' pixels must round to it.
SCORE_TOLERANCE = 1e-4
# A part is planar when this share of its source area faces within PLANAR_DEGREES of its mean normal: a gently
# curved surface counts; a few steep edge triangles do not undo it.
PLANAR_DEGREES = 5.0
PLANAR_AREA_SHARE = 0.99
# A drawn triangle is a sheet when its area exceeds this many times the median drawn triangle of its part and its
# normal stands more than SHEET_DEGREES off the nearest source triangle's.
SHEET_GROWTH = 4.0
SHEET_DEGREES = 30.0
# A row's fields in error.csv's column order, each with the type read turns its text back into.
FIELDS = dict(object=str, source_object=str, material=str, pixel_share=float, error_share=float,
              mean_delta_e=float, source_triangles=int, source_share=float, drawn_triangles=int, drawn_share=float,
              shape=int, copies=int, parts_of_object=int, welded=lambda text: text == "True",
              segment=lambda text: text == "True", planar=lambda text: text == "True", sheet_triangles=int)


class Parts(NamedTuple):
    """Per part: its name, its source object's name, how many parts that object has, whether it shares a welded
    position with another object, its most common material, its source and drawn triangle counts, whether it is
    planar and how many of its drawn triangles are sheets."""
    names: list
    objects: list
    parts_of_object: np.ndarray
    shared: np.ndarray
    materials: list
    source_triangles: np.ndarray
    drawn_triangles: np.ndarray
    planar: np.ndarray
    sheet_triangles: np.ndarray


def part_labels(p, tri_v, tri_o):
    """Per triangle, its connected part: triangles of one object that share a welded position. Parts are numbered
    by object, then by first triangle. Also per part, whether it shares a welded position with another object."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    from r3d.geometry import weld_keeping

    _, welded = weld_keeping(p, tri_v)
    nodes, node = np.unique(np.column_stack([np.repeat(tri_o, 3), welded.ravel()]), axis=0, return_inverse=True)
    node = node.reshape(-1, 3)
    graph = coo_matrix((np.ones(2 * len(node)), (np.r_[node[:, 0], node[:, 0]], np.r_[node[:, 1], node[:, 2]])),
                       shape=(len(nodes), len(nodes)))
    _, component = connected_components(graph, directed=False)
    raw = component[node[:, 0]]
    _, first = np.unique(raw, return_index=True)
    order = np.lexsort((first, tri_o[first]))
    rank = np.empty(len(order), dtype=np.int64)
    rank[order] = np.arange(len(order))
    labels = rank[np.unique(raw, return_inverse=True)[1]]
    owners = np.unique(nodes[:, 1], return_counts=True)
    shared_positions = owners[0][owners[1] > 1]
    shared = np.zeros(len(order), dtype=bool)
    shared[np.unique(labels[np.isin(welded, shared_positions).any(axis=1)])] = True
    return labels, shared


def part_names(labels, tri_o, object_names):
    """`object` for an object of one part, else `object#k`, k counting its parts from 0; also per part its
    object's index and part count."""
    first = np.unique(labels, return_index=True)[1]
    owner = tri_o[first]
    counts = np.bincount(owner, minlength=len(object_names))
    names, seen = [], {}
    for o in owner:
        k = seen.get(o, 0)
        seen[o] = k + 1
        names.append(object_names[o] if counts[o] == 1 else f"{object_names[o]}#{k}")
    return names, owner, counts[owner]


def main_materials(labels, tri_m, material_names, count):
    """Per part, the material most of its triangles use."""
    return [material_names[m] for m in majority(labels, tri_m, count)]


def nearest_source(source_p, tri_v, drawn_p, drawn_tris):
    """Per drawn triangle, the source triangle nearest it by centre: the part the fitted mesh spent it on."""
    from scipy.spatial import cKDTree

    return cKDTree(source_p[tri_v].mean(axis=1)).query(drawn_p[drawn_tris].mean(axis=1))[1]


def planar_parts(source_p, tri_v, labels, count):
    """Per part, whether PLANAR_AREA_SHARE of its source area faces within PLANAR_DEGREES of its area-weighted
    mean normal."""
    unit, area = face_normals(source_p, tri_v)
    mean = np.stack([np.bincount(labels, unit[:, axis] * area, minlength=count) for axis in range(3)], axis=1)
    mean /= np.maximum(np.linalg.norm(mean, axis=1, keepdims=True), 1e-30)
    facing = (unit * mean[labels]).sum(axis=1) >= np.cos(np.radians(PLANAR_DEGREES))
    return np.bincount(labels, area * facing, minlength=count) >= PLANAR_AREA_SHARE * np.bincount(
        labels, area, minlength=count)


def sheet_triangles(source_p, tri_v, drawn_p, drawn_tris, near, labels):
    """Per drawn triangle, whether its area exceeds SHEET_GROWTH times the median drawn triangle of its part (the
    part of its nearest source triangle, `labels[near]`) and its normal is turned more than SHEET_DEGREES from that
    source triangle's, either side."""
    unit, area = face_normals(drawn_p, drawn_tris)
    source_unit, _ = face_normals(source_p, tri_v[near])
    part = labels[near]
    median = np.zeros(part.max() + 1 if len(part) else 0)
    for label in np.unique(part):
        median[label] = np.median(area[part == label])
    turned = np.abs((unit * source_unit).sum(axis=1)) < np.cos(np.radians(SHEET_DEGREES))
    return (area > SHEET_GROWTH * median[part]) & turned


def accumulate(frames, reference_dir, scale, ids_for):
    """Per held-out frame, (part id of each render pixel, its dE76); `ids_for(index, width, height)` is the part
    each render pixel's reference shows, -1 for the sky."""
    from render.render_compare import delta_e76, reference_video

    totals = {}

    def sink(_fps, index, render, reference):
        totals[index] = (ids_for(index, render.width, render.height).ravel(), delta_e76(render, reference).ravel())

    reference_video(frames, reference_dir, scale=scale, sink=sink)
    return totals


def rows_for(parts, shapes, totals):
    """One row per part some held-out pixel shows, plus the sky, ranked by error share; the mean dE76; and the
    share of drawn triangles on parts no held-out pixel shows."""
    count = len(parts.names)
    pixels, error = np.zeros(count + 1), np.zeros(count + 1)
    for ids, frame_error in totals.values():
        slot = np.where(ids < 0, count, ids)
        pixels += np.bincount(slot, minlength=count + 1)
        error += np.bincount(slot, frame_error, minlength=count + 1)
    shape_of, copies = {}, {}
    for number, shape in enumerate(shapes):
        for instance in shape.instances:
            shape_of[instance.object_name] = number
            copies[instance.object_name] = len(shape.instances)
    total_pixels, total_error = pixels.sum(), error.sum()
    total_source, total_drawn = parts.source_triangles.sum(), parts.drawn_triangles.sum()
    # A perfect match has no error to share: every share is then 0.
    error_scale = total_error or 1.0
    seen = np.flatnonzero(pixels[:count])
    rows = [dict(object=SKY, source_object=SKY, material="", pixel_share=pixels[count] / total_pixels,
                 error_share=error[count] / error_scale, mean_delta_e=error[count] / max(pixels[count], 1),
                 source_triangles=0, source_share=0.0, drawn_triangles=0, drawn_share=0.0, shape=-1, copies=0,
                 parts_of_object=0, welded=False, segment=False, planar=False, sheet_triangles=0)]
    for part in seen:
        name = parts.names[part]
        rows.append(dict(
            object=name, source_object=parts.objects[part], material=parts.materials[part],
            pixel_share=pixels[part] / total_pixels, error_share=error[part] / error_scale,
            mean_delta_e=error[part] / pixels[part], source_triangles=int(parts.source_triangles[part]),
            source_share=parts.source_triangles[part] / total_source,
            drawn_triangles=int(parts.drawn_triangles[part]),
            drawn_share=parts.drawn_triangles[part] / total_drawn if total_drawn else 0.0,
            shape=shape_of[name], copies=copies[name], parts_of_object=int(parts.parts_of_object[part]),
            welded=bool(parts.shared[part]), segment=bool(parts.parts_of_object[part] == 1 and parts.shared[part]),
            planar=bool(parts.planar[part]), sheet_triangles=int(parts.sheet_triangles[part])))
    rows.sort(key=lambda row: row["error_share"], reverse=True)
    unseen = 1 - parts.drawn_triangles[seen].sum() / total_drawn if total_drawn else 0.0
    return rows, total_error / total_pixels, unseen


def grouped(rows, key, name, placed, keep=lambda row: True):
    """Parts summed by `key`, named by the first part's `name` field, `placed` the group's full count; ranked like
    the parts."""
    groups = {}
    for row in rows:
        if row["object"] == SKY or not keep(row):
            continue
        group = groups.setdefault(row[key], dict(name=row[name], material=row["material"], seen=0,
                                                 placed=row[placed], pixel_share=0.0, error_share=0.0,
                                                 source_triangles=0, drawn_triangles=0))
        group["seen"] += 1
        for field in ("pixel_share", "error_share", "source_triangles", "drawn_triangles"):
            group[field] += row[field]
    return sorted(groups.values(), key=lambda group: group["error_share"], reverse=True)


def object_rows(rows):
    """Source objects, every part summed."""
    return grouped(rows, "source_object", "source_object", "parts_of_object")


def shape_rows(rows):
    """Repeated shapes, every copy summed."""
    return grouped(rows, "shape", "object", "copies", lambda row: row["copies"] > 1)


def common_prefix(names):
    """The leading text every shown name shares, cut after its last underscore, which the tables and figure leave
    out."""
    prefix = os.path.commonprefix([name for name in names if name != SKY])
    return prefix[:prefix.rfind("_") + 1]


def cell(name, prefix=""):
    return name.removeprefix(prefix).replace("|", "\\|")


def flags(row):
    marks = []
    if row["copies"] > 1:
        marks.append(f"repeat ×{row['copies']}")
    if row["segment"]:
        marks.append("segment")
    elif row["welded"]:
        marks.append("welded")
    return ", ".join(marks)


def caption(text, names):
    """A table's dropdown summary, and its caption naming the prefix its shown names share and leave out; returns
    them and the prefix."""
    prefix = common_prefix(names)
    lines = [f"<details><summary>{text}</summary>", ""]
    return lines + ([f"Names leave out `{prefix}`.", ""] if prefix else []), prefix


def part_table(rows, top=TOP_ROWS):
    lines, prefix = caption(f"Top {top} object parts, ranked by error share", [row["object"] for row in rows[:top]])
    lines += ["| Rank | Object part | Material | Path pixels | Mean dE76 | Error share | Source triangles | "
              "Drawn triangles | Flags |", "|---:|---|---|---:|---:|---:|---:|---:|---|"]
    for rank, row in enumerate(rows[:top], 1):
        lines.append(f"| {rank} | {cell(row['object'], prefix)} | {cell(row['material'])} | {row['pixel_share']:.2%} | "
                     f"{row['mean_delta_e']:.1f} | {row['error_share']:.2%} | {row['source_share']:.2%} | "
                     f"{row['drawn_share']:.2%} | {flags(row)} |")
    return lines + ["", "</details>"]


def group_table(groups, score, text, label, count, top=TOP_ROWS):
    """A group's mean dE76 is the path mean scaled by its error share over its pixel share."""
    lines, prefix = caption(text, [group["name"] for group in groups[:top]])
    lines += [f"| Rank | {label} | Material | {count} | Path pixels | Mean dE76 | Error share | Source triangles | "
              "Drawn triangles |", "|---:|---|---|---:|---:|---:|---:|---:|---:|"]
    for rank, group in enumerate(groups[:top], 1):
        lines.append(f"| {rank} | {cell(group['name'], prefix)} | {cell(group['material'])} | {group['seen']} / "
                     f"{group['placed']} | {group['pixel_share']:.2%} | "
                     f"{score * group['error_share'] / group['pixel_share']:.1f} | {group['error_share']:.2%} | "
                     f"{group['source_triangles']} | {group['drawn_triangles']} |")
    return lines + ["", "</details>"]


def surface_text(rows):
    """How many drawn triangles of the parts in view sit on planar parts, and how many are sheets."""
    planar = sum(row["drawn_triangles"] for row in rows if row["planar"])
    sheets = sum(row["sheet_triangles"] for row in rows)
    return (f" {planar} drawn triangles sit on planar parts in view; {sheets} are sheets (over {SHEET_GROWTH:g} "
            f"times their part's median triangle, turned over {SHEET_DEGREES:g}° from the source).")


def report_body(rows, score, unseen):
    """The three ranked tables."""
    lines = [f"Mean dE76 over the held-out path {score:.3f}, over {len(rows) - 1} object parts in view. "
             f"{unseen:.1%} of the drawn triangles sit on parts no held-out pixel shows.{surface_text(rows)}", ""]
    lines += part_table(rows) + [""]
    lines += group_table(object_rows(rows), score, f"Top {TOP_ROWS} source objects, every part summed",
                         "Source object", "Parts seen / all") + [""]
    lines += group_table(shape_rows(rows), score, f"Top {TOP_ROWS} repeated shapes, every copy summed",
                         "Shape (first copy)",
                         "Copies seen / placed")
    return "\n".join(lines)


def figure(rows, path, title, top=FIGURE_LABELS):
    """Error share against path-pixel share for the parts, the source objects and the repeated shapes, one panel
    each: every row a faint dot, the `top` of each table numbered by its rank there. Above the dashed line a row's
    mean dE76 is over the path's."""
    panels = (("Object parts", rows),
              ("Source objects", object_rows(rows)), ("Repeated shapes", shape_rows(rows)))
    fig, axes = plot_style.subplots(1, len(panels), FIGURE_INCHES)
    for axis, (name, ranked) in zip(axes, panels):
        x = 100 * np.array([row["pixel_share"] for row in ranked])
        y = 100 * np.array([row["error_share"] for row in ranked])
        axis.scatter(x, y, s=4, color=plot_style.FAINT, linewidths=0)
        axis.scatter(x[:top], y[:top], s=14, color=plot_style.SLOTS[1], linewidths=0, zorder=3)
        reach = 1.1 * max(x.max(initial=0), y.max(initial=0)) or 1.0
        axis.plot([0, reach], [0, reach], linestyle="--", linewidth=0.8, color=plot_style.MUTED)
        axis.set(xlim=(0, reach), ylim=(0, reach))
        axis.set_xlabel("% of path pixels", fontsize=plot_style.LABEL_SIZE)
        plot_style.style_axes(axis, name, square=True)
    axes[0].set_ylabel("% of path error", fontsize=plot_style.LABEL_SIZE)
    fig.suptitle(title, fontsize=plot_style.TITLE_SIZE, color=plot_style.INK)
    for axis, (_, ranked) in zip(axes, panels):
        shown = ranked[:top]
        plot_style.place_labels(axis, [(100 * row["pixel_share"], 100 * row["error_share"]) for row in shown],
                                [str(rank) for rank in range(1, len(shown) + 1)])
    plot_style.save(fig, path)


def write(out, rows, score, unseen, title, keys):
    """error.csv (every part), error.json (totals, and the bake `keys` the rows were measured from), error.md (the
    ranked tables), error.png (the bar figure)."""
    with (out / "error.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(FIELDS), lineterminator="\n")
        writer.writeheader()
        writer.writerows({key: f"{value:.{CSV_DIGITS}g}" if isinstance(value, float) else value
                          for key, value in row.items()} for row in rows)
    (out / "error.json").write_text(json.dumps(dict(title=title, **keys, mean_delta_e=score,
                                                    parts_seen=len(rows) - 1, drawn_unseen_share=unseen),
                                               indent=2) + "\n")
    (out / "error.md").write_text(f"# {title}\n\n{report_body(rows, score, unseen)}\n\n![Error and pixel share](error.png)\n")
    figure(rows, out / "error.png", title)


def read(data):
    """The rows and totals write kept as DATA.csv and DATA.json."""
    with data.with_suffix(".csv").open(newline="") as stream:
        rows = [{key: FIELDS[key](value) for key, value in row.items()} for row in csv.DictReader(stream)]
    return rows, json.loads(data.with_suffix(".json").read_text())


def held_out_error(scene_path, scene, job, bundle, inputs, work):
    """Score `bundle` as `job` over `inputs`' held-out poses and split its error by source object part: rows_for's
    rows, mean dE76 and unseen drawn share. Fails when the parts' mean does not round to the held-out score's."""
    from r3d import fitted_variant as fv
    from r3d import mesh_bundle
    from r3d.appearance_simplify import start_mesh
    from r3d.bake_fidelity import build_host
    from r3d.fit.copies import joined, placed_copies
    from r3d.objects.instances import match_instances, source_objects
    from r3d.poses import read_poses
    from r3d.ray_query import RayQuery
    from r3d.reference_render import lit_source, primary_hits

    source = lit_source(job, scene, lit=False)
    p = np.asarray(source.p, dtype=np.float64)
    tri_v, tri_o, tri_m = np.asarray(source.tri_v), np.asarray(source.tri_o), np.asarray(source.tri_m)
    labels, shared = part_labels(p, tri_v, tri_o)
    count = int(labels.max()) + 1
    names, owner, parts_of_object = part_names(labels, tri_o, source.object_names)
    shapes = match_instances(source_objects(p, tri_v, tri_m, labels, names))
    source.intersector = RayQuery(p, tri_v)
    _, _, lens, _, poses = read_poses(inputs / "held_out.txt")

    def ids_for(index, width, height):
        _, _, _, rays, faces, *_ = primary_hits(source, job, poses[index], width, height, lens, 1)
        ids = np.full(width * height, -1, dtype=np.int64)
        ids[rays] = labels[faces]
        return ids

    entries, _rows = mesh_bundle.decode(bundle.read_bytes())
    path = work / f"{job.asset_name}.mesh"
    path.write_bytes(next(data for name, _kind, data in entries if name == job.asset_name))
    rest = start_mesh(path)
    fitted = joined(rest, placed_copies(bundle, rest.flat))
    drawn = fitted.points[fitted.vertex_point], fitted.tris
    near = nearest_source(p, tri_v, *drawn)
    sheets = sheet_triangles(p, tri_v, *drawn, near, labels)
    parts = Parts(names, [source.object_names[o] for o in owner], parts_of_object, shared,
                  main_materials(labels, tri_m, source.names, count), np.bincount(labels, minlength=count),
                  np.bincount(labels[near], minlength=count), planar_parts(p, tri_v, labels, count),
                  np.bincount(labels[near], sheets, minlength=count).astype(np.int64))
    host = build_host(fv.VIEWER, work / "host", scene_path)
    held_out_mean, _p95 = fv.held_out_score(job, bundle, work, host, scene_path, inputs=inputs)
    totals = accumulate(work / "score" / "frames.avi", inputs / "reference_held_out", fv.BOARD_SCALE, ids_for)
    rows, score, unseen = rows_for(parts, shapes, totals)
    if abs(score - held_out_mean) > SCORE_TOLERANCE:
        raise ValueError(f"the parts' mean dE76 {score:.4f} is not the held-out score's {held_out_mean:.4f}")
    return rows, score, unseen


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("data", type=pathlib.Path)
    parser.add_argument("--figure", type=pathlib.Path, required=True)
    parser.add_argument("--table", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    rows, totals = read(args.data)
    figure(rows, args.figure, totals["title"])
    args.table.write_text(report_body(rows, totals["mean_delta_e"], totals["drawn_unseen_share"]) + "\n",
                          encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
