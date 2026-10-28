#include "draw/icon.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/mathi.h"

/* The ink's own bounds, not the declared size: a glyph rarely fills its grid,
 * so centring the grid would off-centre what is drawn. False when nothing is
 * inked. */
static bool
ink_box(const image_t* icon, recti_t* out) {
    *out = (recti_t){0};
    const size_t step = icon->pitch;
    const uint8_t* row = icon->pixels;
    for (int y = 0; y < icon->height; y++, row += step) {
        for (int x = 0; x < icon->width; x++) {
            if (image_mono1_ink(row, x)) {
                *out = recti_union(*out, (recti_t){x, y, 1, 1});
            }
        }
    }
    return !recti_is_empty(*out);
}

/* Each run of ink in `row`, `width` pixels, scaled and placed at (x0, y0). */
static void
emit_runs(const uint8_t* row, int width, int x0, int y0, int scale, icon_emit_fn emit, void* ctx) {
    int x = 0;
    while (x < width) {
        if (!image_mono1_ink(row, x)) {
            x++;
            continue;
        }
        const int run_start = x;
        while (x < width && image_mono1_ink(row, x)) {
            x++;
        }
        emit(ctx, (recti_t){x0 + (run_start * scale), y0, (x - run_start) * scale, scale});
    }
}

void
icon_walk_blocks(const image_t* icon, int box_w, int box_h, icon_emit_fn emit, void* ctx) {
    assert(icon->format == IMAGE_MONO1);
    const int scale = mathi_max(1, mathi_min(box_w / icon->width, box_h / icon->height));
    recti_t box;
    if (!ink_box(icon, &box)) {
        return;
    }
    const int origin_x = ((box_w - (box.w * scale)) / 2) - (box.x * scale);
    const int origin_y = ((box_h - (box.h * scale)) / 2) - (box.y * scale);
    const size_t step = icon->pitch;
    const uint8_t* row = icon->pixels;
    for (int y = 0; y < icon->height; y++, row += step) {
        emit_runs(row, icon->width, origin_x, origin_y + (y * scale), scale, emit, ctx);
    }
}
