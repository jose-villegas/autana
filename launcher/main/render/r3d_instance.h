/*
 * r3d_instance: one mesh drawn where it is placed. A placement is baked by
 * the importer as a 3x3 (rotation times scale) and a position, so the device
 * does no trigonometry; no placement means the mesh is drawn as it is.
 */
#pragma once

#include "math/linear/transformf.h"
#include "render/r3d_lit_mesh.h"

/* A point in the mesh's own units goes to the world as m * point + position.
 * The scale baked into m is positive on every axis, so no triangle turns
 * inside out. */
typedef struct {
    float m[3][3];
    vec3f_t position;
} r3d_placement_t;

static inline r3d_placement_t
r3d_placement_from(const transformf_t* pose) {
    const mat4f_t matrix = transformf_compute_matrix(pose);
    return (r3d_placement_t){{{matrix.m[0][0], matrix.m[0][1], matrix.m[0][2]},
                              {matrix.m[1][0], matrix.m[1][1], matrix.m[1][2]},
                              {matrix.m[2][0], matrix.m[2][1], matrix.m[2][2]}},
                             pose->position};
}

typedef struct {
    const r3d_lit_mesh_t* mesh;
    const r3d_placement_t* placement; /* NULL: drawn as it is */
} r3d_instance_t;
