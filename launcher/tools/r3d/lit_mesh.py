"""Bakes a lit mesh into the r3d_lit_mesh_t C data render/r3d_lit_mesh.h reads,
and reads such data back.

The triangles become meshlets, a few dozen each. Neighbouring meshlets are
merged and simplified, with each group's outer border locked, and split again
until one is left (meshoptimizer's clusterlod scheme); the finest meshlets sit
under an octree, the coarser ones beside it with the error each was
simplified to. Positions are quantized to int16 ticks, and the result is
checked against the format's invariants before a byte is written."""

import os
import pathlib
import re
from types import SimpleNamespace

import numpy as np

from r3d.meshopt import cluster_lod
from r3d.octree import build_octree, flatten_octree, node_bounds

INT16_MAX = 32767
MAX_VERTICES = 65535  # uint16 indices
MAX_NODES = 65535
MAX_TRIANGLES = 65535  # uint16 triangle_first
MAX_CLUSTERS = 65535  # uint16 leaf first cluster
MAX_NODE_CHILDREN = 255
MAX_RADIUS = 65535
LOD_TOP = 3.0e38  # a parent error nothing coarser replaces (R3D_LIT_LOD_TOP)
CONE_NONE = 127


def weld_quantised(q, rgb, tris, double):
    """One vertex per position and colour, the triangles that collapsed
    dropped and the vertices nothing uses gone. A seam, two vertices at one
    position with different colours, stays a seam."""
    key = np.concatenate([q, rgb], axis=1)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    t = inverse.reshape(-1)[tris]
    keep = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    t, double = t[keep], np.asarray(double)[keep]
    used, local = np.unique(t, return_inverse=True)
    return q[first][used], rgb[first][used], local.reshape(-1, 3), double


def local_vertices(tris):
    """(vertices, local tris): the vertices a triangle list uses in order of
    first use, and the triangles indexing that list."""
    unique, first, inverse = np.unique(tris.reshape(-1), return_index=True, return_inverse=True)
    order = np.argsort(first)
    rank = np.empty(len(unique), dtype=np.int64)
    rank[order] = np.arange(len(unique))
    return unique[order], rank[inverse].reshape(-1, 3)


def triangle_normals(pos, tris):
    a, b, c = (pos[tris[:, k]].astype(np.float64) for k in range(3))
    return np.cross(b - a, c - a)


def cone(pos, tris, axis, double):
    """(axis, cutoff) as int8s: the mean-normal axis meshoptimizer found,
    quantized, and the smallest cutoff that still holds every triangle's
    normal, or CONE_NONE when the cluster cannot be culled by facing."""
    none = (np.zeros(3, dtype=np.int64), CONE_NONE)
    if double:
        return none
    q = np.round(np.asarray(axis, dtype=np.float64) * 127.0)
    length = np.linalg.norm(q)
    if length == 0:
        return none
    n = triangle_normals(pos, tris)
    size = np.linalg.norm(n, axis=1)
    n = n[size > 0] / size[size > 0, None]
    if len(n) == 0:
        return none
    spread = (n @ (q / length)).min()
    if spread <= 0:
        return none
    cutoff = int(np.ceil(length * np.sqrt(max(0.0, 1.0 - spread * spread)) + 1e-9))
    return (q.astype(np.int64), min(cutoff, CONE_NONE))


def sphere(bounds):
    """(centre ticks, radius, error) of a float sphere, rounded so the int16
    sphere still holds the float one."""
    centre = np.round(bounds[:3]).astype(np.int64)
    radius = int(np.ceil(bounds[3] + 1.0))
    error = LOD_TOP if bounds[4] >= 1e30 else float(bounds[4])
    return centre, radius, error


