"""Bakes a lit mesh into the r3d_lit_mesh_t C data render/r3d_lit_mesh.h reads,
and reads such data back.

The triangles are cut into meshlets, compact clusters of a few dozen
triangles, under an octree over the meshlets' centres. Positions are
quantized to int16 ticks and the result is checked against the format's
invariants before a byte is written. A mesh's triangles are put in a canonical
order first, so the same triangles always bake to the same bytes."""

import os
import pathlib
import re
from types import SimpleNamespace

import numpy as np

from r3d.meshopt import build_meshlets
from r3d.octree import build_octree, flatten_octree, node_bounds

INT16_MAX = 32767
MAX_VERTICES = 65535  # uint16 indices
MAX_NODES = 65535
MAX_TRIANGLES = 65535  # uint16 triangle_first
MAX_CLUSTERS = 65535  # uint16 leaf first cluster
MAX_NODE_CHILDREN = 255

# What a bake uses unless asked otherwise, and the only place these are set.
MESHLET_TRIANGLES = 32
LEAF_TRIANGLES = 320
MAX_DEPTH = 10
POSITION_SCALE = 8


def weld_quantised(q, rgb, tris, double, face=None):
    """(q, rgb, tris, double, face): one vertex per position and colour, in
    order of position then colour, the triangles that collapsed dropped and the
    vertices nothing uses gone. A seam, two vertices at one position with
    different colours, stays a seam. Without vertex colours (`rgb` None, a flat
    mesh) only the position counts; `face`, a colour per triangle, keeps the
    rows of the triangles that stayed."""
    key = q if rgb is None else np.concatenate([q, rgb], axis=1)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    t = inverse.reshape(-1)[tris]
    keep = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    t, double = t[keep], np.asarray(double)[keep]
    used, local = np.unique(t, return_inverse=True)
    return (q[first][used], None if rgb is None else rgb[first][used], local.reshape(-1, 3), double,
            None if face is None else np.asarray(face)[keep])


def canonical_order(tris, double, face=None):
    """(tris, double, face): the triangles each turned to start at its
    smallest vertex, which keeps its winding, and sorted, so the order says
    nothing of where they came from. `face` follows its triangles."""
    start = np.argmin(tris, axis=1)
    rows = np.arange(len(tris))
    tris = np.stack([tris[rows, (start + k) % 3] for k in range(3)], axis=1)
    order = np.lexsort((tris[:, 2], tris[:, 1], tris[:, 0]))
    return tris[order], np.asarray(double)[order], None if face is None else np.asarray(face)[order]


def oriented(t):
    """A triangle's vertices turned to start at the smallest, which keeps the
    winding: the same key for a triangle however the meshlet builder turned it,
    and a different one for its back-to-back twin."""
    k = int(np.argmin(t))
    return (int(t[k]), int(t[(k + 1) % 3]), int(t[(k + 2) % 3]))


def local_vertices(tris):
    """(vertices, local tris): the vertices a triangle list uses in order of
    first use, and the triangles indexing that list."""
    unique, first, inverse = np.unique(tris.reshape(-1), return_index=True, return_inverse=True)
    order = np.argsort(first)
    rank = np.empty(len(unique), dtype=np.int64)
    rank[order] = np.arange(len(unique))
    return unique[order], rank[inverse].reshape(-1, 3)


