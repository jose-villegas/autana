/* Per-vertex albedo lighting in the palette's object space. */
#pragma once
#include "render/skin/r3d_skinning.h"

#define R3D_SKIN_LIGHT_MAX                  4
#define R3D_SKIN_LIGHT_CHANNELS             3
#define R3D_SKIN_LIGHT_BYTE_SCALE           255
#define R3D_SKIN_LIGHT_DECODE_SIZE          256
#define R3D_SKIN_LIGHT_DECODE_SHIFT         16
#define R3D_SKIN_LIGHT_TRANSFER_WEIGHT_BITS 8
#define R3D_SKIN_LIGHT_TRANSFER_WEIGHT_ONE  (1U << R3D_SKIN_LIGHT_TRANSFER_WEIGHT_BITS)
#define R3D_SKIN_LIGHT_GAMMA                2.2F
#define R3D_SKIN_LIGHT_RADIANCE_BITS        4
#define R3D_SKIN_LIGHT_FLOAT_MANTISSA_BITS  23
#define R3D_SKIN_LIGHT_RADIANCE_SHIFT       (R3D_SKIN_LIGHT_FLOAT_MANTISSA_BITS - R3D_SKIN_LIGHT_RADIANCE_BITS)
#define R3D_SKIN_LIGHT_RADIANCE_MIN_EXP     (-24)
#define R3D_SKIN_LIGHT_RADIANCE_MAX_EXP     8
#define R3D_SKIN_LIGHT_RADIANCE_SIZE                                                                                   \
    ((R3D_SKIN_LIGHT_RADIANCE_MAX_EXP - R3D_SKIN_LIGHT_RADIANCE_MIN_EXP) << R3D_SKIN_LIGHT_RADIANCE_BITS)
#define R3D_SKIN_LIGHT_FLOAT_BIAS 127
#define R3D_SKIN_LIGHT_RADIANCE_FIRST                                                                                  \
    ((R3D_SKIN_LIGHT_FLOAT_BIAS + R3D_SKIN_LIGHT_RADIANCE_MIN_EXP) << R3D_SKIN_LIGHT_RADIANCE_BITS)

typedef struct {
    uint16_t decode[R3D_SKIN_LIGHT_DECODE_SIZE];
    uint8_t encode[R3D_SKIN_LIGHT_RADIANCE_SIZE];
} r3d_skin_light_transfer_t;

typedef struct {
    int count;
    vec3f_t direction[R3D_SKIN_LIGHT_MAX];
    vec3f_t colour[R3D_SKIN_LIGHT_MAX];
    vec3f_t ambient;
    const r3d_skin_light_transfer_t* transfer;
} r3d_skin_lights_t;

void r3d_skin_light_transfer_build(float tonemap_white, r3d_skin_light_transfer_t* out);
void r3d_skin_clusters_lit(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first,
                           int count, r3d_skin_output_t* posed, const r3d_skin_lights_t* lights, uint8_t (*out)[3]);