def bake_lit_mesh(positions, rgb, tris, double, leaf_triangles, max_depth, position_scale=8, meshlet_triangles=64,
                  partition_size=8, colour_weight=1.0):
    """Bakes positions (model units), rgb (0..255 per vertex) and tris
    (counter-clockwise seen from the front, `double` one flag per triangle)
    into a SimpleNamespace holding the finest level (pos, rgb, tris, clusters,
    nodes), the coarser levels (lod, or None for a mesh too small to have
    any) and the source vertex each vertex came from."""
    q = np.round(np.asarray(positions) * position_scale).astype(np.int64)
    col = np.clip(np.rint(rgb), 0, 255).astype(np.int64)
    tris = np.asarray(tris, dtype=np.int64)
    q, col, tris, double = weld_quantised(q, col, tris, double)

    classes = [np.flatnonzero(double == d) for d in (0, 1)]
    classes = [c for c in classes if len(c)]
    lock = np.zeros(len(q), dtype=np.uint8)
    if len(classes) == 2:
        both = np.intersect1d(tris[classes[0]].ravel(), tris[classes[1]].ravel())
        lock[both] = 1

    entries = []  # one per cluster of every level and class
    for members in classes:
        is_double = int(double[members[0]])
        built = cluster_lod(q, col, tris[members], lock, meshlet_triangles, partition_size, colour_weight)
        for k, ct in enumerate(built.cluster_tris):
            refined = int(built.cluster_refined[k])
            entries.append({
                "double": is_double,
                "level": 0 if refined < 0 else int(built.group_depth[refined]) + 1,
                "tris": ct,
                "self": sphere(built.cluster_bounds[k]),
                "parent": sphere(built.group_bounds[built.cluster_group[k]]),
                "axis": built.cluster_cone[k][:3],
            })

    finest = [e for e in entries if e["level"] == 0]
    coarse = sorted((e for e in entries if e["level"] > 0), key=lambda e: e["level"])
    for e in finest + coarse:
        e["box"] = box_of(q, e["tris"])
        e["cone"] = cone(q, e["tris"], e["axis"], e["double"])

    centres = np.array([(e["box"][0] + e["box"][1]) / 2.0 for e in finest])
    root = build_octree(centres, [len(e["tris"]) for e in finest], leaf_triangles, max_depth)
    order, nodes = flatten_octree(root)
    finest = [finest[i] for i in order]

    out = SimpleNamespace(position_scale=position_scale)
    out.pos, out.rgb, out.tris, out.clusters, out.source = lay_out(q, col, finest)
    node_bounds(nodes, out.clusters)
    out.nodes = nodes
    out.records = records(finest + coarse)
    lod = None
    if coarse:
        lod = SimpleNamespace()
        lod.pos, lod.rgb, lod.tris, lod.clusters, lod.source = lay_out(q, col, coarse)
        lod.level_count = max(e["level"] for e in coarse) + 1
        lod.cluster_count = len(coarse)
    out.lod = lod
    validate(out.pos, out.rgb, out.tris, out.clusters, out.nodes)
    validate_lod(out)
    return out


def box_of(q, tris):
    p = q[np.unique(tris.reshape(-1))]
    return p.min(axis=0), p.max(axis=0)


def lay_out(q, col, entries):
    """The vertex, colour and triangle arrays of a run of clusters, each
    owning the vertices its triangles use, and the cluster rows over them."""
    pos, rgb, tris, source, clusters = [], [], [], [], []
    vbase = tbase = 0
    for e in entries:
        used, local = local_vertices(e["tris"])
        pos.append(q[used])
        rgb.append(col[used])
        tris.append(local + vbase)
        source.append(used)
        clusters.append((vbase, len(used), tbase, len(local), e["box"][0], e["box"][1], bool(e["double"])))
        vbase += len(used)
        tbase += len(local)
    return np.concatenate(pos), np.concatenate(rgb), np.concatenate(tris), clusters, np.concatenate(source)


def records(entries):
    return [{"self": e["self"], "parent": e["parent"], "cone": e["cone"], "level": e["level"]} for e in entries]


def validate(pos, rgb, tris, clusters, nodes):
    assert len(pos) <= MAX_VERTICES, f"{len(pos)} vertices exceed uint16 indices"
    assert len(tris) <= MAX_TRIANGLES, f"{len(tris)} triangles exceed uint16 offsets"
    assert len(clusters) <= MAX_CLUSTERS, f"{len(clusters)} clusters exceed uint16 offsets"
    assert np.abs(pos).max() <= INT16_MAX, "a position does not fit int16"
    assert rgb.min() >= 0 and rgb.max() <= 255
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
    if not nodes:
        return
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


def validate_lod(mesh):
    """The coarser levels tile their own arrays as the finest do, and every
    cluster's error stays below its parent's."""
    total = len(mesh.clusters) + (mesh.lod.cluster_count if mesh.lod else 0)
    assert len(mesh.records) == total, "one record per cluster, finest first"
    for r in mesh.records:
        assert r["self"][2] <= r["parent"][2], "a cluster is coarser than its parent"
        for centre, radius, _ in (r["self"], r["parent"]):
            assert np.abs(centre).max() <= INT16_MAX and 0 < radius <= MAX_RADIUS, "a sphere does not fit"
        assert -127 <= r["cone"][0].min() and r["cone"][0].max() <= 127 and 0 <= r["cone"][1] <= CONE_NONE
    assert all(r["level"] == 0 for r in mesh.records[: len(mesh.clusters)])
    if mesh.lod:
        assert all(r["level"] > 0 for r in mesh.records[len(mesh.clusters) :])
        validate(mesh.lod.pos, mesh.lod.rgb, mesh.lod.tris, mesh.lod.clusters, [])