def bake_lit_mesh(positions, rgb, tris, double, position_scale=POSITION_SCALE, leaf_triangles=LEAF_TRIANGLES,
                  max_depth=MAX_DEPTH, meshlet_triangles=MESHLET_TRIANGLES, face_rgb=None):
    """Bakes positions (model units), rgb (0..255 per vertex) and tris
    (counter-clockwise seen from the front, `double` one flag per triangle)
    into a SimpleNamespace holding pos, rgb, tris, clusters, nodes, the
    position_scale and face_colors. A cluster holds at most meshlet_triangles
    triangles of one sidedness; an octree leaf holds meshlets to leaf_triangles
    triangles. Given face_rgb, one 0..255 colour per triangle, the mesh is
    flat: rgb is None, vertices weld by position alone and face_colors holds
    the colours; otherwise face_colors is None."""
    q = np.round(np.asarray(positions) * position_scale).astype(np.int64)
    flat = face_rgb is not None
    col = None if flat else np.clip(np.rint(rgb), 0, 255).astype(np.int64)
    face = np.clip(np.rint(face_rgb), 0, 255).astype(np.int64) if flat else None
    assert not flat or len(face) == len(tris), "a flat bake needs one face colour per triangle"
    q, col, tris, double, face = weld_quantised(q, col, np.asarray(tris, dtype=np.int64), double, face)
    tris, double, face = canonical_order(tris, double, face)

    entries = []
    for is_double in (0, 1):
        member = double == is_double
        members = tris[member]
        if len(members):
            colours = {oriented(t): c for t, c in zip(members, face[member])} if flat else None
            assert not flat or len(colours) == len(members), "two triangles repeat one winding of the same vertices"
            entries += [{"double": is_double, "tris": t,
                         "face": None if colours is None else np.array([colours[oriented(x)] for x in t])}
                        for t in build_meshlets(q, members, meshlet_triangles)]
    for e in entries:
        e["box"] = box_of(q, e["tris"])
    centres = np.array([(lo + hi) / 2.0 for lo, hi in (e["box"] for e in entries)])
    root = build_octree(centres, [len(e["tris"]) for e in entries], leaf_triangles, max_depth)
    order, nodes = flatten_octree(root)

    out = SimpleNamespace(position_scale=position_scale)
    out.pos, out.rgb, out.tris, out.clusters, out.face_colors = lay_out(q, col, [entries[i] for i in order])
    node_bounds(nodes, out.clusters)
    out.nodes = nodes
    validate(out.pos, out.rgb, out.tris, out.clusters, out.nodes, out.face_colors)
    return out


def box_of(q, tris):
    p = q[np.unique(tris.reshape(-1))]
    return p.min(axis=0), p.max(axis=0)


def lay_out(q, col, entries):
    """The vertex, colour and triangle arrays of a run of clusters, each
    owning the vertices its triangles use, and the cluster rows over them.
    With no vertex colours (`col` None) each entry carries its faces' colours."""
    flat = col is None
    pos, rgb, tris, clusters, face = [], [], [], [], []
    vbase = tbase = 0
    for e in entries:
        used, local = local_vertices(e["tris"])
        pos.append(q[used])
        tris.append(local + vbase)
        if flat:
            face.append(e["face"])
        else:
            rgb.append(col[used])
        clusters.append((vbase, len(used), tbase, len(local), e["box"][0], e["box"][1], bool(e["double"])))
        vbase += len(used)
        tbase += len(local)
    return (np.concatenate(pos), None if flat else np.concatenate(rgb), np.concatenate(tris), clusters,
            np.concatenate(face) if flat else None)


def validate(pos, rgb, tris, clusters, nodes, face_colors=None):
    assert len(pos) <= MAX_VERTICES, f"{len(pos)} vertices exceed uint16 indices"
    assert len(tris) <= MAX_TRIANGLES, f"{len(tris)} triangles exceed uint16 offsets"
    assert len(clusters) <= MAX_CLUSTERS, f"{len(clusters)} clusters exceed uint16 offsets"
    assert np.abs(pos).max() <= INT16_MAX, "a position does not fit int16"
    assert (face_colors is not None) != (rgb is not None), "vertex colours or face colours, never both"
    colours = rgb if rgb is not None else face_colors
    assert colours.min() >= 0 and colours.max() <= 255
    assert len(colours) == (len(pos) if rgb is not None else len(tris))
    assert tris.min() >= 0 and tris.max() < len(pos)
    assert np.all((tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2]))
    next_v = next_t = 0
    for vbase, vcount, tbase, tcount, lo, hi, _ in clusters:
        assert vbase == next_v and tbase == next_t, "clusters must tile both arrays in order"
        ct = tris[tbase : tbase + tcount]
        assert ct.min() >= vbase and ct.max() < vbase + vcount, "a triangle reaches outside its cluster"
        cp = pos[vbase : vbase + vcount]
        assert np.all(cp >= lo) and np.all(cp <= hi)
        next_v, next_t = vbase + vcount, tbase + tcount
    assert next_v == len(pos) and next_t == len(tris)
    reached = np.zeros(len(clusters), dtype=np.int64)
    stack = [0]
    while stack:
        node = nodes[stack.pop()]
        assert node["count"] <= MAX_NODE_CHILDREN
        if node["leaf"]:
            reached[node["first"] : node["first"] + node["count"]] += 1
        else:
            stack.extend(range(node["first"], node["first"] + node["count"]))
    assert np.all(reached == 1), "every cluster must hang off exactly one leaf"
    assert len(nodes) <= MAX_NODES


