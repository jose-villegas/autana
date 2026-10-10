#include "gfx/draw/icon.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool
ink(const uint8_t* row, int x) {
    return (row[x / 8] & (uint8_t)(0x80U >> (x % 8))) != 0;
}

/* Row `y`: a MONO1 stride is whole bytes. */
static const uint8_t*
row_at(const gfx_image_t* icon, int y) {
    return icon->bits + ((size_t)y * (icon->stride / 8U));
}

void
icon_walk_blocks(const gfx_image_t* icon, int box_w, int box_h, icon_emit_fn emit, void* ctx) {
    assert(icon->format == GFX_IMAGE_MONO1);
    const int iw = icon->width;
    const int ih = icon->height;
    const int scale_w = box_w / iw;
    const int scale_h = box_h / ih;
    int scale = (scale_w < scale_h) ? scale_w : scale_h;
    if (scale < 1) {
        scale = 1;
    }

    /* The ink's own bounds, not the declared size: a glyph rarely fills its
     * grid, so centring the grid would off-centre what is drawn. */
    int min_x = iw, max_x = -1;
    int min_y = ih, max_y = -1;
    for (int y = 0; y < ih; y++) {
        const uint8_t* row = row_at(icon, y);
        for (int x = 0; x < iw; x++) {
            if (ink(row, x)) {
                min_x = x < min_x ? x : min_x;
                max_x = x > max_x ? x : max_x;
                min_y = y < min_y ? y : min_y;
                max_y = y > max_y ? y : max_y;
            }
        }
    }
    if (max_x < 0) {
        return;
    }

    const int origin_x = (box_w - (max_x - min_x + 1) * scale) / 2 - min_x * scale;
    const int origin_y = (box_h - (max_y - min_y + 1) * scale) / 2 - min_y * scale;
    for (int y = 0; y < ih; y++) {
        const uint8_t* row = row_at(icon, y);
        int x = 0;
        while (x < iw) {
            if (!ink(row, x)) {
                x++;
                continue;
            }
            const int run_start = x;
            while (x < iw && ink(row, x)) {
                x++;
            }
            emit(ctx, origin_x + run_start * scale, origin_y + y * scale, (x - run_start) * scale, scale);
        }
    }
}
