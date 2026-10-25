/*
 * iso_surface: the surface where a sampled scalar field crosses a level,
 * as a triangle mesh, by naive surface nets (Gibson): one vertex per cell
 * the surface passes through, at the mean of its edge crossings, and a quad
 * across every sample edge that crosses. Smoother than marching cubes for
 * the same samples, with no case table.
 *
 * Inside is above the level. Each triangle winds counter-clockwise about its
 * outward normal in a right-handed frame: (b - a) x (c - a) points from
 * inside to outside. A field that is below the level all round its border
 * gives a closed surface.
 *
 * The caller owns every buffer; the scratch holds two slabs of cell
 * indices, so memory does not grow with the field's length along x.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/linear/vec3f.h"

typedef struct {
    const float* values; /* indexed x * ny * nz + y * nz + z */
    int nx, ny, nz;      /* samples per axis, at least 2 each */
    vec3f_t origin;      /* where sample (0, 0, 0) sits */
    vec3f_t step;        /* sample spacing per axis */
} iso_field_t;

typedef struct {
    vec3f_t* positions;
    vec3f_t* normals; /* unit, outward: down the field's gradient */
    int vertex_capacity;
    uint16_t (*triangles)[3];
    int triangle_capacity;
    int32_t* scratch; /* iso_surface_scratch_count() entries */
    int vertex_count, triangle_count;
} iso_mesh_t;

/* The scratch a field of these sizes needs. */
static inline size_t
iso_surface_scratch_count(int ny, int nz) {
    return 2U * (size_t)(ny - 1) * (size_t)(nz - 1);
}

/* Meshes `field` at `level` into `out`, replacing what it held. False when
 * a capacity ran out (what fitted is kept, every triangle whole) or the
 * field is smaller than 2 samples on an axis. */
bool iso_surface_nets(const iso_field_t* field, float level, iso_mesh_t* out);
