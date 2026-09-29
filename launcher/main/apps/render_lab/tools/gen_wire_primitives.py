#!/usr/bin/env python3
"""Generate main/apps/render_lab/wire_primitives_generated.h - the baked
wire_mesh_t tables for the wireframe demo primitives (plane, cube, UV
sphere, capsule).

    python main/apps/render_lab/tools/gen_wire_primitives.py > main/apps/render_lab/wire_primitives_generated.h

Every coordinate is an integer in S3L_F (512 = 1.0) fixed point, computed
here in floating point and rounded once at the end - there is no rasterizer
or device code in this script, only the geometry. The generator validates
its own output (indices in range, no duplicate or degenerate edge in either
direction, every coordinate fits int16, vertex/edge counts within the
mesh-wide 1024/2048 hard limits) before emitting anything.
"""

import argparse
import math
import sys

S3L_F = 512
MAX_VERTICES = 1024
MAX_EDGES = 2048
INT16_MIN = -32768
INT16_MAX = 32767

CUBE_HALF_EXTENT = S3L_F


def fx(value):
    """Round a float S3L_F-unit coordinate to its nearest integer tick."""
    return int(round(value))


def build_plane(n, size):
    half = size / 2.0
    step = size / (n - 1) if n > 1 else 0.0
    vertices = []
    for row in range(n):
        z = -half + row * step
        for col in range(n):
            x = -half + col * step
            vertices.append((fx(x), 0, fx(z)))

    edges = []
    for row in range(n):
        for col in range(n - 1):
            edges.append((row * n + col, row * n + col + 1))
    for col in range(n):
        for row in range(n - 1):
            edges.append((row * n + col, (row + 1) * n + col))
    return vertices, edges


def build_cube(half_extent):
    vertices = []
    for i in range(8):
        sx = half_extent if (i & 1) else -half_extent
        sy = half_extent if (i & 2) else -half_extent
        sz = half_extent if (i & 4) else -half_extent
        vertices.append((sx, sy, sz))

    # Two corners are an edge apart exactly when their indices differ in one
    # bit - the standard hypercube adjacency, which yields all 12 cube edges
    # with no separate connectivity bookkeeping.
    edges = []
    for i in range(8):
        for bit in range(3):
            j = i ^ (1 << bit)
            if i < j:
                edges.append((i, j))
    return vertices, edges


def hemisphere_rings(rings, meridians, radius, span):
    """Latitude rings strictly between a pole and the equator, span radians
    wide (pi for a full sphere, pi/2 for one capsule hemisphere)."""
    ring_vertices = []
    for i in range(rings):
        theta = span * (i + 1) / (rings if span < math.pi else rings + 1)
        y = radius * math.cos(theta)
        ring_r = radius * math.sin(theta)
        row = []
        for m in range(meridians):
            phi = 2.0 * math.pi * m / meridians
            row.append((fx(ring_r * math.cos(phi)), fx(y), fx(ring_r * math.sin(phi))))
        ring_vertices.append(row)
    return ring_vertices


def ring_edges(base, rings, meridians):
    edges = []
    for i in range(rings):
        ring_base = base + i * meridians
        for m in range(meridians):
            edges.append((ring_base + m, ring_base + (m + 1) % meridians))
    return edges


def meridian_edges(pole, base, rings, meridians):
    edges = [(pole, base + m) for m in range(meridians)]
    for i in range(rings - 1):
        base_i = base + i * meridians
        base_next = base + (i + 1) * meridians
        edges += [(base_i + m, base_next + m) for m in range(meridians)]
    return edges


def build_sphere(rings, meridians, radius):
    north = 0
    ring_base = 1
    rows = hemisphere_rings(rings, meridians, radius, math.pi)
    vertices = [(0, fx(radius), 0)]
    for row in rows:
        vertices += [(x, y, z) for x, y, z in row]
    south = len(vertices)
    vertices.append((0, -fx(radius), 0))

    edges = ring_edges(ring_base, rings, meridians)
    edges += meridian_edges(north, ring_base, rings, meridians)
    edges += [(south, ring_base + (rings - 1) * meridians + m) for m in range(meridians)]
    return vertices, edges


def build_capsule(rings, meridians, radius, cyl_half_len):
    rows = hemisphere_rings(rings, meridians, radius, math.pi / 2.0)

    top_pole = 0
    top_base = 1
    vertices = [(0, fx(cyl_half_len + radius), 0)]
    for x, y, z in (v for row in rows for v in row):
        vertices.append((x, y + fx(cyl_half_len), z))

    bottom_base = len(vertices)
    for x, y, z in (v for row in rows for v in row):
        vertices.append((x, -(y + fx(cyl_half_len)), z))
    bottom_pole = len(vertices)
    vertices.append((0, -fx(cyl_half_len + radius), 0))

    edges = ring_edges(top_base, rings, meridians)
    edges += meridian_edges(top_pole, top_base, rings, meridians)
    edges += ring_edges(bottom_base, rings, meridians)
    edges += meridian_edges(bottom_pole, bottom_base, rings, meridians)

    top_equator = top_base + (rings - 1) * meridians
    bottom_equator = bottom_base + (rings - 1) * meridians
    edges += [(top_equator + m, bottom_equator + m) for m in range(meridians)]
    return vertices, edges


