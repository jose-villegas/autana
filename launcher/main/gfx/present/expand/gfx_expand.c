/*
 * gfx_expand: the half picture doubled to the panel, nearest or blended
 * within a surface (through the seam mask), with the frame overlay replayed
 * over each strip.
 */
#include "core/memory.h"
#include "gfx/present/expand/gfx_quarter_select.h"
#include "gfx/present/gfx_fb_guard.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present_guard.h"

#include <assert.h>

static memory_aligned_t half; /* the half picture; its seam mask follows the pixels */
static gfx_color_t* half_image;
static bool frame_expanded;
static bool frame_blended; /* expanded through the seam mask */
static GFX_TARGET_STORAGE(1) half_target = GFX_TARGET_COLOR_INIT(NULL, GFX_WIDTH / 2, GFX_HEIGHT / 2);

#define HALF_WIDTH  (GFX_WIDTH / 2)
#define HALF_HEIGHT (GFX_HEIGHT / 2)
/* Every half row starts on a vector load's 16 bytes (gfx_quarter_select.h). */
#define HALF_ALIGN  16
_Static_assert((HALF_WIDTH * sizeof(gfx_color_t)) % HALF_ALIGN == 0, "half rows stay aligned");
static gfx_band_overlay_fn frame_overlay;

gfx_color_t*
gfx_half_picture(void) {
    GFX_PRESENT_GUARD();
    if (current_mode.layout != GFX_LAYOUT_FULL_FB) {
        return NULL;
    }
    if (half_image == NULL) {
        half = memory_alloc_aligned((sizeof(*half_image) * HALF_WIDTH * HALF_HEIGHT)
                                        + GFX_SEAM_BYTES(HALF_WIDTH, HALF_HEIGHT),
                                    HALF_ALIGN, MEMORY_PSRAM);
        half_image = half.at;
        half_target.attachment[0].pixels = half_image;
    }
    return half_image;
}

uint8_t*
gfx_half_seams(void) {
    gfx_color_t* pixels = gfx_half_picture();
    return pixels == NULL ? NULL : (uint8_t*)(pixels + ((size_t)HALF_WIDTH * HALF_HEIGHT));
}

static void
expand_frame(bool blended) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_FULL_FB && half_image != NULL);
    frame_expanded = true;
    frame_blended = blended;
    gfx_fb_guard_set_available(false);
    gfx_mark_all_dirty();
}

void
gfx_expand_frame(void) {
    expand_frame(false);
}

void
gfx_expand_frame_blended(void) {
    expand_frame(true);
}

bool
gfx_frame_expanded(void) {
    return frame_expanded;
}

void
gfx_set_frame_overlay(gfx_band_overlay_fn overlay) {
    GFX_PRESENT_GUARD();
    frame_overlay = overlay;
}

gfx_band_overlay_fn
gfx_frame_overlay(void) {
    return frame_overlay;
}

void
clear_expanded_frame(bool free_picture) {
    frame_expanded = false;
    gfx_fb_guard_set_available(gfx_mode_has_framebuffer(&current_mode) && fb != NULL);
    if (free_picture) {
        memory_free(half.block);
        half = (memory_aligned_t){0};
        half_image = NULL;
        half_target.attachment[0].pixels = half_image;
    }
}

/* At 2x each panel pixel's centre sits a quarter of a half-picture pixel from
 * its own pixel's centre, towards one neighbour: bilinear gives that neighbour
 * a quarter (rgb565_spread_quarter). */

/* Seam bytes cover whole half rows, so each byte's eight pixels share one test. */
_Static_assert(HALF_WIDTH % GFX_SEAM_BITS == 0, "a half row is whole seam bytes");
#define SEAM_BYTE_ALL ((1U << GFX_SEAM_BITS) - 1)

/* One seam byte's eight pixels, each doubled across: a pixel blends a quarter
 * toward its left neighbour unless `left` (bit 0) marks a seam there, and
 * toward its right one unless `bits` does. */
static void
spread_byte(const uint32_t* column, int first, unsigned left, unsigned bits, gfx_color_t* out) {
    for (int x = first; x < first + GFX_SEAM_BITS; x++, bits >>= 1) {
        const bool right = (bits & 1U) == 0 && x + 1 < HALF_WIDTH;
        out[2 * x] = rgb565be_unspread(left == 0 ? rgb565_spread_quarter(column[x], column[x - 1]) : column[x]);
        out[(2 * x) + 1] = rgb565be_unspread(right ? rgb565_spread_quarter(column[x], column[x + 1]) : column[x]);
        left = bits & 1U;
    }
}

