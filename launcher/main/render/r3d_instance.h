/*
 * r3d_instance: one mesh drawn through its affine model matrix, the
 * position in column 3. Its 3x3 part has a positive determinant, so no
 * triangle turns inside out. No model draws the mesh as it is. A world box,
 * when given, holds the mesh where it sits (r3d_lit_mesh_world_bounds()):
 * an instance whose box is outside the picture skips its lens and its walk.
 * A mesh with levels of detail draws the level its coarse bits pick
 * (render/lod/r3d_lod.h), which persist from frame to frame; without them it
 * draws its finest.
 */
#pragma once

#include <stdint.h>

#include "math/geometry/box3f.h"
#include "math/linear/mat4f.h"
#include "render/r3d_lit_mesh.h"

typedef struct {
    const r3d_lit_mesh_t* mesh;
    const mat4f_t* model;        /* NULL: drawn as it is */
    const box3f_t* world_bounds; /* NULL: culled from the mesh's own root */
    uint32_t* lod_coarse;        /* R3D_LOD_STATE_WORDS(mesh->lod_group_count) words, or NULL: the finest level */
} r3d_instance_t;
