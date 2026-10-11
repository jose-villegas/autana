/* Allocation-free skinned-normal lighting; palettes and lights share the normal's space. */
#pragma once
#include "math/linear/vec3f.h"
#include "render/skin/r3d_skinning.h"

#define R3D_SKIN_LIGHT_MAX         4
#define R3D_SKIN_LIGHT_TABLE_SIZE  16
#define R3D_SKIN_LIGHT_BYTE_SCALE  255
#define R3D_SKIN_LIGHT_WEIGHT_BITS 8
#define R3D_SKIN_LIGHT_WEIGHT_ONE  (1U << R3D_SKIN_LIGHT_WEIGHT_BITS)
#define R3D_SKIN_LIGHT_CELL_BYTES  4
#define R3D_SKIN_LIGHT_CHANNELS    3

typedef struct {
    int count;
    vec3f_t direction[R3D_SKIN_LIGHT_MAX];
    vec3f_t colour[R3D_SKIN_LIGHT_MAX];
    vec3f_t ambient;
} r3d_skin_lights_t;

typedef struct {
    uint8_t rgb[R3D_SKIN_LIGHT_TABLE_SIZE * R3D_SKIN_LIGHT_TABLE_SIZE][R3D_SKIN_LIGHT_CELL_BYTES];
} r3d_skin_light_table_t;

/* Unit directions, linear colour times intensity and ambient in [0,1]; output saturates. */
void r3d_skin_light_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, uint8_t out[3]);
/* Float weights allow host references to share the rotation blend without packing a SKIN record. */
vec3f_t r3d_skin_light_normal(const mat4f_t* palette, const uint8_t* joints, const float* weights, int influences,
                              vec3f_t normal);
/* Cell directions are computed once into caller storage, outside table-build timing.
 * Grid helpers also serve the host benchmark's other resolutions. */
void r3d_skin_light_directions(int size, vec3f_t* directions);
void r3d_skin_light_grid_build(int size, const vec3f_t* directions, const r3d_skin_lights_t* lights,
                               uint8_t (*rgb)[R3D_SKIN_LIGHT_CELL_BYTES]);
void r3d_skin_light_bilinear(int size, const uint8_t (*rgb)[R3D_SKIN_LIGHT_CELL_BYTES], vec3f_t normal, uint8_t out[3]);
void r3d_skin_light_build(const vec3f_t* directions, const r3d_skin_lights_t* lights, r3d_skin_light_table_t* table);
/* first/count select clusters; output uses absolute vertex indexes, like skin_clusters. */
void r3d_skin_light_direct(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first,
                           int count, const r3d_skin_lights_t* lights, bool renormalise, uint8_t (*out)[3]);
void r3d_skin_light_table(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first,
                          int count, const r3d_skin_light_table_t* table, uint8_t (*out)[3]);
