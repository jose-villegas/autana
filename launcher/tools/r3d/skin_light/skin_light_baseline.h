/* Host comparison of direct and normal-indexed linear lighting. */
#pragma once
#include "math/linear/vec3f.h"
#include "render/skin/r3d_skin_light.h"

#define R3D_SKIN_LIGHT_WEIGHT_BITS 8
#define R3D_SKIN_LIGHT_WEIGHT_ONE  (1U << R3D_SKIN_LIGHT_WEIGHT_BITS)
#define R3D_SKIN_LIGHT_CELL_BYTES  4

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
