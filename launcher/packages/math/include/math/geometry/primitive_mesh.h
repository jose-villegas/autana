/*
 * primitive_mesh: standard shapes as indexed triangle meshes, for geometry
 * made at run time: a square grid and a latitude-longitude sphere, each one
 * unit in size and tessellated by one count. Each triangle winds
 * counter-clockwise about its outward normal in a right-handed frame, as
 * iso_surface.h winds its own: (b - a) x (c - a) points out of the front.
 *
 * The caller owns the buffers; a shape that does not fit them, or needs
 * more vertices than a uint16 index reaches, writes nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/vec3f.h"

typedef struct {
    vec3f_t* positions;
    int vertex_capacity;
    uint16_t (*triangles)[3];
    int triangle_capacity;
    int vertex_count, triangle_count;
} primitive_mesh_t;

/* The fewest bands a sphere has: one cap of triangles round each pole. */
#define PRIMITIVE_SPHERE_RINGS_MIN 2

/* A grid of `cells` by `cells` squares, each cut in two. */
static inline int
primitive_grid_vertex_count(int cells) {
    return (cells + 1) * (cells + 1);
}

static inline int
primitive_grid_triangle_count(int cells) {
    return 2 * cells * cells;
}

/* `rings` bands from pole to pole, each cut into twice as many segments
 * round the axis. The seam's and each pole's vertices are repeated, one per
 * segment, so every vertex has one latitude and one longitude. */
static inline int
primitive_sphere_segments(int rings) {
    return 2 * rings;
}

static inline int
primitive_sphere_vertex_count(int rings) {
    return (rings + 1) * (primitive_sphere_segments(rings) + 1);
}

/* The caps' bands have one triangle per segment, the others two. */
static inline int
primitive_sphere_triangle_count(int rings) {
    return 2 * primitive_sphere_segments(rings) * (rings - 1);
}

/* The unit square from (-0.5, -0.5) to (0.5, 0.5) at z = 0, facing +z, as a
 * grid of `cells` by `cells` squares. False, writing nothing, for fewer than
 * one cell or a grid the buffers or uint16 indices cannot hold. */
bool primitive_grid(int cells, primitive_mesh_t* out);

/* The unit sphere about the origin, `rings` bands from +y to -y, facing
 * out. False, writing nothing, for fewer than PRIMITIVE_SPHERE_RINGS_MIN
 * rings or a sphere the buffers or uint16 indices cannot hold. */
bool primitive_sphere(int rings, primitive_mesh_t* out);