def write_lit_mesh(out_dir, name, positions, rgb, tris, double, banner_lines, **options):
    """Writes <name>_mesh_generated.h and .c into out_dir, defining <name>_mesh.
    positions are model units, rgb 0..255 per vertex (None for a flat mesh),
    tris counter-clockwise seen from the front, double one flag per triangle;
    banner_lines open both
    files, and options, face_rgb among them, go to bake_lit_mesh. Returns the
    baked mesh."""
    mesh = bake_lit_mesh(positions, rgb, tris, double, **options)
    emit(out_dir, name, banner_lines, mesh)
    return mesh


def emit_rows(out, name, ctype, rows, per_line):
    print(f"static const {ctype} {name}[][{len(rows[0])}] = {{", file=out)
    for i in range(0, len(rows), per_line):
        chunk = rows[i : i + per_line]
        print("    " + " ".join("{" + ",".join(str(int(v)) for v in r) + "}," for r in chunk), file=out)
    print("};", file=out)


def banner(lines, out):
    print("/*", file=out)
    for line in lines:
        print((" * " + line).rstrip(), file=out)
    print(" */", file=out)


def triple(v):
    return "{" + ", ".join(str(int(x)) for x in v) + "}"


def c_bool(v):
    return "true" if v else "false"


def emit(out_dir, name, banner_lines, mesh):
    low, up = name, name.upper()
    header = f"{low}_mesh_generated.h"
    with open(os.path.join(out_dir, header), "w", newline="\n") as out:
        banner(banner_lines, out)
        print("#pragma once", file=out)
        print(file=out)
        print('#include "render/r3d_lit_mesh.h"', file=out)
        print(file=out)
        print(f"#define {up}_VERTEX_COUNT {len(mesh.pos)}", file=out)
        print(f"#define {up}_TRIANGLE_COUNT {len(mesh.tris)}", file=out)
        print(f"#define {up}_CLUSTER_COUNT {len(mesh.clusters)}", file=out)
        print(f"#define {up}_NODE_COUNT {len(mesh.nodes)}", file=out)
        print(f"#define {up}_POSITION_SCALE {mesh.position_scale}", file=out)
        print(file=out)
        print(f"extern const r3d_lit_mesh_t {low}_mesh;", file=out)

    with open(os.path.join(out_dir, f"{low}_mesh_generated.c"), "w", newline="\n") as out:
        banner(banner_lines, out)
        print(f'#include "{header}"', file=out)
        if mesh.face_colors is not None:
            print('#include "gfx/gfx_color.h"', file=out)
        print(file=out)
        emit_rows(out, f"{low}_positions", "int16_t", mesh.pos.tolist(), 8)
        print(file=out)
        if mesh.rgb is not None:
            emit_rows(out, f"{low}_colors", "uint8_t", mesh.rgb.tolist(), 10)
            print(file=out)
        emit_rows(out, f"{low}_triangles", "uint16_t", mesh.tris.tolist(), 8)
        print(file=out)
        if mesh.face_colors is not None:
            print(f"static const uint16_t {low}_face_colors[] = {{", file=out)
            for i in range(0, len(mesh.face_colors), 6):
                print("    " + " ".join(f"GFX_RGB(0x{r:02X}{g:02X}{bl:02X})," for r, g, bl in
                                        mesh.face_colors[i : i + 6].tolist()), file=out)
            print("};", file=out)
            print(file=out)
        print(f"static const r3d_lit_cluster_t {low}_clusters[] = {{", file=out)
        for vbase, vcount, tbase, tcount, lo, hi, double in mesh.clusters:
            print(f"    {{{vbase}, {vcount}, {tbase}, {tcount}, {triple(lo)}, {triple(hi)}, {c_bool(double)}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"static const r3d_lit_node_t {low}_nodes[] = {{", file=out)
        for n in mesh.nodes:
            print(f"    {{{triple(n['lo'])}, {triple(n['hi'])}, {n['first']}, {n['count']}, {c_bool(n['leaf'])}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"const r3d_lit_mesh_t {low}_mesh = {{", file=out)
        print(f"    .positions = {low}_positions,", file=out)
        if mesh.rgb is not None:
            print(f"    .colors = {low}_colors,", file=out)
        if mesh.face_colors is not None:
            print(f"    .face_colors = {low}_face_colors,", file=out)
        print(f"    .triangles = {low}_triangles, .clusters = {low}_clusters, .nodes = {low}_nodes,", file=out)
        print(f"    .vertex_count = {up}_VERTEX_COUNT, .triangle_count = {up}_TRIANGLE_COUNT,", file=out)
        print(f"    .cluster_count = {up}_CLUSTER_COUNT, .node_count = {up}_NODE_COUNT,", file=out)
        print(f"    .position_scale = {up}_POSITION_SCALE,", file=out)
        print("};", file=out)


def parse_arrays(text):
    """{array name: flat list of its integers} for every `static const`
    array in generated mesh C, `true`/`false` read as 1/0."""
    arrays = {}
    for m in re.finditer(r"static const \w+ (\w+)\[\](?:\[\d+\])? = \{(.*?)\n\};", text, re.DOTALL):
        body = m.group(2).replace("true", "1").replace("false", "0")
        arrays[m.group(1)] = [int(x) for x in re.findall(r"-?\d+", body)]
    return arrays


def read_lit_mesh(path):
    """Reads a generated <name>_mesh_generated.c back into what bake_lit_mesh
    returns."""
    path = pathlib.Path(path)
    text = path.read_text()
    low = re.search(r"const r3d_lit_mesh_t (\w+)_mesh = ", text).group(1)
    arrays = parse_arrays(text)
    mesh = SimpleNamespace()
    mesh.position_scale = int(re.search(rf"{low.upper()}_POSITION_SCALE (\d+)", path.with_suffix(".h").read_text()).group(1))
    mesh.pos = np.array(arrays[f"{low}_positions"], dtype=np.int64).reshape(-1, 3)
    mesh.rgb = np.array(arrays[f"{low}_colors"], dtype=np.int64).reshape(-1, 3) if f"{low}_colors" in arrays else None
    faces = re.search(rf"{low}_face_colors\[\] = \{{(.*?)\n\}};", text, re.DOTALL)
    mesh.face_colors = None if faces is None else np.array(
        [[int(h[i : i + 2], 16) for i in (0, 2, 4)] for h in re.findall(r"GFX_RGB\(0x([0-9A-Fa-f]{6})\)", faces.group(1))],
        dtype=np.int64)
    mesh.tris = np.array(arrays[f"{low}_triangles"], dtype=np.int64).reshape(-1, 3)
    rows = np.array(arrays[f"{low}_clusters"], dtype=np.int64).reshape(-1, 11)
    mesh.clusters = [(r[0], r[1], r[2], r[3], r[4:7], r[7:10], bool(r[10])) for r in rows]
    nodes = np.array(arrays[f"{low}_nodes"], dtype=np.int64).reshape(-1, 9)
    mesh.nodes = [{"lo": r[0:3], "hi": r[3:6], "first": r[6], "count": r[7], "leaf": bool(r[8])} for r in nodes]
    return mesh


def finest_triangles(mesh):
    """(pos, rgb, tris, double, face) of a read mesh with the per-cluster
    vertex copies welded back together: one vertex per position and colour, or
    per position alone for a flat mesh, whose rgb is None."""
    double = np.zeros(len(mesh.tris), dtype=np.int64)
    for _, _, tbase, tcount, _, _, is_double in mesh.clusters:
        double[tbase : tbase + tcount] = int(is_double)
    return weld_quantised(mesh.pos, mesh.rgb, mesh.tris, double, mesh.face_colors)
