#include "gfx/present/gfx_mode.h"
#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx_internal.h"
#include "gfx/present/gfx_band_run.h"
#include "gfx/present/gfx_fb_guard.h"
#include "gfx/present/gfx_full_redraw.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_present_guard.h"
#include "util/runtime/memory.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/task.h"

static const char* TAG = "gfx";
#endif

/* The one instance of the state gfx_fb_guard.h declares. */
bool gfx_fb_guard_available = true;
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
unsigned gfx_fb_guard_trips;
#endif

gfx_color_t* fb;
static gfx_color_t* half_image;
static bool frame_expanded;
static gfx_band_overlay_fn frame_overlay;

/* GFX_LAYOUT_FULL_FB until gfx_init() sets real geometry, or an app's
 * gfx_mode_enter() grants something else.
 * gfx_present_begin()/gfx_present_wait() read this to know whether there is
 * a framebuffer to send at all. */
gfx_mode_t current_mode;

/* The band ring's own buffers; current_target() (gfx_internal.h) reads
 * strip_target for every primitive. */
static gfx_color_t* band_buf[GFX_BAND_SLOTS];
static gfx_band_ring_t band_ring;
static int band_current_slot;
bool band_render_active;
gfx_target_t strip_target;
static int band_render_row0;
static int band_render_height;

/* GFX_LAYOUT_INDEXED's own state: the app writes indices,
 * run_present_strips() (gfx_present.c) expands them through whichever LUT is
 * installed. Not a gfx_target.h render target: no drawing primitive writes
 * through it. */
static uint8_t* indexed_image;
static int indexed_grid_w, indexed_grid_h, indexed_cell_size;
static gfx_color_t indexed_lut256[GFX_INDEXED_PALETTE_SIZE];
static bool indexed_dither16_on;

/* Lever 2: which of gfx_dither_mode_t's five is installed for 16-colour
 * mode, meaningless while indexed_dither16_on is false (256 mode keeps
 * its own plain LUT above, no dither concept at all). One table per mode,
 * not a shared buffer: gfx_indexed_set_dither() only ever overwrites the
 * one an app's own mode switch actually asks for. */
static gfx_dither_mode_t indexed_dither_mode = GFX_DITHER_PIXEL_BAYER4;
static gfx_color_t indexed_dither_none_lut[GFX_INDEXED_PALETTE_SIZE];
static gfx_color_t indexed_dither_cell_checker[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CELL_CHECKER_PHASES];
static gfx_color_t indexed_dither_cell_bayer2[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CELL_BAYER2_PHASES];
static gfx_color_t indexed_dither_pixel_checker2[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CHECKER2_ROW_PHASES
                                                 * GFX_INDEXED_CHECKER2_CHUNK_PX];
static gfx_color_t indexed_dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES];

static const gfx_color_t*
indexed_active_table(void) {
    if (!indexed_dither16_on) {
        return indexed_lut256;
    }
    switch (indexed_dither_mode) {
        case GFX_DITHER_NONE: return indexed_dither_none_lut;
        case GFX_DITHER_CELL_CHECKER: return indexed_dither_cell_checker;
        case GFX_DITHER_CELL_BAYER2: return indexed_dither_cell_bayer2;
        case GFX_DITHER_PIXEL_CHECKER2: return indexed_dither_pixel_checker2;
        case GFX_DITHER_PIXEL_BAYER4:
        case GFX_DITHER_MODE_COUNT:
        default: return indexed_dither16_rgb;
    }
}

gfx_indexed_frame_t
indexed_frame(void) {
    return (gfx_indexed_frame_t){
        .image = indexed_image,
        .grid_w = indexed_grid_w,
        .grid_h = indexed_grid_h,
        .cell_size = indexed_cell_size,
        .dither16_on = indexed_dither16_on,
        .dither_mode = indexed_dither_mode,
        .table = indexed_active_table(),
    };
}

/* This frame's own capture of gfx_full_redraw.h's force flag, taken once by
 * gfx_band_frame_begin() so a later gfx_invalidate() call mid-frame
 * affects the NEXT frame, not this one. */
static bool band_frame_force_all;

/* A readback's copy of one whole band-mode frame: gfx_band_submit() fills
 * it during a frame gfx_readback_begin() forced, since the band ring keeps
 * nothing once a band is sent. PSRAM, and only while a readback is open. */
