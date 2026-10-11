/* Linear blend skinning into caller-owned internal RAM; bind colours are retained. */
#pragma once
#include <stddef.h>
#include "math/linear/mat4f.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_skin.h"

typedef struct {
    int16_t (*positions)[3];
    r3d_lit_cluster_t* clusters;
    r3d_lit_node_t* nodes;
    r3d_lit_mesh_t mesh;
} r3d_skin_output_t;

size_t r3d_skin_output_bytes(const r3d_lit_mesh_t* bind);
/* block has malloc alignment and at least output_bytes bytes; the caller allocates internal RAM. */
bool r3d_skin_output_init(const r3d_lit_mesh_t* bind, void* block, size_t bytes, r3d_skin_output_t* out);
/* Palette maps bind position ticks to posed position ticks. */
void r3d_skin_palette(const r3d_skin_t* skin, const mat4f_t* model, int position_scale, mat4f_t* palette);
void r3d_skin_clusters(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
                       r3d_skin_output_t* out);
void r3d_skin_refit_nodes(r3d_skin_output_t* out);
