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
#include <string.h>

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
 * a quarter, both down (between half rows) and across (within one). */

#define STEPS      (HALF_WIDTH / GFX_QUARTER_LANES)
#define SEAM_PITCH GFX_SEAM_PITCH(HALF_WIDTH)
_Static_assert(HALF_WIDTH % GFX_QUARTER_LANES == 0, "a half row is whole steps");
_Static_assert(HALF_WIDTH % GFX_SEAM_BITS == 0, "a half row is whole seam bytes");
_Static_assert(GFX_QUARTER_LANES == GFX_SEAM_BITS, "a step's keep byte is one seam byte");

/* A row's working set, big-endian, at a vector load's alignment: the blend
 * down, its neighbours one pixel left and right, and the two panel pixels
 * each one becomes. Static, beside the present task's small stack. */
static struct {
    uint16_t down[HALF_WIDTH], left[HALF_WIDTH], right[HALF_WIDTH], even[HALF_WIDTH], odd[HALF_WIDTH];
} work __attribute__((aligned(HALF_ALIGN)));

/* Half row `pixels` doubled across into `out`: each pixel's left panel pixel
 * blends a quarter toward its left neighbour and its right one toward its
 * right neighbour, unless a seam or the panel edge is there. */
static void
spread_across(const uint16_t* pixels, const uint8_t* across, gfx_color_t* out) {
    memcpy(work.left + 1, pixels, (HALF_WIDTH - 1) * sizeof(*pixels));
    memcpy(work.right, pixels + 1, (HALF_WIDTH - 1) * sizeof(*pixels));
    uint8_t keep_left[SEAM_PITCH];
    uint8_t keep_right[SEAM_PITCH];
    unsigned before = 1; /* the seam left of the first pixel: the panel edge */
    for (int byte = 0; byte < SEAM_PITCH; byte++) {
        keep_left[byte] = (uint8_t)((across[byte] << 1) | before);
        keep_right[byte] = across[byte];
        before = across[byte] >> (GFX_SEAM_BITS - 1);
    }
    keep_right[SEAM_PITCH - 1] |= 1U << ((HALF_WIDTH - 1) % GFX_SEAM_BITS); /* the right panel edge */
    gfx_quarter_select(pixels, work.left, keep_left, work.even, STEPS);
    gfx_quarter_select(pixels, work.right, keep_right, work.odd, STEPS);
    for (int x = 0; x < HALF_WIDTH; x++) {
        out[2 * x] = work.even[x];
        out[(2 * x) + 1] = work.odd[x];
    }
}

/* Bilinear at 2x, except across a seam, where each pixel keeps its own colour.
 * Out of line, so the nearest expand's caller stays as small as it was. */
static __attribute__((noinline)) void
expand_blended_rows(int y0, int y1, gfx_color_t* destination) {
    const uint8_t* seams = (const uint8_t*)(half_image + ((size_t)HALF_WIDTH * HALF_HEIGHT));
    for (int y = y0; y < y1; y++) {
        const int row = y / 2;
        const bool lower_half = y % 2 != 0;
        const int other = lower_half ? row + 1 : row - 1;
        const uint16_t* pixels = half_image + ((size_t)row * HALF_WIDTH);
        if (other >= 0 && other < HALF_HEIGHT) {
            gfx_quarter_select(pixels, half_image + ((size_t)other * HALF_WIDTH),
                               seams + ((size_t)(lower_half ? row : other) * SEAM_PITCH), work.down, STEPS);
            pixels = work.down;
        }
        spread_across(pixels, seams + ((size_t)row * SEAM_PITCH), destination + ((size_t)(y - y0) * GFX_WIDTH));
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

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
gfx_expand_checkpoint_capture(checkpoint_t* saved) {
    checkpoint_capture(saved, &frame_expanded, sizeof frame_expanded);
    checkpoint_capture(saved, &frame_blended, sizeof frame_blended);
    checkpoint_capture(saved, &frame_overlay, sizeof frame_overlay);
    checkpoint_capture(saved, half_image,
                       sizeof(*half_image) * HALF_WIDTH * HALF_HEIGHT + GFX_SEAM_BYTES(HALF_WIDTH, HALF_HEIGHT));
}
#endif