static gfx_color_t* band_snapshot;
static int band_snapshot_bands;
static bool band_snapshot_filling;
static bool band_snapshot_complete;

bool
alloc_full_framebuffer(void) {
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = memory_alloc(bytes, MEMORY_PSRAM);
#ifdef ESP_PLATFORM
    if (fb == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte framebuffer (largest free PSRAM block is %u bytes)", (unsigned)bytes,
                 (unsigned)memory_largest_block(MEMORY_PSRAM));
    }
#endif
    return fb != NULL;
}

static void
free_full_framebuffer(void) {
    memory_free(fb);
    fb = NULL;
}

#ifdef ESP_PLATFORM
static bool
alloc_band_buffers(int band_height) {
    (void)band_height; /* GFX_BAND_HEIGHT <= STRIP_HEIGHT, asserted in gfx_present.c */
    for (int i = 0; i < GFX_BAND_SLOTS; i++) {
        band_buf[i] = strip_bounce[i];
    }
    return true;
}

static bool
alloc_indexed_image(int grid_w, int grid_h) {
    const size_t bytes = (size_t)grid_w * (size_t)grid_h;
    indexed_image = memory_alloc(bytes, MEMORY_INTERNAL);
    if (indexed_image == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte index image", (unsigned)bytes);
        return false;
    }
    return true;
}

static void
free_indexed_image(void) {
    memory_free(indexed_image);
    indexed_image = NULL;
}
#else
static bool
alloc_band_buffers(int band_height) {
    /* No strip_bounce[] to alias on a host build (render_harness's own
     * memory is not the constraint the device alias exists for). */
    const size_t bytes = (size_t)GFX_WIDTH * (size_t)band_height * sizeof(gfx_color_t);
    for (int i = 0; i < GFX_BAND_SLOTS; i++) {
        band_buf[i] = malloc(bytes);
        if (band_buf[i] == NULL) {
            return false;
        }
    }
    return true;
}

static bool
alloc_indexed_image(int grid_w, int grid_h) {
    indexed_image = malloc((size_t)grid_w * (size_t)grid_h);
    return indexed_image != NULL;
}

static void
free_indexed_image(void) {
    free(indexed_image);
    indexed_image = NULL;
}
#endif

static bool
alloc_band_snapshot(void) {
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    band_snapshot = memory_alloc(bytes, MEMORY_PSRAM);
    band_snapshot_bands = 0;
    band_snapshot_filling = false;
    band_snapshot_complete = false;
    return band_snapshot != NULL;
}

static void
free_band_snapshot(void) {
    memory_free(band_snapshot);
    band_snapshot = NULL;
    band_snapshot_filling = false;
    band_snapshot_complete = false;
}

static void
free_band_buffers(void) {
#ifdef ESP_PLATFORM
    /* Aliased into strip_bounce[]: that memory outlives band mode. */
    for (int i = 0; i < GFX_BAND_SLOTS; i++) {
        band_buf[i] = NULL;
    }
#else
    for (int i = 0; i < GFX_BAND_SLOTS; i++) {
        free(band_buf[i]);
        band_buf[i] = NULL;
    }
#endif
}

static void
reset_mode_to_full_fb(void) {
    current_mode.layout = GFX_LAYOUT_FULL_FB;
    current_mode.interlace_x = false;
    current_mode.interlace_y = false;
    current_mode.width = GFX_WIDTH;
    current_mode.height = GFX_HEIGHT;
    current_mode.band_height = 0;
    current_mode.index_grid_w = 0;
    current_mode.index_grid_h = 0;
    current_mode.cell_size = 0;
}

bool
gfx_init(void) {
#ifdef ESP_PLATFORM
    if (!present_start()) {
        return false;
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Framebuffer state post SD probe & panel bring-up; paired with HEAPMARK
     * in main.c. See heap_mark() comment. */
    ESP_LOGI(TAG, "HEAPMARK %-18s free %6u largest %6u", "before framebuffer",
             (unsigned)memory_free_bytes(MEMORY_PSRAM), (unsigned)memory_largest_block(MEMORY_PSRAM));
#endif
#endif
    /* PSRAM: the internal pool has no room for it. It never goes to the
     * panel directly; see strip_bounce. */
    if (!alloc_full_framebuffer()) {
        return false;
    }
#ifdef ESP_PLATFORM
    if (!present_alloc_send_buffers()) {
        return false;
    }
#endif
    reset_mode_to_full_fb();
    gfx_fb_guard_set_available(true);
    gfx_clear_clip();
    gfx_mark_all_dirty();
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG,
             "%dx%d framebuffer at %p, %u bytes in PSRAM; heap free %u, "
             "largest PSRAM block %u",
             GFX_WIDTH, GFX_HEIGHT, (void*)fb, (unsigned)(GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t)),
             (unsigned)memory_free_bytes(MEMORY_8BIT), (unsigned)memory_largest_block(MEMORY_PSRAM));
