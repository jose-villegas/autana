/*=============================================================================
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen/gen_icons.py main/apps/sand/icons/dither.png main/apps/sand/icons/dither.json > main/apps/sand/icons_dither.h
 *
 * Baked from main/apps/sand/icons/dither.png (4x4 cells) - see gfx/icon.h for
 * icon_t's own fields and tools/gen/gen_icons.py for the PNG/SVG decode,
 * validation and packing this table was produced by.
 *===========================================================================*/
#pragma once

#include <stdint.h>

#include "gfx/icon.h"

typedef enum {
    ICON_DITHER_NONE,
    ICON_DITHER_CELL_CHECKER,
    ICON_DITHER_CELL_BAYER2,
    ICON_DITHER_PIXEL_CHECKER2,
    ICON_DITHER_PIXEL_BAYER4,
    ICON_DITHER_COUNT
} icon_dither_id_t;

static const uint8_t icon_dither_rows[20] = {
    0xF0, 0xF0, 0xF0, 0xF0, 0xC0, 0xC0, 0x30, 0x30, 0xC0, 0xC0, 0x00, 0x00, 0xA0, 0x50, 0xA0, 0x50, 0xA0, 0x00, 0xA0, 0x00,
};

static const icon_t icon_dither_table[ICON_DITHER_COUNT] = {
    [ICON_DITHER_NONE] = { .offset = 0, .w = 4, .h = 4, .stride = 1, .blocks = 4 },
    [ICON_DITHER_CELL_CHECKER] = { .offset = 4, .w = 4, .h = 4, .stride = 1, .blocks = 4 },
    [ICON_DITHER_CELL_BAYER2] = { .offset = 8, .w = 4, .h = 4, .stride = 1, .blocks = 2 },
    [ICON_DITHER_PIXEL_CHECKER2] = { .offset = 12, .w = 4, .h = 4, .stride = 1, .blocks = 8 },
    [ICON_DITHER_PIXEL_BAYER4] = { .offset = 16, .w = 4, .h = 4, .stride = 1, .blocks = 4 },
};

/* Rects every icon here emits if all are drawn once - a
 * command-list cost, not just a count. */
#define ICON_DITHER_TOTAL_BLOCKS 22

/* Pins this table against its own blob, so a bad offset or
 * stride is a compile error where the header is included rather
 * than a wrong glyph at draw time. */
_Static_assert(sizeof icon_dither_rows == 20,
               "icon_dither_rows was rebaked without its offsets");
_Static_assert(0 + 4 * 1 <= (int)sizeof icon_dither_rows,
               "icon none runs past the end of icon_dither_rows");
_Static_assert(1 == (4 + 7) / 8,
               "icon none stride does not match its width");
_Static_assert(4 + 4 * 1 <= (int)sizeof icon_dither_rows,
               "icon cell_checker runs past the end of icon_dither_rows");
_Static_assert(1 == (4 + 7) / 8,
               "icon cell_checker stride does not match its width");
_Static_assert(8 + 4 * 1 <= (int)sizeof icon_dither_rows,
               "icon cell_bayer2 runs past the end of icon_dither_rows");
_Static_assert(1 == (4 + 7) / 8,
               "icon cell_bayer2 stride does not match its width");
_Static_assert(12 + 4 * 1 <= (int)sizeof icon_dither_rows,
               "icon pixel_checker2 runs past the end of icon_dither_rows");
_Static_assert(1 == (4 + 7) / 8,
               "icon pixel_checker2 stride does not match its width");
_Static_assert(16 + 4 * 1 <= (int)sizeof icon_dither_rows,
               "icon pixel_bayer4 runs past the end of icon_dither_rows");
_Static_assert(1 == (4 + 7) / 8,
               "icon pixel_bayer4 stride does not match its width");
