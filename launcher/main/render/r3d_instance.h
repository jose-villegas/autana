/*
 * r3d_instance: one mesh drawn where it is placed. A placement is baked by
 * the importer as a 3x3 (rotation times scale) and a position, so the device
 * does no trigonometry; no placement means the mesh is drawn as it is.
 */
#pragma once

#include "render/r3d_lit_mesh.h"
#include "util/math/mat4f.h"

/* A point in the mesh's own units goes to the world as m * point + position.
 * The scale baked into m is positive on every axis, so no triangle turns
 * inside out. */
typedef struct {
    float m[3][3];
    vec3f_t position;
} r3d_placement_t;

typedef struct {
    const r3d_lit_mesh_t* mesh;
    const r3d_placement_t* placement; /* NULL: drawn as it is */
} r3d_instance_t;

/* Placement translation is expressed in the mesh's position ticks. */
static inline mat4f_t
r3d_placement_matrix(const r3d_placement_t* placement, int position_scale) {
    const vec3f_t p = vec3f_scale(placement->position, (float)position_scale);
    return (mat4f_t){{
        {placement->m[0][0], placement->m[0][1], placement->m[0][2], p.x},
        {placement->m[1][0], placement->m[1][1], placement->m[1][2], p.y},
        {placement->m[2][0], placement->m[2][1], placement->m[2][2], p.z},
        {0.0F, 0.0F, 0.0F, 1.0F},
    }};
}