#endif
    return true;
}

gfx_color_t*
gfx_framebuffer(void) {
    /* gfx can't guess intent. "Everything" wastes resources. Raw writers use
     * gfx_mark_dirty(). Be cautious. */
    GFX_PRESENT_GUARD();
    /* fb is already NULL in band mode, the right answer for a caller that
     * checks. This call is only the loud dev-time signal that one reached
     * for the framebuffer at all while it does not exist. */
    return GFX_REQUIRE_FRAMEBUFFER() ? fb : NULL;
}

gfx_color_t*
gfx_half_picture(void) {
    GFX_PRESENT_GUARD();
    if (current_mode.layout != GFX_LAYOUT_FULL_FB) {
        return NULL;
    }
    if (half_image == NULL) {
        half_image = memory_alloc(sizeof(*half_image) * (GFX_WIDTH / 2) * (GFX_HEIGHT / 2), MEMORY_PSRAM);
    }
    return half_image;
}

void
gfx_expand_frame(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_FULL_FB && half_image != NULL);
    frame_expanded = true;
    gfx_fb_guard_set_available(false);
    gfx_mark_all_dirty();
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

void
clear_expanded_frame(bool free_picture) {
    frame_expanded = false;
    gfx_fb_guard_set_available(current_mode.layout == GFX_LAYOUT_FULL_FB && fb != NULL);
    if (free_picture) {
        memory_free(half_image);
        half_image = NULL;
    }
}

void
expand_rows(int y0, int y1, gfx_color_t* destination) {
    const gfx_target_t target = {destination, y0, y1 - y0, GFX_WIDTH};
    gfx_target_paired_rows(target, half_image, NULL, 0, GFX_WIDTH / 2, NULL, y0, y1 - y0, true);
    if (frame_overlay != NULL) {
        const gfx_box_t saved_clip = clip;
        strip_target = target;
        band_render_active = true;
        gfx_fb_guard_set_available(true);
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
        gfx_present_guard_replaying = true;
#endif
        gfx_clear_clip();
        frame_overlay(y0, y1);
        clip = saved_clip;
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
        gfx_present_guard_replaying = false;
#endif
        band_render_active = false;
        gfx_fb_guard_set_available(false);
    }
}

#ifndef ESP_PLATFORM
unsigned
gfx_fb_guard_trips_for_test(void) {
    return gfx_fb_guard_trips;
}

void
gfx_reset_for_test(void) {
    clear_expanded_frame(true);
    frame_overlay = NULL;
    band_render_active = false;
    strip_target = (gfx_target_t){0};
    if (current_mode.layout == GFX_LAYOUT_INDEXED) {
        free_indexed_image();
    } else if (current_mode.layout == GFX_LAYOUT_BANDS) {
        free_band_buffers();
        free_band_snapshot();
    }
    free_full_framebuffer();
    current_mode = (gfx_mode_t){0};
    gfx_fb_guard_set_available(false);
    gfx_clear_clip();
}
#endif

const gfx_mode_t*
gfx_mode_enter(const gfx_mode_request_t* request) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_FULL_FB);

    clear_expanded_frame(true);
    const gfx_mode_t granted = gfx_mode_resolve(request, GFX_WIDTH, GFX_HEIGHT, GFX_BAND_HEIGHT);

    if (granted.layout == GFX_LAYOUT_INDEXED) {
        if (granted.index_grid_w <= 0 || granted.index_grid_h <= 0 || granted.cell_size <= 0
            || !alloc_indexed_image(granted.index_grid_w, granted.index_grid_h)) {
            free_indexed_image();
            return &current_mode; /* stays GFX_LAYOUT_FULL_FB */
        }
        free_full_framebuffer();
        gfx_fb_guard_set_available(false);
        indexed_grid_w = granted.index_grid_w;
        indexed_grid_h = granted.index_grid_h;
        indexed_cell_size = granted.cell_size;
        indexed_dither16_on = false;
        gfx_mark_all_dirty(); /* nothing sent to the panel yet this visit */
    } else if (granted.layout == GFX_LAYOUT_BANDS) {
        if (!alloc_band_buffers(granted.band_height)) {
            free_band_buffers();
            return &current_mode; /* stays GFX_LAYOUT_FULL_FB */
        }
        free_full_framebuffer();
        gfx_fb_guard_set_available(false);
        gfx_band_ring_begin(&band_ring, granted.height / granted.band_height);
        band_current_slot = 0;
        gfx_band_force_all(); /* nothing sent to the panel yet this visit */
    }

    current_mode = granted;
    return &current_mode;
}

