/*
 * gfx_dither - screen-fixed ordered thresholds for coverage and quantising.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx_dither_patterns_generated.h"

#define GFX_DITHER_MAX_LEVELS 1024

typedef enum {
    GFX_DITHER_BAYER2,
    GFX_DITHER_BAYER4,
    GFX_DITHER_BAYER8,
    GFX_DITHER_BAYER4_2PX,
    GFX_DITHER_CLUSTER4,
    GFX_DITHER_CLUSTER8,
    GFX_DITHER_BLUE32,
    GFX_DITHER_SCANLINES4,
    GFX_DITHER_HATCH4,
    GFX_DITHER_PATTERN_COUNT,
} gfx_dither_pattern_id_t;

typedef struct {
    const uint16_t* ranks;
    uint16_t width, height;
    uint8_t scale_x, scale_y;
    uint16_t levels;
} gfx_dither_pattern_t;

static inline int
gfx_dither_bayer_rank(int x, int y, int bits) {
    static const uint8_t cell[2][2] = {{0, 2}, {3, 1}};
    int rank = 0;
    for (int bit = bits - 1; bit >= 0; bit--) {
        rank = rank * 4 + cell[(y >> bit) & 1][(x >> bit) & 1];
    }
    return rank;
}

static inline const gfx_dither_pattern_t*
gfx_dither_pattern(gfx_dither_pattern_id_t id) {
    static const gfx_dither_pattern_t patterns[GFX_DITHER_PATTERN_COUNT] = {
        [GFX_DITHER_BAYER2] = {NULL, 2, 2, 1, 1, 4},
        [GFX_DITHER_BAYER4] = {NULL, 4, 4, 1, 1, 16},
        [GFX_DITHER_BAYER8] = {NULL, 8, 8, 1, 1, 64},
        [GFX_DITHER_BAYER4_2PX] = {NULL, 4, 4, 2, 2, 16},
        [GFX_DITHER_CLUSTER4] = {gfx_dither_cluster4, 4, 4, 1, 1, 16},
        [GFX_DITHER_CLUSTER8] = {gfx_dither_cluster8, 8, 8, 1, 1, 64},
        [GFX_DITHER_BLUE32] = {gfx_dither_blue32, 32, 32, 1, 1, 1024},
        [GFX_DITHER_SCANLINES4] = {NULL, 1, 4, 1, 1, 4},
        [GFX_DITHER_HATCH4] = {NULL, 4, 1, 1, 1, 4},
    };
    return id >= 0 && id < GFX_DITHER_PATTERN_COUNT ? &patterns[id] : NULL;
}

static inline int
gfx_dither_threshold(gfx_dither_pattern_id_t id, int x, int y) {
    const gfx_dither_pattern_t* const pattern = gfx_dither_pattern(id);
    if (pattern == NULL) {
        return 0;
    }
    const int cell_x = (x / pattern->scale_x) & (pattern->width - 1);
    const int cell_y = (y / pattern->scale_y) & (pattern->height - 1);
    if (pattern->ranks != NULL) {
        return pattern->ranks[cell_y * pattern->width + cell_x];
    }
    switch (id) {
        case GFX_DITHER_BAYER2: return gfx_dither_bayer_rank(cell_x, cell_y, 1);
        case GFX_DITHER_BAYER4:
        case GFX_DITHER_BAYER4_2PX: return gfx_dither_bayer_rank(cell_x, cell_y, 2);
        case GFX_DITHER_BAYER8: return gfx_dither_bayer_rank(cell_x, cell_y, 3);
        case GFX_DITHER_SCANLINES4: return (int[]){0, 2, 1, 3}[cell_y];
        case GFX_DITHER_HATCH4: return (cell_x * 3) & 3;
        default: return 0;
    }
}

static inline bool
gfx_dither_alpha_pick(gfx_dither_pattern_id_t id, int x, int y, uint8_t alpha) {
    const gfx_dither_pattern_t* const pattern = gfx_dither_pattern(id);
    return alpha != 0 && pattern != NULL
           && (alpha == 255 || (int)alpha * pattern->levels / 256 > gfx_dither_threshold(id, x, y));
}