def validate(name, vertices, edges):
    if len(vertices) > MAX_VERTICES:
        raise ValueError(f"{name}: {len(vertices)} vertices exceeds the {MAX_VERTICES} limit")
    if len(edges) > MAX_EDGES:
        raise ValueError(f"{name}: {len(edges)} edges exceeds the {MAX_EDGES} limit")

    for x, y, z in vertices:
        for coord in (x, y, z):
            if coord < INT16_MIN or coord > INT16_MAX:
                raise ValueError(f"{name}: coordinate {coord} does not fit int16")

    seen = set()
    normalized = []
    for a, b in edges:
        if not (0 <= a < len(vertices) and 0 <= b < len(vertices)):
            raise ValueError(f"{name}: edge ({a}, {b}) has an index out of range")
        if a == b:
            raise ValueError(f"{name}: edge ({a}, {b}) is degenerate")
        key = (a, b) if a < b else (b, a)
        if key in seen:
            raise ValueError(f"{name}: duplicate edge {key}")
        seen.add(key)
        normalized.append(key)
    return normalized


def emit_mesh(f, name, vertices, edges):
    f.write(f"static const wire_vertex_t {name}_vertices[{len(vertices)}] = {{\n")
    for i in range(0, len(vertices), 4):
        row = vertices[i : i + 4]
        f.write("    " + " ".join(f"{{{x}, {y}, {z}}}," for x, y, z in row) + "\n")
    f.write("};\n\n")

    f.write(f"static const wire_edge_t {name}_edges[{len(edges)}] = {{\n")
    for i in range(0, len(edges), 8):
        row = edges[i : i + 8]
        f.write("    " + " ".join(f"{{{a}, {b}}}," for a, b in row) + "\n")
    f.write("};\n\n")

    f.write(
        f"static const wire_mesh_t {name}_mesh = {{\n"
        f"    {name}_vertices, {name}_edges, {len(vertices)}, {len(edges)},\n"
        f"}};\n\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plane-n", type=int, default=24)
    parser.add_argument("--plane-size", type=int, default=8 * S3L_F)
    parser.add_argument("--sphere-rings", type=int, default=16)
    parser.add_argument("--sphere-meridians", type=int, default=24)
    parser.add_argument("--capsule-rings", type=int, default=8)
    parser.add_argument("--capsule-meridians", type=int, default=12)
    parser.add_argument("--radius", type=int, default=2 * S3L_F)
    args = parser.parse_args()

    plane_v, plane_e = build_plane(args.plane_n, args.plane_size)
    cube_v, cube_e = build_cube(CUBE_HALF_EXTENT)
    sphere_v, sphere_e = build_sphere(args.sphere_rings, args.sphere_meridians, args.radius)
    capsule_v, capsule_e = build_capsule(args.capsule_rings, args.capsule_meridians, args.radius, args.radius)

    plane_e = validate("plane", plane_v, plane_e)
    cube_e = validate("cube", cube_v, cube_e)
    sphere_e = validate("sphere", sphere_v, sphere_e)
    capsule_e = validate("capsule", capsule_v, capsule_e)

    # Every text file in the tree is LF; plain stdout redirection on
    # Windows would otherwise write CRLF.
    sys.stdout.reconfigure(newline="\n")
    f = sys.stdout
    f.write(
        "/*\n"
        " * GENERATED FILE - do not edit.\n"
        " *\n"
        " *     python main/apps/render_lab/tools/gen_wire_primitives.py > "
        "main/apps/render_lab/wire_primitives_generated.h\n"
        " *\n"
        " * Baked wire_mesh_t tables for the wireframe demo primitives: a\n"
        " * centred XZ plane grid, a cube, a UV sphere and a two-hemisphere\n"
        " * capsule. Coordinates are S3L_F fixed point; density is a bake-time\n"
        " * knob set by the generator's own arguments, recorded below as the\n"
        " * defines a test can read them back from.\n"
        " */\n"
        "#pragma once\n\n"
        '#include "wire_mesh.h"\n\n'
        f"#define WIRE_PLANE_N {args.plane_n}\n"
        f"#define WIRE_PLANE_SIZE {args.plane_size}\n"
        f"#define WIRE_CUBE_HALF_EXTENT {CUBE_HALF_EXTENT}\n"
        f"#define WIRE_SPHERE_RINGS {args.sphere_rings}\n"
        f"#define WIRE_SPHERE_MERIDIANS {args.sphere_meridians}\n"
        f"#define WIRE_SPHERE_RADIUS {args.radius}\n"
        f"#define WIRE_CAPSULE_RINGS {args.capsule_rings}\n"
        f"#define WIRE_CAPSULE_MERIDIANS {args.capsule_meridians}\n"
        f"#define WIRE_CAPSULE_RADIUS {args.radius}\n"
        f"#define WIRE_CAPSULE_CYLINDER_HALF_LEN {args.radius}\n\n"
    )
    emit_mesh(f, "wire_plane", plane_v, plane_e)
    emit_mesh(f, "wire_cube", cube_v, cube_e)
    emit_mesh(f, "wire_sphere", sphere_v, sphere_e)
    emit_mesh(f, "wire_capsule", capsule_v, capsule_e)


if __name__ == "__main__":
    main()