def write_lit_mesh(out_dir, name, positions, rgb, tris, double, banner_lines, leaf_triangles, max_depth,
                   position_scale=8, **options):
    """Writes <name>_mesh_generated.h and .c into out_dir, defining <name>_mesh.
    positions are model units, rgb 0..255 per vertex, tris counter-clockwise
    seen from the front, double one flag per triangle; banner_lines open both
    files, and options go to bake_lit_mesh. Returns the baked mesh."""
    mesh = bake_lit_mesh(positions, rgb, tris, double, leaf_triangles, max_depth, position_scale, **options)
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


def c_float(v):
    if v >= LOD_TOP:
        return "R3D_LIT_LOD_TOP"
    text = f"{v:.9g}"
    return text + ("F" if "." in text or "e" in text else ".0F")


def bound(b):
    centre, radius, error = b
    return f"{{{triple(centre)}, {radius}, {c_float(error)}}}"


def emit_clusters(out, low, prefix, clusters):
    print(f"static const r3d_lit_cluster_t {low}_{prefix}clusters[] = {{", file=out)
    for vbase, vcount, tbase, tcount, lo, hi, double in clusters:
        print(f"    {{{vbase}, {vcount}, {tbase}, {tcount}, {triple(lo)}, {triple(hi)}, {c_bool(double)}}},", file=out)
    print("};", file=out)


def emit_arrays(out, low, prefix, pos, rgb, tris, clusters):
    emit_rows(out, f"{low}_{prefix}positions", "int16_t", pos.tolist(), 8)
    print(file=out)
    emit_rows(out, f"{low}_{prefix}colors", "uint8_t", rgb.tolist(), 10)
    print(file=out)
    emit_rows(out, f"{low}_{prefix}triangles", "uint16_t", tris.tolist(), 8)
    print(file=out)
    emit_clusters(out, low, prefix, clusters)
    print(file=out)


def emit(out_dir, name, banner_lines, mesh):
    low, up = name, name.upper()
    header = f"{low}_mesh_generated.h"
    lod = mesh.lod
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
        if lod:
            print(f"#define {up}_LOD_VERTEX_COUNT {len(lod.pos)}", file=out)
            print(f"#define {up}_LOD_TRIANGLE_COUNT {len(lod.tris)}", file=out)
            print(f"#define {up}_LOD_CLUSTER_COUNT {lod.cluster_count}", file=out)
            print(f"#define {up}_LOD_LEVEL_COUNT {lod.level_count}", file=out)
        print(file=out)
        print(f"extern const r3d_lit_mesh_t {low}_mesh;", file=out)

    with open(os.path.join(out_dir, f"{low}_mesh_generated.c"), "w", newline="\n") as out:
        banner(banner_lines, out)
        print(f'#include "{header}"', file=out)
        print(file=out)
        emit_arrays(out, low, "", mesh.pos, mesh.rgb, mesh.tris, mesh.clusters)
        print(f"static const r3d_lit_node_t {low}_nodes[] = {{", file=out)
        for n in mesh.nodes:
            print(f"    {{{triple(n['lo'])}, {triple(n['hi'])}, {n['first']}, {n['count']}, {c_bool(n['leaf'])}}},", file=out)
        print("};", file=out)
        print(file=out)
        if lod:
            emit_arrays(out, low, "lod_", lod.pos, lod.rgb, lod.tris, lod.clusters)
            print(f"static const r3d_lit_cluster_lod_t {low}_lod_records[] = {{", file=out)
            for r in mesh.records:
                axis, cutoff = r["cone"]
                print(f"    {{{bound(r['self'])}, {bound(r['parent'])}, {triple(axis)}, {cutoff}, {r['level']}}},", file=out)
            print("};", file=out)
            print(file=out)
            print(f"static const r3d_lit_lod_t {low}_lod = {{", file=out)
            print(f"    {low}_lod_positions, {low}_lod_colors, {low}_lod_triangles, {low}_lod_clusters,", file=out)
            print(f"    {low}_lod_records, {up}_LOD_VERTEX_COUNT, {up}_LOD_TRIANGLE_COUNT, {up}_LOD_CLUSTER_COUNT,", file=out)
            print(f"    {up}_LOD_LEVEL_COUNT,", file=out)
            print("};", file=out)
            print(file=out)
        print(f"const r3d_lit_mesh_t {low}_mesh = {{", file=out)
        print(f"    {low}_positions, {low}_colors, {low}_triangles, {low}_clusters, {low}_nodes,", file=out)
        print(f"    {up}_VERTEX_COUNT, {up}_TRIANGLE_COUNT, {up}_CLUSTER_COUNT, {up}_NODE_COUNT,", file=out)
        print(f"    {up}_POSITION_SCALE, {'&' + low + '_lod' if lod else 'NULL'},", file=out)
        print("};", file=out)


