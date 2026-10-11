/* Per-vertex albedo lighting in world space. */
#pragma once
#include <float.h>
#include "math/linear/mat3f.h"
#include "render/skin/r3d_skinning.h"

#define R3D_SKIN_LIGHT_MAX 4

enum { R3D_SKIN_LIGHT_CHANNELS = 3 };

#define R3D_SKIN_LIGHT_BYTE_SCALE          255
#define R3D_SKIN_LIGHT_DECODE_SIZE         256
#define R3D_SKIN_LIGHT_DECODE_SHIFT        16
/* test_skin_light_transfer.py checks these transfer constants against tools/r3d/light.py. */
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

#define R3D_SKIN_LIGHT_TABLE_SIDE        16
#define R3D_SKIN_LIGHT_TABLE_CELLS       (R3D_SKIN_LIGHT_TABLE_SIDE * R3D_SKIN_LIGHT_TABLE_SIDE)
#define R3D_SKIN_LIGHT_TABLE_BITS        8
#define R3D_SKIN_LIGHT_TABLE_WEIGHT_BITS 8
#define R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE  (1 << R3D_SKIN_LIGHT_TABLE_WEIGHT_BITS)
#define R3D_SKIN_LIGHT_TABLE_MASK        ((1U << R3D_SKIN_LIGHT_TABLE_BITS) - 1U)

/* Keep reciprocal quantisation and interpolated irradiance in the finite float range. */
#define R3D_SKIN_LIGHT_IRRADIANCE_MIN    (FLT_MIN * R3D_SKIN_LIGHT_TABLE_MASK)
#define R3D_SKIN_LIGHT_IRRADIANCE_MAX    (FLT_MAX / R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE)

typedef struct {
    uint32_t cells[R3D_SKIN_LIGHT_TABLE_CELLS];
} r3d_skin_light_table_t;

typedef struct {
    int count;
    vec3f_t direction[R3D_SKIN_LIGHT_MAX];
    vec3f_t color[R3D_SKIN_LIGHT_MAX];
    vec3f_t ambient;
    const r3d_skin_light_table_t* table;
    float table_scale;
    const r3d_skin_light_transfer_t* transfer;
} r3d_skin_lights_t;

/* False for a singular or non-finite normal transform; the caller skips lighting. */
bool r3d_skin_normal_palette(const mat4f_t* palette, int joints, mat4f_t world, mat3f_t* normals);
void r3d_skin_light_transfer_build(float tonemap_white, r3d_skin_light_transfer_t* out);
void r3d_skin_clusters_lit(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first,
                           int count, r3d_skin_output_t* posed, const mat3f_t* normals, const r3d_skin_lights_t* lights,
                           uint8_t (*out)[3]);

float r3d_skin_light_table_build(const r3d_skin_lights_t* lights, r3d_skin_light_table_t* out);
vec3f_t r3d_skin_light_table_sample(vec3f_t normal, const r3d_skin_lights_t* lights);
typedef void (*r3d_skin_light_kernel_t)(const r3d_skin_t*, const r3d_lit_mesh_t*, const mat4f_t*, int, int,
                                        r3d_skin_output_t*, const mat3f_t*, const r3d_skin_lights_t*, uint8_t (*)[3]);
void r3d_skin_clusters_renormalised(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette,
                                    int first, int count, r3d_skin_output_t* out, const mat3f_t* normals,
                                    const r3d_skin_lights_t* lights, uint8_t (*light)[3]);
void r3d_skin_clusters_table(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first,
                             int count, r3d_skin_output_t* out, const mat3f_t* normals, const r3d_skin_lights_t* lights,
                             uint8_t (*light)[3]);
