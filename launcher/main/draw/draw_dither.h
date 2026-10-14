/*
 * draw_dither: screen-fixed ordered thresholds for coverage and quantising.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "draw/draw_dither_patterns_generated.h"

#define DRAW_DITHER_MAX_LEVELS 1024

typedef enum {
    DRAW_DITHER_BAYER2,
    DRAW_DITHER_BAYER4,
    DRAW_DITHER_BAYER8,
    DRAW_DITHER_BAYER4_2PX,
    DRAW_DITHER_CLUSTER4,
    DRAW_DITHER_CLUSTER8,
    DRAW_DITHER_BLUE32,
    DRAW_DITHER_SCANLINES4,
    DRAW_DITHER_HATCH4,
    DRAW_DITHER_PATTERN_COUNT,
} draw_dither_pattern_id_t;

typedef struct {
    const uint16_t* ranks;
    uint16_t width, height;
    uint8_t scale_x, scale_y;
    uint16_t levels;
} draw_dither_pattern_t;

static inline int
draw_dither_bayer_rank(int x, int y, int bits) {
    static const uint8_t cell[2][2] = {{0, 2}, {3, 1}};
    int rank = 0;
    for (int bit = bits - 1; bit >= 0; bit--) {
        rank = rank * 4 + cell[(y >> bit) & 1][(x >> bit) & 1];
    }
    return rank;
}

static inline const draw_dither_pattern_t*
draw_dither_pattern(draw_dither_pattern_id_t id) {
    static const draw_dither_pattern_t patterns[DRAW_DITHER_PATTERN_COUNT] = {
        [DRAW_DITHER_BAYER2] = {NULL, 2, 2, 1, 1, 4},
        [DRAW_DITHER_BAYER4] = {NULL, 4, 4, 1, 1, 16},
        [DRAW_DITHER_BAYER8] = {NULL, 8, 8, 1, 1, 64},
        [DRAW_DITHER_BAYER4_2PX] = {NULL, 4, 4, 2, 2, 16},
        [DRAW_DITHER_CLUSTER4] = {draw_dither_cluster4, 4, 4, 1, 1, 16},
        [DRAW_DITHER_CLUSTER8] = {draw_dither_cluster8, 8, 8, 1, 1, 64},
        [DRAW_DITHER_BLUE32] = {draw_dither_blue32, 32, 32, 1, 1, DRAW_DITHER_MAX_LEVELS},
        [DRAW_DITHER_SCANLINES4] = {NULL, 1, 4, 1, 1, 4},
        [DRAW_DITHER_HATCH4] = {NULL, 4, 1, 1, 1, 4},
    };
    return id >= 0 && id < DRAW_DITHER_PATTERN_COUNT ? &patterns[id] : NULL;
}

static inline int
draw_dither_threshold(draw_dither_pattern_id_t id, int x, int y) {
    const draw_dither_pattern_t* const pattern = draw_dither_pattern(id);
    if (pattern == NULL) {
        return 0;
    }
    const int cell_x = (x / pattern->scale_x) & (pattern->width - 1);
    const int cell_y = (y / pattern->scale_y) & (pattern->height - 1);
    if (pattern->ranks != NULL) {
        return pattern->ranks[(cell_y * pattern->width) + cell_x];
    }
    switch (id) {
        case DRAW_DITHER_BAYER2: return draw_dither_bayer_rank(cell_x, cell_y, 1);
        case DRAW_DITHER_BAYER4:
        case DRAW_DITHER_BAYER4_2PX: return draw_dither_bayer_rank(cell_x, cell_y, 2);
        case DRAW_DITHER_BAYER8: return draw_dither_bayer_rank(cell_x, cell_y, 3);
        case DRAW_DITHER_SCANLINES4: return (int[]){0, 2, 1, 3}[cell_y];
        case DRAW_DITHER_HATCH4: return (cell_x * 3) & 3;
        default: return 0;
    }
}

static inline bool
draw_dither_alpha_pick(draw_dither_pattern_id_t id, int x, int y, uint8_t alpha) {
    const draw_dither_pattern_t* const pattern = draw_dither_pattern(id);
    return alpha != 0 && pattern != NULL
           && (alpha == 255 || (int)alpha * pattern->levels / 256 > draw_dither_threshold(id, x, y));
}
