"""Held-out error per source object part of a fitted scene renderer, ranked by its share of the path's error.

objects_report.py --held-out drives it. Objects are the source objects split
into connected parts (welded positions within one object). Each pixel of the
held-out frames counts toward the part the reference shows there, traced with
the reference's own primary rays at the render's size; the sky counts as its
own row. The error is the held-out score's CIE76 dE (render_compare), so the
rows sum to the score.
"""

import csv
import json
import os
import pathlib
import sys
from typing import NamedTuple

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "render"))

from docs import plot_style  # noqa: E402
from r3d.geometry import weld_keeping  # noqa: E402
from r3d.objects.instances import match_instances, source_objects  # noqa: E402
from r3d.poses import read_poses  # noqa: E402
from render.render_compare import delta_e76, reference_video  # noqa: E402

SKY = "(sky)"
TOP_ROWS = 20
FIGURE_ROWS = 30
# The bar figure grows by this many inches per part, past a fixed frame for its title and axis.
ROW_INCHES = 0.24
FRAME_INCHES = 1.2


class Parts(NamedTuple):
    """Per part: its name, its source object's name, how many parts that object has, whether it shares a welded
    position with another object, its most common material, and its source and drawn triangle counts."""
    names: list
    objects: list
    parts_of_object: np.ndarray
    shared: np.ndarray
    materials: list
    source_triangles: np.ndarray
    drawn_triangles: np.ndarray


def part_labels(p, tri_v, tri_o):
    """Per triangle, its connected part: triangles of one object that share a welded position. Parts are numbered
    by object, then by first triangle. Also per part, whether it shares a welded position with another object."""
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
    pairs, hits = np.unique(np.column_stack([labels, tri_m]), axis=0, return_counts=True)
    best = np.zeros(count, dtype=np.int64)
    most = np.zeros(count, dtype=np.int64)
    for (part, material), number in zip(pairs, hits):
        if number > most[part]:
            best[part], most[part] = material, number
    return [material_names[m] for m in best]


def drawn_parts(source_p, tri_v, labels, drawn, count):
    """Per part, the drawn triangles nearest its source triangles by centre: where the fitted mesh spent them."""
    _, near = cKDTree(source_p[tri_v].mean(axis=1)).query(drawn.mean(axis=1))
    return np.bincount(labels[near], minlength=count)


def accumulate(frames, reference_dir, scale, ids_for):
    """Per held-out frame, (part id of each render pixel, its dE76); `ids_for(index, width, height)` is the part
    each render pixel's reference shows, -1 for the sky."""
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
    seen = np.flatnonzero(pixels[:count])
    rows = [dict(object=SKY, source_object=SKY, material="", pixel_share=pixels[count] / total_pixels,
                 error_share=error[count] / total_error, mean_delta_e=error[count] / max(pixels[count], 1),
                 source_triangles=0, source_share=0.0, drawn_triangles=0, drawn_share=0.0, shape=-1, copies=0,
                 parts_of_object=0, welded=False, segment=False)]
    for part in seen:
        name = parts.names[part]
        rows.append(dict(
            object=name, source_object=parts.objects[part], material=parts.materials[part],
            pixel_share=pixels[part] / total_pixels, error_share=error[part] / total_error,
            mean_delta_e=error[part] / pixels[part], source_triangles=int(parts.source_triangles[part]),
            source_share=parts.source_triangles[part] / total_source,
            drawn_triangles=int(parts.drawn_triangles[part]),
            drawn_share=parts.drawn_triangles[part] / total_drawn if total_drawn else 0.0,
            shape=shape_of[name], copies=copies[name], parts_of_object=int(parts.parts_of_object[part]),
            welded=bool(parts.shared[part]), segment=bool(parts.parts_of_object[part] == 1 and parts.shared[part])))
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
    """A table's caption, naming the prefix its shown names share and leave out; returns it and the prefix."""
    prefix = common_prefix(names)
    return [text + (f" Names leave out `{prefix}`." if prefix else ""), ""], prefix


def part_table(rows, top=TOP_ROWS):
    lines, prefix = caption("Object parts, ranked by error share.", [row["object"] for row in rows[:top]])
    lines += ["| Rank | Object part | Material | Path pixels | Mean dE76 | Error share | Source triangles | "
              "Drawn triangles | Flags |", "|---:|---|---|---:|---:|---:|---:|---:|---|"]
    for rank, row in enumerate(rows[:top], 1):
        lines.append(f"| {rank} | {cell(row['object'], prefix)} | {cell(row['material'])} | {row['pixel_share']:.2%} | "
                     f"{row['mean_delta_e']:.1f} | {row['error_share']:.2%} | {row['source_share']:.2%} | "
                     f"{row['drawn_share']:.2%} | {flags(row)} |")
    return lines


