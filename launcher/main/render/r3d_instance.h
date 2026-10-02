/*
 * r3d_instance: one mesh drawn where it is placed. A placement is baked by
 * the importer as a 3x3 (rotation times scale) and a position, so the device
 * does no trigonometry; no placement means the mesh is drawn as it is.
 */
#pragma once

#include "render/r3d_lit_mesh.h"
#include "util/math/vec3f.h"

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