NUMBER = re.compile(r"-?\d+(?:\.\d+)?(?:e[+-]?\d+)?", re.IGNORECASE)


def parse_arrays(text):
    """{array name: flat list of its numbers} for every `static const`
    array in generated mesh C, `true`/`false` read as 1/0 and the parent
    error nothing replaces as LOD_TOP."""
    arrays = {}
    for m in re.finditer(r"static const \w+ (\w+)\[\](?:\[\d+\])? = \{(.*?)\n\};", text, re.DOTALL):
        body = m.group(2).replace("true", "1").replace("false", "0").replace("R3D_LIT_LOD_TOP", repr(LOD_TOP))
        arrays[m.group(1)] = [float(x) if re.search(r"[.eE]", x) else int(x) for x in NUMBER.findall(body)]
    return arrays


def read_level(arrays, low, prefix):
    pos = np.array(arrays[f"{low}_{prefix}positions"], dtype=np.int64).reshape(-1, 3)
    rgb = np.array(arrays[f"{low}_{prefix}colors"], dtype=np.int64).reshape(-1, 3)
    tris = np.array(arrays[f"{low}_{prefix}triangles"], dtype=np.int64).reshape(-1, 3)
    rows = np.array(arrays[f"{low}_{prefix}clusters"], dtype=np.int64).reshape(-1, 11)
    clusters = [(r[0], r[1], r[2], r[3], r[4:7], r[7:10], bool(r[10])) for r in rows]
    return SimpleNamespace(pos=pos, rgb=rgb, tris=tris, clusters=clusters, source=None)


def read_lit_mesh(path):
    """Reads a generated <name>_mesh_generated.c back into what bake_lit_mesh
    returns, apart from the source vertices, whether it was written before
    the coarser levels existed or after."""
    text = pathlib.Path(path).read_text()
    low = re.search(r"const r3d_lit_mesh_t (\w+)_mesh = ", text).group(1)
    arrays = parse_arrays(text)
    mesh = read_level(arrays, low, "")
    mesh.position_scale = int(re.search(rf"{low.upper()}_POSITION_SCALE (\d+)", pathlib.Path(path).with_suffix(".h").read_text()).group(1))
    nodes = np.array(arrays[f"{low}_nodes"], dtype=np.int64).reshape(-1, 9)
    mesh.nodes = [{"lo": r[0:3], "hi": r[3:6], "first": r[6], "count": r[7], "leaf": bool(r[8])} for r in nodes]
    mesh.records, mesh.lod = [], None
    if f"{low}_lod_records" in arrays:
        lod = read_level(arrays, low, "lod_")
        rows = np.array(arrays[f"{low}_lod_records"], dtype=object).reshape(-1, 15)
        for r in rows:
            mesh.records.append({
                "self": (np.array(r[0:3], dtype=np.int64), int(r[3]), float(r[4])),
                "parent": (np.array(r[5:8], dtype=np.int64), int(r[8]), float(r[9])),
                "cone": (np.array(r[10:13], dtype=np.int64), int(r[13])),
                "level": int(r[14]),
            })
        lod.cluster_count = len(lod.clusters)
        lod.level_count = max(r["level"] for r in mesh.records) + 1
        mesh.lod = lod
    return mesh


def finest_triangles(mesh):
    """(pos, rgb, tris, double) of the finest level with the per-cluster
    vertex copies welded back together: one vertex per position and colour."""
    double = np.zeros(len(mesh.tris), dtype=np.int64)
    for _, _, tbase, tcount, _, _, is_double in mesh.clusters:
        double[tbase : tbase + tcount] = int(is_double)
    return weld_quantised(mesh.pos, mesh.rgb, mesh.tris, double)
