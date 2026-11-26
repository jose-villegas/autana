/*
 * r3d_skin: a validated SKIN v2 mapped view; the pack outlives it.
 * docs/assets/formats/SKIN.md owns the layout. No allocation.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "asset/asset_pack.h"
#include "math/linear/mat4f.h"
#include "render/r3d_skin_format.h"

#define R3D_SKIN_ID_SUFFIX ".skin"

typedef struct {
    uint8_t joint_count;
    uint8_t influences;
    uint8_t lighting_mode;
    uint32_t vertex_count;
    const float (*inverse_binds)[R3D_SKIN_MATRIX_WIDTH];
    const uint8_t* vertices;
    uint8_t vertex_stride;
} r3d_skin_t;

static inline __attribute__((always_inline)) float
r3d_skin_weight(const uint8_t* record, int influences, int i) {
    return (float)record[influences + i] * R3D_SKIN_WEIGHT_RECIP;
}

static inline mat4f_t
r3d_skin_inverse_bind(const r3d_skin_t* skin, int joint) {
    mat4f_t inverse = mat4f_identity();
    memcpy(inverse.m, skin->inverse_binds[joint], sizeof skin->inverse_binds[joint]);
    return inverse;
}

asset_status_t r3d_skin_open(const asset_pack_t* pack, const char* id, r3d_skin_t* out);
