#include "gfx/draw/icon.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/mathi.h"

/* The inked part of an icon, inclusive. */
typedef struct {
    int min_x, max_x, min_y, max_y;
} ink_box_t;

static bool
ink(const uint8_t* row, int x) {
    return (row[x / 8] & (uint8_t)(0x80U >> (x % 8))) != 0;
}

/* Row `y`: a MONO1 stride is whole bytes. */
static const uint8_t*
row_at(const gfx_image_t* icon, int y) {
    return icon->bits + ((size_t)y * (icon->stride / 8U));
}

/* The ink's own bounds, not the declared size: a glyph rarely fills its grid,
 * so centring the grid would off-centre what is drawn. False when nothing is
 * inked. */
static bool
ink_box(const gfx_image_t* icon, ink_box_t* out) {
    *out = (ink_box_t){icon->width, -1, icon->height, -1};
    for (int y = 0; y < icon->height; y++) {
        const uint8_t* row = row_at(icon, y);
        for (int x = 0; x < icon->width; x++) {
            if (ink(row, x)) {
                *out = (ink_box_t){mathi_min(x, out->min_x), mathi_max(x, out->max_x), mathi_min(y, out->min_y),
                                   mathi_max(y, out->max_y)};
            }
        }
    }
    return out->max_x >= 0;
}

/* Each run of ink in `row`, `width` pixels, scaled and placed at (x0, y0). */
static void
emit_runs(const uint8_t* row, int width, int x0, int y0, int scale, icon_emit_fn emit, void* ctx) {
    int x = 0;
    while (x < width) {
        if (!ink(row, x)) {
            x++;
            continue;
        }
        const int run_start = x;
        while (x < width && ink(row, x)) {
            x++;
        }
        emit(ctx, x0 + (run_start * scale), y0, (x - run_start) * scale, scale);
    }
}

void
icon_walk_blocks(const gfx_image_t* icon, int box_w, int box_h, icon_emit_fn emit, void* ctx) {
    assert(icon->format == GFX_IMAGE_MONO1);
    const int scale = mathi_max(1, mathi_min(box_w / icon->width, box_h / icon->height));
    ink_box_t box;
    if (!ink_box(icon, &box)) {
        return;
    }
    const int origin_x = ((box_w - ((box.max_x - box.min_x + 1) * scale)) / 2) - (box.min_x * scale);
    const int origin_y = ((box_h - ((box.max_y - box.min_y + 1) * scale)) / 2) - (box.min_y * scale);
    for (int y = 0; y < icon->height; y++) {
        emit_runs(row_at(icon, y), icon->width, origin_x, origin_y + (y * scale), scale, emit, ctx);
    }
}