void
gfx_mode_exit(void) {
    GFX_PRESENT_GUARD();
    clear_expanded_frame(true);
    if (current_mode.layout == GFX_LAYOUT_INDEXED) {
        free_indexed_image();
    } else if (current_mode.layout == GFX_LAYOUT_BANDS) {
#ifdef ESP_PLATFORM
        /* band_buf[] aliases strip_bounce[]: the full-fb path that
         * owns it next must never write it while this mode's last
         * band is still on the wire. */
        if (!gfx_band_ring_settled(&band_ring)) {
            xSemaphoreTake(strip_sent, portMAX_DELAY);
            gfx_band_ring_settle(&band_ring);
        }
#endif
        free_band_buffers();
        free_band_snapshot();
    }
    if (current_mode.layout != GFX_LAYOUT_FULL_FB) {
        if (alloc_full_framebuffer()) {
            gfx_fb_guard_set_available(true);
        }
#ifdef ESP_PLATFORM
        else {
            /* Nothing downstream can draw without a framebuffer: the same
             * dead end gfx_init() itself parks in on the same allocation. */
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
#endif
        gfx_clear_clip();
        gfx_mark_all_dirty();
    }
    reset_mode_to_full_fb();
    gfx_fb_guard_set_available(fb != NULL);
}

const gfx_mode_t*
gfx_mode_current(void) {
    return &current_mode;
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Bands whose last submit carried overlay borders, and whether the band in
 * hand is being rendered only to take them off again. */
static uint32_t band_overlay_bordered;
static bool band_overlay_cleanup_only;
_Static_assert(GFX_HEIGHT / 16 <= 32, "one bit per band at the smallest GFX_BAND_HEIGHT");
_Static_assert(GFX_BAND_HEIGHT % LEAF_H == 0, "a leaf must never straddle two bands");
#endif

static void
gfx_band_frame_begin(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_BANDS);
#ifdef ESP_PLATFORM
    panel_clock_apply();
#endif
    band_render_active = false;
    gfx_fb_guard_set_available(false);
    gfx_band_ring_begin(&band_ring, current_mode.height / current_mode.band_height);

    /* Captured once per frame: gfx_invalidate() from a draw or overlay callback
     * cannot retroactively force bands already skipped this frame. */
    band_frame_force_all = gfx_band_take_force_all();

    if (band_snapshot != NULL && !band_snapshot_complete) {
        band_snapshot_bands = 0;
        band_snapshot_filling = band_frame_force_all;
    }

    /* Band mode heals nothing: a band is gone once sent, so gfx holds
     * nothing to resend. The reset stops rows marked or swept in an earlier
     * full-fb present from carrying into this mode. */
    gfx_heal_reset(&heal);
}

/* Band mode's own "does this band need touching" query, on gfx_dirty.h's
 * cell tracker. It always reads the band gfx_band_next() just handed out,
 * so there is no range to pass wrong. */
static bool
gfx_band_dirty(void) {
    const int row0 = band_render_row0;
    const int row1 = band_render_row0 + band_render_height;
    int x0, x1;
    bool dirty = band_frame_force_all || dirty_band_extent(row0, row1, &x0, &x1);
#if CONFIG_LAUNCHER_DEVELOPMENT
    /* A border exists only in the band that was sent, and gfx holds no
     * copy to resend: the one way to take it off the panel is to have the
     * app render the band once more. */
    if (!dirty && (band_overlay_bordered & (1u << (row0 / band_render_height)))) {
        band_overlay_cleanup_only = true;
        dirty = true;
    }
#endif
    return dirty;
}

/* The band gfx_band_next() just handed out needs no redraw this frame
 * (gfx_band_dirty() said so), advances past it without rendering or
 * sending anything, leaving whatever the panel already shows there. */
static void
gfx_band_skip(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_BANDS);
    band_render_active = false;
    gfx_fb_guard_set_available(false);
    gfx_band_ring_skip(&band_ring);
}