def group_table(groups, score, text, label, count, top=TOP_ROWS):
    """A group's mean dE76 is the path mean scaled by its error share over its pixel share."""
    lines, prefix = caption(text, [group["name"] for group in groups[:top]])
    lines += [f"| {label} | Material | {count} | Path pixels | Mean dE76 | Error share | Source triangles | "
              "Drawn triangles |", "|---|---|---:|---:|---:|---:|---:|---:|"]
    for group in groups[:top]:
        lines.append(f"| {cell(group['name'], prefix)} | {cell(group['material'])} | {group['seen']} / "
                     f"{group['placed']} | {group['pixel_share']:.2%} | "
                     f"{score * group['error_share'] / group['pixel_share']:.1f} | {group['error_share']:.2%} | "
                     f"{group['source_triangles']} | {group['drawn_triangles']} |")
    return lines


def report_body(rows, score, unseen):
    """The three ranked tables."""
    lines = [f"Mean dE76 over the held-out path {score:.3f}, over {len(rows) - 1} object parts in view. "
             f"{unseen:.1%} of the drawn triangles sit on parts no held-out pixel shows.", ""]
    lines += part_table(rows) + [""]
    lines += group_table(object_rows(rows), score, "Source objects, every part summed.", "Source object",
                         "Parts seen / all") + [""]
    lines += group_table(shape_rows(rows), score, "Repeated shapes, every copy summed.", "Shape (first copy)",
                         "Copies seen / placed")
    return "\n".join(lines)


def figure(rows, path, title, top=FIGURE_ROWS):
    """The parts with the largest error share, each beside its share of path pixels, named without the prefix all
    their names share."""
    shown = rows[:top]
    prefix = common_prefix([row["object"] for row in shown])
    fig, ax = plot_style.subplots(1, 1, (plot_style.FIGURE_SIZE[0], ROW_INCHES * len(shown) + FRAME_INCHES))
    index = np.arange(len(shown))
    half = plot_style.BAR_HEIGHT / 2
    ax.barh(index - half / 2, [100 * row["error_share"] for row in shown], half, color=plot_style.SLOTS[1],
            label="Share of path error")
    ax.barh(index + half / 2, [100 * row["pixel_share"] for row in shown], half, color=plot_style.SLOTS[0],
            label="Share of path pixels")
    ax.set_yticks(index, [row["object"].removeprefix(prefix) for row in shown])
    ax.invert_yaxis()
    ax.set(xlabel="% of the held-out path", xlim=(0, None))
    ax.legend(loc="lower right", fontsize=plot_style.LABEL_SIZE)
    plot_style.style_axes(ax, title)
    plot_style.save(fig, path)


def write(out, rows, score, unseen, held_out_mean, title):
    """error.csv (every part), error.json (totals), error.md (the ranked tables), error.png (the bar figure);
    returns the tables' Markdown."""
    with (out / "error.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (out / "error.json").write_text(json.dumps(dict(mean_delta_e=score, held_out_mean_delta_e=held_out_mean,
                                                    parts_seen=len(rows) - 1, drawn_unseen_share=unseen),
                                               indent=2) + "\n")
    body = report_body(rows, score, unseen)
    (out / "error.md").write_text(f"# {title}\n\n{body}\n\n![Error and pixel share](error.png)\n")
    figure(rows, out / "error.png", title)
    return body


def held_out_error(scene_path, scene, job, bundle, inputs, work):
    """Score `bundle` as `job` over `inputs`' held-out poses and split its error by source object part: rows_for's
    rows, mean dE76 and unseen drawn share, and the held-out score's own mean."""
    from r3d import fitted_variant as fv
    from r3d import mesh_bundle
    from r3d.bake_fidelity import build_host
    from r3d.fit.copies import placed_copies
    from r3d.lit_mesh import finest_triangles, read_lit_mesh
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
    rest = work / f"{job.asset_name}.mesh"
    rest.write_bytes(next(data for name, _kind, data in entries if name == job.asset_name))
    mesh = read_lit_mesh(rest)
    q, rgb, tris, *_ = finest_triangles(mesh)
    drawn = (q / float(mesh.position_scale))[tris]
    copies = placed_copies(bundle, rgb is None)
    if copies is not None:
        drawn = np.concatenate([drawn, copies.points[copies.vertex_point][copies.tris]])
    parts = Parts(names, [source.object_names[o] for o in owner], parts_of_object, shared,
                  main_materials(labels, tri_m, source.names, count), np.bincount(labels, minlength=count),
                  drawn_parts(p, tri_v, labels, drawn, count))
    host = build_host(fv.VIEWER, work / "host", scene_path)
    held_out_mean, _p95 = fv.held_out_score(job, bundle, work, host, scene_path, inputs=inputs)
    totals = accumulate(work / "score" / "frames.avi", inputs / "reference_held_out", fv.BOARD_SCALE, ids_for)
    return (*rows_for(parts, shapes, totals), held_out_mean)
