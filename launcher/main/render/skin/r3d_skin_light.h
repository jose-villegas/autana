/* Per-vertex albedo lighting in the palette's object space. */
#pragma once
#include <float.h>
#include "render/skin/r3d_skinning.h"

#define R3D_SKIN_LIGHT_MAX                 4
#define R3D_SKIN_LIGHT_CHANNELS            3
#define R3D_SKIN_LIGHT_BYTE_SCALE          255
#define R3D_SKIN_LIGHT_DECODE_SIZE         256
#define R3D_SKIN_LIGHT_DECODE_SHIFT        16
/* tools/r3d/light.py owns copies of these constants; test_skin_light_transfer.py gates equality until the baker reads this header. */
#define R3D_SKIN_LIGHT_GAMMA               2.2F
#define R3D_SKIN_LIGHT_FLOAT_MANTISSA_BITS (FLT_MANT_DIG - 1)
#define R3D_SKIN_LIGHT_FLOAT_BIAS          (FLT_MAX_EXP - 1)
#define R3D_SKIN_LIGHT_RADIANCE_MIN_EXP    (-16)
#define R3D_SKIN_LIGHT_RADIANCE_SPLIT_EXP  (-8)
#define R3D_SKIN_LIGHT_RADIANCE_MAX_EXP    7
#define R3D_SKIN_LIGHT_RADIANCE_LOW_BITS   2
#define R3D_SKIN_LIGHT_RADIANCE_BITS       5
#define R3D_SKIN_LIGHT_RADIANCE_LOW_SHIFT  (R3D_SKIN_LIGHT_FLOAT_MANTISSA_BITS - R3D_SKIN_LIGHT_RADIANCE_LOW_BITS)
#define R3D_SKIN_LIGHT_RADIANCE_SHIFT      (R3D_SKIN_LIGHT_FLOAT_MANTISSA_BITS - R3D_SKIN_LIGHT_RADIANCE_BITS)
#define R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE                                                                               \
    ((R3D_SKIN_LIGHT_RADIANCE_SPLIT_EXP - R3D_SKIN_LIGHT_RADIANCE_MIN_EXP) << R3D_SKIN_LIGHT_RADIANCE_LOW_BITS)
#define R3D_SKIN_LIGHT_RADIANCE_SIZE                                                                                   \
    (R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE                                                                                  \
     + ((R3D_SKIN_LIGHT_RADIANCE_MAX_EXP - R3D_SKIN_LIGHT_RADIANCE_SPLIT_EXP) << R3D_SKIN_LIGHT_RADIANCE_BITS))
#define R3D_SKIN_LIGHT_RADIANCE_LOW_FIRST                                                                              \
    ((R3D_SKIN_LIGHT_FLOAT_BIAS + R3D_SKIN_LIGHT_RADIANCE_MIN_EXP) << R3D_SKIN_LIGHT_RADIANCE_LOW_BITS)
#define R3D_SKIN_LIGHT_RADIANCE_FIRST                                                                                  \
    ((R3D_SKIN_LIGHT_FLOAT_BIAS + R3D_SKIN_LIGHT_RADIANCE_SPLIT_EXP) << R3D_SKIN_LIGHT_RADIANCE_BITS)
#define R3D_SKIN_LIGHT_RADIANCE_SPLIT                                                                                  \
    ((R3D_SKIN_LIGHT_FLOAT_BIAS + R3D_SKIN_LIGHT_RADIANCE_SPLIT_EXP) << R3D_SKIN_LIGHT_FLOAT_MANTISSA_BITS)

typedef struct {
    uint16_t decode[R3D_SKIN_LIGHT_DECODE_SIZE];
    uint8_t encode[R3D_SKIN_LIGHT_RADIANCE_SIZE];
} r3d_skin_light_transfer_t;

typedef struct {
    int count;
    vec3f_t direction[R3D_SKIN_LIGHT_MAX];
    vec3f_t color[R3D_SKIN_LIGHT_MAX];
    vec3f_t ambient;
    const r3d_skin_light_transfer_t* transfer;
} r3d_skin_lights_t;

void r3d_skin_light_transfer_build(float tonemap_white, r3d_skin_light_transfer_t* out);
void r3d_skin_clusters_lit(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first,
                           int count, r3d_skin_output_t* posed, const r3d_skin_lights_t* lights, uint8_t (*out)[3]);