/* A seam byte with no seam inside or at its edges: every pixel blends both ways. */
static void
spread_byte_blended(const uint32_t* column, int first, gfx_color_t* out) {
    for (int x = first; x < first + GFX_SEAM_BITS; x++) {
        out[2 * x] = rgb565be_unspread(rgb565_spread_quarter(column[x], column[x - 1]));
        out[(2 * x) + 1] = rgb565be_unspread(rgb565_spread_quarter(column[x], column[x + 1]));
    }
}

/* A seam byte with seams all round: every pixel doubled. */
static void
spread_byte_doubled(const uint32_t* column, int first, gfx_color_t* out) {
    for (int x = first; x < first + GFX_SEAM_BITS; x++) {
        out[2 * x] = out[(2 * x) + 1] = rgb565be_unspread(column[x]);
    }
}

/* Each spread pixel doubled across, blending a quarter toward each
 * neighbour it shares a surface with. */
static void
spread_across(const uint32_t* column, const uint8_t* across, gfx_color_t* out) {
    unsigned before = 1; /* the seam left of the first pixel: the panel edge */
    for (int byte = 0; byte < HALF_WIDTH / GFX_SEAM_BITS; byte++) {
        const int first = byte * GFX_SEAM_BITS;
        const unsigned bits = across[byte];
        if (bits == SEAM_BYTE_ALL && before != 0) {
            spread_byte_doubled(column, first, out);
        } else if (bits == 0 && before == 0 && first + GFX_SEAM_BITS < HALF_WIDTH) {
            spread_byte_blended(column, first, out);
        } else {
            spread_byte(column, first, before, bits, out);
        }
        before = bits >> (GFX_SEAM_BITS - 1);
    }
}

/* The step between two half rows: one half row, big-endian, at a vector
 * store's alignment. Static, beside the present task's small stack. */
static uint16_t blended_row[HALF_WIDTH] __attribute__((aligned(HALF_ALIGN)));
_Static_assert(HALF_WIDTH % GFX_QUARTER_LANES == 0, "a half row is whole steps");

/* Bilinear at 2x, except across a seam, where each pixel keeps its own colour.
 * Out of line, so the nearest expand's caller stays as small as it was. */
static __attribute__((noinline)) void
expand_blended_rows(int y0, int y1, gfx_color_t* destination) {
    const uint8_t* seams = (const uint8_t*)(half_image + ((size_t)HALF_WIDTH * HALF_HEIGHT));
    const size_t seam_pitch = GFX_SEAM_PITCH(HALF_WIDTH);
    uint32_t column[HALF_WIDTH]; /* the row blended down, spread */
    for (int y = y0; y < y1; y++) {
        const int row = y / 2;
        const bool lower_half = y % 2 != 0;
        const int other = lower_half ? row + 1 : row - 1;
        const uint16_t* pixels = half_image + ((size_t)row * HALF_WIDTH);
        if (other >= 0 && other < HALF_HEIGHT) {
            gfx_quarter_select(pixels, half_image + ((size_t)other * HALF_WIDTH),
                               seams + ((size_t)(lower_half ? row : other) * seam_pitch), blended_row,
                               HALF_WIDTH / GFX_QUARTER_LANES);
            pixels = blended_row;
        }
        for (int x = 0; x < HALF_WIDTH; x++) {
            column[x] = rgb565be_spread(pixels[x]);
        }
        spread_across(column, seams + ((size_t)row * seam_pitch), destination + ((size_t)(y - y0) * GFX_WIDTH));
    }
}

void
expand_rows(int y0, int y1, gfx_color_t* destination) {
    GFX_TARGET_STORAGE(1) target_storage;
    gfx_target_of_color(&target_storage.target, destination, GFX_WIDTH, y1 - y0, y0);
    const gfx_target_t* target = &target_storage.target;
    if (frame_blended) {
        expand_blended_rows(y0, y1, destination);
    } else {
        gfx_target_paired_rows_shaded(target, &half_target.attachment[0], NULL, 0, NULL, y0, y1 - y0, true, 0,
                                      RGB565_SCALE_UNIT);
    }
    if (frame_overlay != NULL) {
        replay_overlay(frame_overlay, target, y0, y1);
    }
}

#ifndef ESP_PLATFORM
void
expand_reset_for_test(void) {
    clear_expanded_frame(true);
    frame_overlay = NULL;
}
#endif
