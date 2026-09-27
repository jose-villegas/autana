"""Writes a baked mesh as the r3d_lit_mesh_t C data render/r3d_lit_mesh.h reads:
clusters from an octree, positions quantized to int16 ticks, the result
checked against the format's invariants before a byte is written."""

import os

import numpy as np

from r3d.octree import build_octree, flatten_octree, node_bounds

INT16_MAX = 32767
MAX_VERTICES = 65535  # uint16 indices
MAX_NODES = 65535
MAX_TRIANGLES = 65535  # uint16 triangle_first
MAX_CLUSTERS = 65535  # uint16 leaf first cluster
MAX_NODE_CHILDREN = 255


def write_lit_mesh(out_dir, name, positions, rgb, tris, double, banner_lines, leaf_triangles, max_depth,
                   position_scale=8):
    """Writes <name>_mesh_generated.h and .c into out_dir, defining <name>_mesh.
    positions are model units, rgb 0..255 per vertex, tris counter-clockwise
    seen from the front, double one flag per triangle; banner_lines open both
    files. Returns (vertices, triangles, clusters, nodes) as written."""
    root = build_octree(positions, tris, leaf_triangles, max_depth)
    clusters, nodes = flatten_octree(root, double)
    out_pos, out_rgb, out_tris, out_clusters = [], [], [], []
    vbase = tbase = 0
    for is_double, members in clusters:
        ct = tris[members]
        used, local = np.unique(ct, return_inverse=True)
        out_pos.append(positions[used])
        out_rgb.append(rgb[used])
        out_tris.append(local.reshape(-1, 3) + vbase)
        q = np.round(positions[used] * position_scale).astype(np.int64)
        out_clusters.append((vbase, len(used), tbase, len(ct), q.min(axis=0), q.max(axis=0), is_double))
        vbase += len(used)
        tbase += len(ct)

    q_pos = np.round(np.concatenate(out_pos) * position_scale).astype(np.int64)
    q_rgb = np.concatenate(out_rgb)
    q_tris = np.concatenate(out_tris)
    node_bounds(nodes, out_clusters)
    validate(q_pos, q_rgb, q_tris, out_clusters, nodes)
    emit(out_dir, name, banner_lines, position_scale, q_pos, q_rgb, q_tris, out_clusters, nodes)
    return len(q_pos), len(q_tris), len(out_clusters), len(nodes)


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


def emit(out_dir, name, banner_lines, position_scale, pos, rgb, tris, clusters, nodes):
    low, up = name, name.upper()
    header = f"{low}_mesh_generated.h"
    with open(os.path.join(out_dir, header), "w", newline="\n") as out:
        banner(banner_lines, out)
        print("#pragma once", file=out)
        print(file=out)
        print('#include "render/r3d_lit_mesh.h"', file=out)
        print(file=out)
        print(f"#define {up}_VERTEX_COUNT {len(pos)}", file=out)
        print(f"#define {up}_TRIANGLE_COUNT {len(tris)}", file=out)
        print(f"#define {up}_CLUSTER_COUNT {len(clusters)}", file=out)
        print(f"#define {up}_NODE_COUNT {len(nodes)}", file=out)
        print(f"#define {up}_POSITION_SCALE {position_scale}", file=out)
        print(file=out)
        print(f"extern const r3d_lit_mesh_t {low}_mesh;", file=out)

    with open(os.path.join(out_dir, f"{low}_mesh_generated.c"), "w", newline="\n") as out:
        banner(banner_lines, out)
        print(f'#include "{header}"', file=out)
        print(file=out)
        emit_rows(out, f"{low}_positions", "int16_t", pos.tolist(), 8)
        print(file=out)
        emit_rows(out, f"{low}_colors", "uint8_t", rgb.tolist(), 10)
        print(file=out)
        emit_rows(out, f"{low}_triangles", "uint16_t", tris.tolist(), 8)
        print(file=out)
        print(f"static const r3d_lit_cluster_t {low}_clusters[] = {{", file=out)
        for vbase, vcount, tbase, tcount, lo, hi, double in clusters:
            print(f"    {{{vbase}, {vcount}, {tbase}, {tcount}, {triple(lo)}, {triple(hi)}, {c_bool(double)}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"static const r3d_lit_node_t {low}_nodes[] = {{", file=out)
        for n in nodes:
            print(f"    {{{triple(n['lo'])}, {triple(n['hi'])}, {n['first']}, {n['count']}, {c_bool(n['leaf'])}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"const r3d_lit_mesh_t {low}_mesh = {{", file=out)
        print(f"    {low}_positions, {low}_colors, {low}_triangles, {low}_clusters, {low}_nodes,", file=out)
        print(f"    {up}_VERTEX_COUNT, {up}_TRIANGLE_COUNT, {up}_CLUSTER_COUNT, {up}_NODE_COUNT,", file=out)
        print(f"    {up}_POSITION_SCALE,", file=out)
        print("};", file=out)