static bool
gfx_band_next(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_BANDS);
    if (gfx_band_ring_done(&band_ring)) {
        if (!gfx_band_ring_settled(&band_ring)) {
#ifdef ESP_PLATFORM
            xSemaphoreTake(strip_sent, portMAX_DELAY);
#endif
            gfx_band_ring_settle(&band_ring);
        }
        band_render_active = false;
        /* This frame's gfx_mark_dirty() calls (an app's own coverage, ui.c's
         * per-band UI changes) have done their job for gfx_band_dirty();
         * clear them so next frame's marks start from nothing, the same
         * reset a full-fb present gives itself at the end of every frame. */
        for (int row = 0; row < STRIP_COUNT; row++) {
            dirty_row_sent(row);
        }
        dirty_frame_sent();
        return false;
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    band_overlay_cleanup_only = false;
#endif
    band_current_slot = gfx_band_ring_slot(&band_ring);
    band_render_row0 = gfx_band_ring_row0(&band_ring, current_mode.band_height);
    band_render_height = current_mode.band_height;
    strip_target = (gfx_target_t){band_buf[band_current_slot], band_render_row0, band_render_height, GFX_WIDTH};
    band_render_active = true;
    gfx_fb_guard_set_available(true);
    return true;
}

static gfx_color_t*
gfx_band_buffer(void) {
    GFX_PRESENT_GUARD();
    return band_buf[band_current_slot];
}

static int
gfx_band_row0(void) {
    GFX_PRESENT_GUARD();
    return band_render_row0;
}

static int
gfx_band_height(void) {
    GFX_PRESENT_GUARD();
    return band_render_height;
}

/* Always the full band width, straight from the buffer the app drew: the
 * panel transfer takes no source stride, so a narrower send would first
 * have to repack the rows, and that costs more CPU than the bytes it saves. */
static void
gfx_band_submit(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_BANDS);

#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Drawn into the buffer about to be sent. A band submitted only to
     * clean last frame's borders (gfx_band_dirty()) goes out bare. */
    const uint32_t band_bit = 1u << (band_render_row0 / band_render_height);
    if (overlay_any_on() && !band_overlay_cleanup_only) {
        mark_band_overlay(band_buf[band_current_slot], band_render_row0, band_render_height);
        band_overlay_bordered |= band_bit;
    } else {
        band_overlay_bordered &= ~band_bit;
    }
#endif

    if (band_snapshot_filling || band_snapshot_complete) {
        memcpy(band_snapshot + (size_t)band_render_row0 * GFX_WIDTH, band_buf[band_current_slot],
               (size_t)band_render_height * GFX_WIDTH * sizeof(gfx_color_t));
    }
    if (band_snapshot_filling) {
        band_snapshot_complete = ++band_snapshot_bands == band_ring.band_count;
        band_snapshot_filling = !band_snapshot_complete;
    }
    bool sent = true;
#ifdef ESP_PLATFORM
    if (gfx_band_ring_must_wait(&band_ring)) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
    const int row0 = gfx_band_ring_row0(&band_ring, current_mode.band_height);
    const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, row0, GFX_WIDTH, row0 + current_mode.band_height,
                                                    band_buf[band_current_slot]);
    if (err != ESP_OK) {
        note_send_failure(err);
        sent = false;
    }
#endif
    band_render_active = false;
    gfx_fb_guard_set_available(false);
    if (sent) {
        gfx_band_ring_advance(&band_ring);
        return;
    }
    /* Nothing queued, so nothing will ever mark this band's strip_sent;
     * settle the ring in place and force every band next frame instead of
     * leaving gfx_band_next() waiting on a give that is never coming. */
    gfx_band_force_all();
    gfx_band_ring_settle(&band_ring);
    gfx_band_ring_skip(&band_ring);
}

