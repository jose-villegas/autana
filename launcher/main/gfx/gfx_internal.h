/*
 * gfx_internal: the state gfx's own files share, and the calls that cross
 * between them. Nothing outside gfx/ includes it; every other file goes
 * through gfx.h, gfx_draw.h, gfx_present.h, gfx_mode.h or gfx_debug.h.
 */
#pragma once

#include <stdbool.h>

#include "gfx/draw/gfx_box.h"
#include "gfx/draw/gfx_target.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_band.h"
#include "gfx/present/gfx_dirty.h"
#include "gfx/present/gfx_heal.h"
#include "gfx/present/gfx_mode.h"

#ifdef ESP_PLATFORM
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

/* gfx_mode.c: the framebuffer, the mode it is held in, and the strip being
 * rendered (a band, or an expanded frame's overlay replay). band_render_active
 * is true only while one is; outside it band mode has no valid target at
 * all, matching gfx_fb_guard.h. */
extern gfx_color_t* fb;
extern gfx_mode_t current_mode;
extern bool band_render_active;
extern gfx_target_t strip_target;

bool alloc_full_framebuffer(void);
gfx_indexed_frame_t indexed_frame(void);

/* An expanded frame: the half picture doubled into rows [y0, y1), with the
 * frame overlay replayed over them; and its end, after the present. */
void expand_rows(int y0, int y1, gfx_color_t* destination);
void clear_expanded_frame(bool free_picture);

/* gfx_draw.c: the clip rectangle an overlay replay saves around itself. */
extern gfx_box_t clip;

/* True for the whole GFX_LAYOUT_BANDS mode. */
static inline bool
band_is_transient(void) {
    return current_mode.layout == GFX_LAYOUT_BANDS;
}

/* What every pixel-writing primitive draws into: the whole framebuffer, or
 * the band currently being rendered; see gfx_target.h for why a target
 * carries its own row range rather than every primitive checking
 * band_render_active for itself. */
static inline gfx_target_t
current_target(void) {
    if (band_render_active) {
        return strip_target;
    }
    return (gfx_target_t){fb, 0, GFX_HEIGHT, GFX_WIDTH};
}

/* Presentation state and the heal queue band mode resets. */
extern bool interlace_on;
extern gfx_heal_t heal;

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
extern dirty_leaf_rect_t* leaf_rect_scratch;
extern int dev_strips_sent_full;
extern int dev_strips_sent_gathered;
extern int dev_strips_sent_partial;
extern int64_t dev_bytes_sent;
extern int64_t dev_heal_bytes_sent;

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
