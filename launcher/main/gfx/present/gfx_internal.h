/*
 * gfx_internal: state and calls shared only by gfx/present/ sources.
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/modes/band/gfx_band.h"
#include "gfx/present/send/gfx_dirty.h"
#include "gfx/target/gfx_target.h"

#ifdef ESP_PLATFORM
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

/* gfx_draw_begin()'s sources. What draw writes into: the framebuffer, or the band or replay strip being drawn. */
const gfx_target_t* gfx_present_target(void);
/* True while a band or an overlay replay is drawn; draw marks nothing dirty then. */
bool gfx_present_in_band(void);
/* Save and restore the clip verbatim, including empty boxes. Restore does not
 * clamp; callers must supply a saved clip or a screen-intersected box. */
recti_t gfx_clip_save(void);
void gfx_clip_restore(recti_t clip);

/* gfx_mode.c: the framebuffer and the mode it is held in. */
extern gfx_color_t* fb;
extern gfx_color_t* present_fb;
void gfx_swap_framebuffers(void);
/* Whether fb holds only fb_fill_colour, as a send that cleared it left it.
 * Core 0's fact: gfx_draw_begin() clears it only when set, so a core-1
 * overlay replay (single layout, never set) does not write it. */
extern bool fb_filled;
extern gfx_color_t fb_fill_colour;

/* Waits for the send task before changing its source or reading back. */
int32_t gfx_present_drain(void);
/* Whether the last send filled present_fb after sending it (gfx_set_clear_on_send()). */
bool gfx_present_sent_frame_filled(void);
#ifndef ESP_PLATFORM
/* gfx_reset_for_test(): the next test starts with no frame cleared on send. */
void gfx_present_fill_reset_for_test(void);
#endif
extern gfx_mode_t current_mode;

bool alloc_full_framebuffer(void);
/* A frame overlay drawn over rows [y0, y1) of `target`, through the band
 * strip, as band mode draws one. */
void replay_overlay(gfx_band_overlay_fn overlay, const gfx_target_t* target, int y0, int y1);

/* modes/indexed/gfx_indexed.c: the index image and its tables. Begin takes
 * the granted grid (false when its image cannot be allocated); end frees it. */
bool indexed_begin(const gfx_mode_t* granted);
void indexed_end(void);
gfx_indexed_frame_t indexed_frame(void);

/* expand/gfx_expand.c: an expanded frame, the half picture doubled into rows
 * [y0, y1), with the frame overlay replayed over them; and its end, after
 * the present. */
void expand_rows(int y0, int y1, gfx_color_t* destination);
void clear_expanded_frame(bool free_picture);
#ifndef ESP_PLATFORM
/* gfx_reset_for_test(): no expanded frame, picture or overlay. */
void expand_reset_for_test(void);
#endif

/* True for the whole GFX_LAYOUT_BANDS mode. */
static inline bool
band_is_transient(void) {
    return current_mode.layout == GFX_LAYOUT_BANDS;
}

/* Presentation state band mode reads. */
extern bool interlace_on;

#ifdef ESP_PLATFORM
/* The panel link, and the send buffers band mode and the overlays borrow. */
#define STRIP_BOUNCE_SLOTS 2
extern esp_lcd_panel_handle_t panel;
extern SemaphoreHandle_t strip_sent;
extern gfx_color_t* gather_buf;
extern gfx_color_t* strip_bounce[STRIP_BOUNCE_SLOTS];

bool present_start(void);
bool present_alloc_send_buffers(void);
void panel_clock_apply(void);
void note_send_failure(esp_err_t err);
bool send_fb_rows(int y0, int y1);
#endif

#if CONFIG_LAUNCHER_DEVELOPMENT
/* gfx_debug.c: the overlays and counts gfx_debug.h switches and reads. */
extern bool debug_overlay_on;
extern bool leaf_overlay_on;
extern recti_t* leaf_rect_scratch;
extern int dev_strips_sent_full;
extern int dev_strips_sent_gathered;
extern int dev_strips_sent_partial;
extern int64_t dev_bytes_sent;

static inline bool
overlay_any_on(void) {
    return debug_overlay_on || leaf_overlay_on;
}

void mark_rect_border(gfx_color_t* buf, int stride, int w, int h, gfx_color_t colour);
void mark_band_overlay(gfx_color_t* buf, int row0, int height);
#ifdef ESP_PLATFORM
extern uint32_t overlay_bordered_rows;
void mark_strip_overlay(gfx_color_t* slot, int row);
bool send_row_with_overlays(int row, int y);
void send_overlay_bordered_rows_clean(bool (*send_rows)(int y0, int y1), int* queued);
void send_audit_capture(int x0, int y0, int w, int h, const gfx_color_t* buf);
void send_audit_check(void);
#endif
#endif
