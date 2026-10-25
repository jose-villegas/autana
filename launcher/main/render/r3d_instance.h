/*
 * r3d_instance: one mesh drawn through its affine model matrix, the
 * position in column 3. Its 3x3 part has a positive determinant, so no
 * triangle turns inside out. No model draws the mesh as it is.
 */
#pragma once

#include "math/linear/mat4f.h"
#include "render/r3d_lit_mesh.h"

typedef struct {
    const r3d_lit_mesh_t* mesh;
    const mat4f_t* model; /* NULL: drawn as it is */
} r3d_instance_t;