void
gfx_band_run(gfx_band_draw_fn draw, gfx_band_overlay_fn overlay) {
    if (current_mode.layout != GFX_LAYOUT_BANDS) {
        return;
    }
    assert(draw != NULL);

    gfx_band_frame_begin();
    while (gfx_band_next()) {
        if (!gfx_band_dirty()) {
            gfx_band_skip();
            continue;
        }
        const int row0 = gfx_band_row0();
        const int row1 = row0 + gfx_band_height();
        gfx_color_t* const target = gfx_band_buffer();
        draw(row0, row1, target);
        if (overlay != NULL) {
            overlay(row0, row1);
        }
        gfx_band_submit();
    }
}

uint8_t*
gfx_indexed_image(void) {
    GFX_PRESENT_GUARD();
    return indexed_image;
}

gfx_readback_t
gfx_readback_begin(void) {
    GFX_PRESENT_GUARD();
    if (!band_is_transient()) {
        return GFX_READBACK_READY;
    }
    if (band_snapshot == NULL && !alloc_band_snapshot()) {
        return GFX_READBACK_UNAVAILABLE;
    }
    if (band_snapshot_complete) {
        return GFX_READBACK_READY;
    }
    gfx_band_force_all();
    return GFX_READBACK_PENDING;
}

void
gfx_read_panel_row(int y, gfx_color_t out_row[GFX_WIDTH]) {
    GFX_PRESENT_GUARD();
    if (frame_expanded) {
        expand_rows(y, y + 1, out_row);
    } else if (current_mode.layout == GFX_LAYOUT_FULL_FB) {
        memcpy(out_row, fb + (size_t)y * GFX_WIDTH, GFX_WIDTH * sizeof(gfx_color_t));
    } else if (current_mode.layout == GFX_LAYOUT_INDEXED) {
        const gfx_indexed_frame_t frame = indexed_frame();
        gfx_indexed_expand_panel_row(&frame, y, out_row, GFX_WIDTH);
    } else {
        assert(band_snapshot_complete);
        memcpy(out_row, band_snapshot + (size_t)y * GFX_WIDTH, GFX_WIDTH * sizeof(gfx_color_t));
    }
}

/* Band mode's snapshot stays: once filled, every submitted band keeps it
 * equal to the panel, so the next capture is ready at once. It goes with
 * the mode, in gfx_mode_exit(). */
void
gfx_readback_end(void) {
    GFX_PRESENT_GUARD();
    if (!band_is_transient()) {
        free_band_snapshot();
    }
}

void
gfx_indexed_set_lut(const gfx_color_t lut[GFX_INDEXED_PALETTE_SIZE]) {
    GFX_PRESENT_GUARD();
    memcpy(indexed_lut256, lut, sizeof indexed_lut256);
}

void
gfx_indexed_set_lut16(const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES]) {
    GFX_PRESENT_GUARD();
    memcpy(indexed_dither16_rgb, dither16_rgb, sizeof indexed_dither16_rgb);
}

void
gfx_indexed_set_dither16(bool enabled) {
    GFX_PRESENT_GUARD();
    indexed_dither16_on = enabled;
}

/* Installs `table` for `mode` and selects it as the active one:
 * GFX_LAYOUT_INDEXED's own dither pattern while indexed_dither16_on is
 * true (gfx_indexed_set_dither16()); meaningless in 256 mode, which never
 * consults it. `table` must be sized for `mode`; see gfx_dither_mode_t's
 * own comment (gfx_indexed.h) for which. Present-task-only, like every
 * other indexed setter here (GFX_PRESENT_GUARD()). */
void
gfx_indexed_set_dither(gfx_dither_mode_t mode, const gfx_color_t* table) {
    GFX_PRESENT_GUARD();
    switch (mode) {
        case GFX_DITHER_NONE: memcpy(indexed_dither_none_lut, table, sizeof indexed_dither_none_lut); break;
        case GFX_DITHER_CELL_CHECKER:
            memcpy(indexed_dither_cell_checker, table, sizeof indexed_dither_cell_checker);
            break;
        case GFX_DITHER_CELL_BAYER2:
            memcpy(indexed_dither_cell_bayer2, table, sizeof indexed_dither_cell_bayer2);
            break;
        case GFX_DITHER_PIXEL_CHECKER2:
            memcpy(indexed_dither_pixel_checker2, table, sizeof indexed_dither_pixel_checker2);
            break;
        case GFX_DITHER_PIXEL_BAYER4: memcpy(indexed_dither16_rgb, table, sizeof indexed_dither16_rgb); break;
        case GFX_DITHER_MODE_COUNT: break;
    }
    indexed_dither_mode = mode;
}
