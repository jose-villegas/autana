/* Host comparison of direct and normal-indexed linear lighting. */
#pragma once
#include "math/linear/vec3f.h"
#include "render/skin/r3d_skin_light.h"

#define SKIN_LIGHT_BASELINE_WEIGHT_BITS 8
#define SKIN_LIGHT_BASELINE_WEIGHT_ONE  (1U << SKIN_LIGHT_BASELINE_WEIGHT_BITS)
#define SKIN_LIGHT_BASELINE_CELL_BYTES  4

/* Unit directions, linear color times intensity and ambient in [0,1]; output saturates. */
void skin_light_baseline_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, uint8_t out[3]);
/* Float weights allow host references to share the rotation blend without packing a SKIN record. */
vec3f_t skin_light_baseline_normal(const mat4f_t* palette, const uint8_t* joints, const float* weights, int influences,
                                   vec3f_t normal);
/* Cell directions are computed once into caller storage, outside table-build timing.
 * Grid helpers also serve the host benchmark's other resolutions. */
void skin_light_baseline_directions(int size, vec3f_t* directions);
void skin_light_baseline_grid_build(int size, const vec3f_t* directions, const r3d_skin_lights_t* lights,
                                    uint8_t (*rgb)[SKIN_LIGHT_BASELINE_CELL_BYTES]);
void skin_light_baseline_bilinear(int size, const uint8_t (*rgb)[SKIN_LIGHT_BASELINE_CELL_BYTES], vec3f_t normal,
                                  uint8_t out[3]);
