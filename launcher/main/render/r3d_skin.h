/*
 * r3d_skin: a validated SKIN v2 mapped view; the pack outlives it.
 * docs/assets/formats/SKIN.md owns the layout. No allocation.
 */
#pragma once
#include <stdint.h>
#include "asset/asset_pack.h"
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

asset_status_t r3d_skin_open(const asset_pack_t* pack, const char* id, r3d_skin_t* out);
