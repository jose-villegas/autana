/*
 * r3d_skin: a validated SKIN v2 mapped view; the pack outlives it.
 * docs/render/Skeleton-and-Skin.md owns the layout. No allocation.
 */
#pragma once
#include <stdint.h>
#include "asset/asset_pack.h"

#define R3D_SKIN_ID_SUFFIX          ".skin"
#define R3D_SKIN_ASSET              ASSET_TYPE('S', 'K', 'I', 'N')
#define R3D_SKIN_VERSION            2U
#define R3D_SKIN_LIGHT_DIRECT       0U
#define R3D_SKIN_LIGHT_RENORMALISED 1U
#define R3D_SKIN_LIGHT_TABLE        2U
#define R3D_SKIN_WEIGHT_SUM         255U
#define R3D_SKIN_NORMAL_SCALE       127
#define R3D_SKIN_WEIGHT_RECIP       (1.0F / (float)R3D_SKIN_WEIGHT_SUM)
#define R3D_SKIN_NORMAL_RECIP       (1.0F / (float)R3D_SKIN_NORMAL_SCALE)

enum {
    R3D_SKIN_HEADER_SIZE = 20,
    R3D_SKIN_AT_LIGHTING_MODE = 16,
    R3D_SKIN_AT_PAD = 17,
    R3D_SKIN_AT_VERSION = 0,
    R3D_SKIN_AT_JOINTS = 2,
    R3D_SKIN_AT_INFLUENCES = 3,
    R3D_SKIN_AT_VERTICES = 4,
    R3D_SKIN_AT_INVERSE = 8,
    R3D_SKIN_AT_RECORDS = 12,
    R3D_SKIN_ALIGNMENT = 4,
    R3D_SKIN_MATRIX_WIDTH = 12,
    R3D_SKIN_INFLUENCES_MIN = 2,
    R3D_SKIN_INFLUENCES_MAX = 4,
    R3D_SKIN_NORMAL_WIDTH = 3,
    R3D_SKIN_RECORD_TAIL = R3D_SKIN_NORMAL_WIDTH + 1,
};

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
