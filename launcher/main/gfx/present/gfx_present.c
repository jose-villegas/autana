#include "gfx/present/gfx_present.h"
#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx_internal.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_full_redraw.h"
#include "gfx/present/gfx_present_guard.h"
#include "util/build/build_variant.h"
#include "util/runtime/frame_watch.h"
#include "util/runtime/memory.h"
#include "util/scalar/mathi.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "board/board.h"
#include "board/board_panel.h"
#include "esp_log.h"
#include "freertos/task.h"
#if CONFIG_LAUNCHER_QEMU
#include "gfx/present/gfx_null_panel.h"
#endif

_Static_assert(GFX_WIDTH == BSP_LCD_H_RES && GFX_HEIGHT == BSP_LCD_V_RES, "gfx.h's panel size must be the board's");
#endif
_Static_assert(GFX_HEAL_STRIP_ROWS <= STRIP_HEIGHT, "a heal strip must fit one strip bounce slot");

#ifdef ESP_PLATFORM
static const char* TAG = "gfx";
#endif

/* The one instance of the state gfx_dirty.h, gfx_full_redraw.h and
 * gfx_present_guard.h declare. */
uint32_t cell_dirty;
bool all_dirty;
int cell_x0[CELL_COUNT];
int cell_x1[CELL_COUNT];
int cell_y0[CELL_COUNT];
int cell_y1[CELL_COUNT];
uint16_t leaf_dirty[STRIP_COUNT * LEAF_SUB];
bool gfx_band_force_all_dirty = true;
bool gfx_full_redraw_latched;
bool gfx_present_guard_in_flight;
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
unsigned gfx_present_guard_trips;
#endif

static bool present_async_on = true;

/* The panel clock choice, written by gfx_set_panel_clock_hz() from any task.
 * The send side reopens the link at this rate before a present's first send,
 * when nothing is in flight. */
static volatile int panel_clock_requested_hz = GFX_QSPI_HZ;

/* Filled on the caller's side of a present, drained on the send side. */
gfx_heal_t heal;
static int heal_budget_pixels = GFX_HEAL_DEFAULT_BUDGET_PIXELS;
static int heal_rolling_rows;

static bool
panel_clock_valid(int hz) {
    return hz == GFX_PANEL_CLOCK_SLOW_HZ || hz == GFX_PANEL_CLOCK_FAST_HZ;
}

#ifdef ESP_PLATFORM
esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
SemaphoreHandle_t strip_sent;

/* The present task: brings the panel up on core 1 (so the strip-sent
 * interrupt lands there) and, from then on, is the only task that ever
 * sends. gfx_init() waits on present_bringup_sem for present_bringup_ok
 * before deciding its own return value. */
static TaskHandle_t present_task_handle;
static SemaphoreHandle_t present_bringup_sem;
static SemaphoreHandle_t present_done_sem;
static bool present_bringup_ok;
static StaticTask_t present_task_tcb;

typedef enum { PRESENT_TASK_NORMAL, PRESENT_TASK_RAW_FULL } present_task_mode_t;

static present_task_mode_t present_task_mode;

#define PRESENT_TASK_STACK_BYTES 4096
#define PRESENT_TASK_PRIORITY    5
#define PRESENT_TASK_CORE        1

/* Overlay save/restore scratch (see overlay_cell_save()), MEMORY_DMA. */
gfx_color_t* gather_buf;

/* The panel controller takes a window only on even edges: an odd start or
 * an odd exclusive end leaves stale pixels at the window's corners, at
 * either clock.
 * Waveshare's BSP rounds every flush area the same way. GFX_WIDTH and
 * GFX_HEIGHT are even, so rounding outward never leaves the screen. A
 * gathered box grows by at most one column and one row, hence the slack. */
_Static_assert(GFX_WIDTH % 2 == 0 && GFX_HEIGHT % 2 == 0, "panel windows round to even edges");
#define GATHER_WINDOW_MAX_PIXELS (GATHER_MAX_PIXELS + GFX_WIDTH + STRIP_HEIGHT + 1)

/* Full-width sends copy out of the PSRAM framebuffer into internal DMA
 * RAM first. SPI DMA reading PSRAM in place shares the PSRAM bus's
 * bandwidth, and past 40 MHz QSPI the panel receives dropped data. Two
 * slots are enough to keep strips queuing back to back: esp_lcd sends a
 * window's address commands only after the previous transfer has drained,
 * so once esp_lcd_panel_draw_bitmap() returns, the strip before it is off
 * the bus. */
gfx_color_t* strip_bounce[STRIP_BOUNCE_SLOTS];
static int strip_bounce_next;
_Static_assert(GATHER_WINDOW_MAX_PIXELS <= GFX_WIDTH * STRIP_HEIGHT,
               "a gathered window must fit one strip_bounce slot");

/* band_buf[] (alloc_band_buffers(), gfx_mode.c) always aliases these slots
 * instead of allocating; idle whenever band mode is, since band mode
 * never runs the full-fb send path they belong to. */
_Static_assert(GFX_BAND_HEIGHT <= STRIP_HEIGHT, "a band must fit one strip_bounce slot to alias it");
_Static_assert(GFX_BAND_SLOTS == STRIP_BOUNCE_SLOTS, "band_buf[] aliasing strip_bounce[] needs equal slot counts");

/*
 * Panel plumbing: device-only. A host build never brings a panel up or
 * presents to one; see gfx_init()/gfx_present() for the host side of each.
 */

static bool IRAM_ATTR
on_strip_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* event, void* user_context) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(strip_sent, &woken);
    return woken == pdTRUE;
}

#if CONFIG_LAUNCHER_QEMU
static void
null_panel_strip_done(void) {
    xSemaphoreGive(strip_sent);
}
#endif

static esp_err_t
panel_open(int hz) {
#if CONFIG_LAUNCHER_QEMU
    if (board_variant() == BOARD_VARIANT_UNKNOWN) {
        return gfx_null_panel_open(hz, null_panel_strip_done, &panel);
    }
#endif
    return board_panel_open(hz, on_strip_sent, &panel_io, &panel);
}

static int panel_clock_applied_hz;

/* VERY important: only with nothing queued on the link; deleting the io
 * waits out its transactions, but the strip_sent a caller is owed is lost. */
void
panel_clock_apply(void) {
    const int hz = panel_clock_requested_hz;
    if (hz == panel_clock_applied_hz) {
        return;
    }
    esp_lcd_panel_del(panel);
    if (panel_io != NULL) {
        esp_lcd_panel_io_del(panel_io);
    }
    panel = NULL;
    panel_io = NULL;
    if (panel_open(hz) != ESP_OK) {
        ESP_LOGE(TAG, "could not reopen the panel link at %d MHz", hz / 1000000);
        abort();
    }
    panel_clock_applied_hz = hz;
}

static bool
display_bring_up(int hz) {
    if (board_detect() == BOARD_VARIANT_UNKNOWN) {
#if CONFIG_LAUNCHER_QEMU
        ESP_LOGW(TAG, "No board answered; presenting to a null panel");
        return panel_open(hz) == ESP_OK;
#endif
        ESP_LOGE(TAG, "Could not identify the board");
        return false;
    }
    if (board_panel_bring_up(hz, GFX_WIDTH * STRIP_HEIGHT * (int)sizeof(gfx_color_t), on_strip_sent, &panel_io, &panel)
        != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the display");
        return false;
    }
    return true;
}

/* Defined far below, alongside every other send-path function; the task
 * loop only needs to call them. */
static void run_present_normal(void);
#if CONFIG_LAUNCHER_DEVELOPMENT
static void run_present_raw_full(void);
#endif

/* Runs entirely on core 1. Bring-up happens here, once, so the strip-sent
 * interrupt esp_lcd installs lands on this core; see board_panel_bring_up().
 * After reporting bring-up, waits for a notification per present and gives
 * present_done_sem back once everything queued has actually landed. */
static void
present_task_fn(void* arg) {
    (void)arg;

    /* Sized for STRIP_COUNT * GRID_COLS: see send_one_row(). Undersizing
     * blocks this task's own send loop forever. */
    strip_sent = xSemaphoreCreateCounting(STRIP_COUNT * GRID_COLS + 2, 0);
    if (strip_sent == NULL) {
        ESP_LOGE(TAG, "Could not create the strip-transfer semaphore");
        present_bringup_ok = false;
        xSemaphoreGive(present_bringup_sem);
        vTaskDelete(NULL);
        return;
    }

    panel_clock_applied_hz = panel_clock_requested_hz;
    if (!display_bring_up(panel_clock_applied_hz)) {
        present_bringup_ok = false;
        xSemaphoreGive(present_bringup_sem);
        vTaskDelete(NULL);
        return;
    }

    present_bringup_ok = true;
    xSemaphoreGive(present_bringup_sem);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if CONFIG_LAUNCHER_DEVELOPMENT
        if (present_task_mode == PRESENT_TASK_RAW_FULL) {
            run_present_raw_full();
            xSemaphoreGive(present_done_sem);
            continue;
        }
#endif
        run_present_normal();
        xSemaphoreGive(present_done_sem);
    }
}

/* Starts the present task and waits for it to bring the panel up. */
bool
present_start(void) {
    present_bringup_sem = xSemaphoreCreateBinary();
    present_done_sem = xSemaphoreCreateBinary();
    if (present_bringup_sem == NULL || present_done_sem == NULL) {
        ESP_LOGE(TAG, "Could not create the present task's semaphores");
        return false;
    }

    StackType_t* const present_stack = memory_alloc(PRESENT_TASK_STACK_BYTES, MEMORY_INTERNAL);
    if (present_stack == NULL) {
        ESP_LOGE(TAG, "Could not allocate the present task's %u byte stack", (unsigned)PRESENT_TASK_STACK_BYTES);
        return false;
    }
    present_task_handle =
        xTaskCreateStaticPinnedToCore(present_task_fn, "gfx_present", PRESENT_TASK_STACK_BYTES / sizeof(StackType_t),
                                      NULL, PRESENT_TASK_PRIORITY, present_stack, &present_task_tcb, PRESENT_TASK_CORE);
    if (present_task_handle == NULL) {
        ESP_LOGE(TAG, "Could not create the present task");
        return false;
    }
    frame_watch_add_task(present_task_handle);

    /* Bring-up (board_detect(), board_panel_bring_up()) runs on that task; see
     * present_task_fn(). Its own ESP_LOGE already named the failure. */
    xSemaphoreTake(present_bringup_sem, portMAX_DELAY);
    return present_bringup_ok;
}

/* The send side's internal DMA RAM, allocated after the framebuffer. */
bool
present_alloc_send_buffers(void) {
    const size_t gather_bytes = (size_t)GATHER_WINDOW_MAX_PIXELS * sizeof(gfx_color_t);
    gather_buf = memory_alloc(gather_bytes, MEMORY_DMA);
    if (gather_buf == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte gather buffer", (unsigned)gather_bytes);
        return false;
    }

    const size_t strip_bytes = (size_t)GFX_WIDTH * STRIP_HEIGHT * sizeof(gfx_color_t);
    for (int i = 0; i < STRIP_BOUNCE_SLOTS; i++) {
        strip_bounce[i] = memory_alloc(strip_bytes, MEMORY_DMA);
        if (strip_bounce[i] == NULL) {
            ESP_LOGE(TAG, "Could not allocate %u byte strip buffer", (unsigned)strip_bytes);
            return false;
        }
    }
    return true;
}
#endif /* ESP_PLATFORM */

/* Dirty tracking */

bool partial_clear_on;
bool interlace_on;
#ifdef ESP_PLATFORM
static int frame_parity; /* read only inside run_present_normal(), below */
#endif
gfx_box_t prev_bbox = GFX_BOX_EMPTY;
gfx_box_t drawn_bbox = GFX_BOX_EMPTY;

void
gfx_set_partial_clear(bool on) {
    GFX_PRESENT_GUARD();
    if (!on) {
        prev_bbox = GFX_BOX_EMPTY;
    }
    partial_clear_on = on;
}

void
gfx_set_interlace(bool on) {
    GFX_PRESENT_GUARD();
    interlace_on = on;
}

bool
gfx_interlace_enabled(void) {
    return interlace_on;
}

void
gfx_invalidate(void) {
    GFX_PRESENT_GUARD();
    prev_bbox = GFX_BOX_EMPTY;
    gfx_band_force_all();
}

/* Guard-free body of gfx_mark_all_dirty(), also called from the send path
 * itself (already past the guard by definition: a present is in flight)
 * when a rejected esp_lcd_panel_draw_bitmap() means this frame never
 * reached the panel. */
static void
mark_all_dirty_now(void) {
    dirty_mark_all();
    drawn_bbox = GFX_BOX_EMPTY;
    prev_bbox = GFX_BOX_EMPTY;
}

/* gfx_dirty.h header-only for inlining mark_band(); thin wrappers for gfx.h
 * API. */
void
gfx_mark_all_dirty(void) {
    GFX_PRESENT_GUARD();
    mark_all_dirty_now();
}

/* The one call a transition needs instead of composing gfx_mark_all_dirty()
 * and gfx_invalidate() separately: every gfx-side cache that decides
 * whether to repaint or resend a region is reset in one place. Latches a
 * pending flag an app's optional invalidate() callback (app.h) answers to
 * on the pass that follows; see gfx_full_redraw_pending() below. Sets
 * state only and frees nothing, so it is safe from anywhere on core 0,
 * including inside a UI build or an app callback. */
void
gfx_request_full_redraw(void) {
    GFX_PRESENT_GUARD();
    gfx_mark_all_dirty();
    gfx_invalidate();
    gfx_full_redraw_latch();
}

/* True once gfx_request_full_redraw() has been called and the shell has
 * not yet cleared it for the pass that follows; see
 * gfx_full_redraw_clear_pending(). */
bool
gfx_full_redraw_pending(void) {
    return gfx_full_redraw_is_pending();
}

/* Ends the window gfx_request_full_redraw() opened. The shell calls this
 * once it has read the flag and decided whether to invoke an app's
 * invalidate(), before that pass's frame() runs; see shell/shell_apps.c's
 * apply_pending_full_redraw(). */
void
gfx_full_redraw_clear_pending(void) {
    GFX_PRESENT_GUARD();
    gfx_full_redraw_unlatch();
}

void
gfx_mark_dirty(int x, int y, int w, int h) {
    GFX_PRESENT_GUARD();
    dirty_mark(x, y, w, h);

    if (w <= 0 || h <= 0) {
        return;
    }
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w > GFX_WIDTH ? GFX_WIDTH : x + w;
    const int y1 = y + h > GFX_HEIGHT ? GFX_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    gfx_box_extend(&drawn_bbox, (gfx_box_t){x0, y0, x1, y1});
}

bool
gfx_region_dirty(int x, int y, int w, int h) {
    GFX_PRESENT_GUARD();
    return dirty_region_dirty(x, y, w, h);
}

/* Device-only path for QSPI panel send. Host build is no-op. */
#ifdef ESP_PLATFORM

/* A rejected esp_lcd_panel_draw_bitmap() queues nothing, so its strip_sent
 * give never comes; every send site below checks this instead of taking
 * the semaphore unconditionally. present_send_failed lets one present
 * notice a mid-frame rejection and force a full resend once, at the end,
 * rather than re-deriving which region was affected at each call site. */
static bool present_send_failed;

#if CONFIG_LAUNCHER_DEVELOPMENT
#define SEND_FAILURE_KINDS_MAX 4
static esp_err_t send_failure_kinds[SEND_FAILURE_KINDS_MAX];
static int send_failure_kind_count;
#endif

void
note_send_failure(esp_err_t err) {
    present_send_failed = true;
#if CONFIG_LAUNCHER_DEVELOPMENT
    for (int i = 0; i < send_failure_kind_count; i++) {
        if (send_failure_kinds[i] == err) {
            return;
        }
    }
    ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap rejected a strip: %s", esp_err_to_name(err));
    if (send_failure_kind_count < SEND_FAILURE_KINDS_MAX) {
        send_failure_kinds[send_failure_kind_count++] = err;
    }
#endif
}

/* Packs the box into the next strip_bounce slot and queues it, the same
 * ring send_fb_rows() uses, so copying one run overlaps sending the last. */
static void
gather_and_send(int x0, int y0, int x1, int y1, int row, int run_start, int run_end, bool refined, int* queued,
                gfx_color_t border) {
    x0 = mathi_even_floor(x0);
    y0 = mathi_even_floor(y0);
    x1 = mathi_even_ceil(x1);
    y1 = mathi_even_ceil(y1);
    const int w = x1 - x0;
    const int h = y1 - y0;

    gfx_color_t* const slot = strip_bounce[strip_bounce_next];
    strip_bounce_next = (strip_bounce_next + 1) % STRIP_BOUNCE_SLOTS;
    for (int r = 0; r < h; r++) {
        memcpy(slot + (size_t)r * w, fb + (size_t)(y0 + r) * GFX_WIDTH + x0, (size_t)w * sizeof(gfx_color_t));
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    send_audit_capture(x0, y0, w, h, slot);
    if (debug_overlay_on && refined) {
        /* One border around the whole packed box. */
        mark_rect_border(slot, w, w, h, border);
    } else if (debug_overlay_on) {
        for (int col = run_start; col < run_end; col++) {
            const int idx = row * GRID_COLS + col;
            gfx_color_t* at = slot + (size_t)(cell_y0[idx] - y0) * w + (cell_x0[idx] - x0);
            mark_rect_border(at, w, cell_x1[idx] - cell_x0[idx], cell_y1[idx] - cell_y0[idx], border);
        }
    }
    if (leaf_overlay_on) {
        const int n = dirty_leaf_rects(row, x0, y0, x1, y1, leaf_rect_scratch, LEAF_RECTS_PER_ROW_MAX);
        for (int i = 0; i < n; i++) {
            const dirty_leaf_rect_t* r = &leaf_rect_scratch[i];
            gfx_color_t* at = slot + (size_t)(r->y0 - y0) * w + (r->x0 - x0);
            mark_rect_border(at, w, r->x1 - r->x0, r->y1 - r->y0, gfx_rgb(0x00FF00));
        }
    }
#else
    (void)row;
    (void)run_start;
    (void)run_end;
    (void)refined;
    (void)border;
#endif
#if CONFIG_LAUNCHER_DEVELOPMENT
    dev_bytes_sent += (int64_t)w * h * sizeof(gfx_color_t);
#endif
    const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, slot);
    if (err != ESP_OK) {
        note_send_failure(err);
        return;
    }
    (*queued)++;
}

/* Queues framebuffer rows [y0, y1) through the next strip_bounce slot. At
 * most STRIP_HEIGHT rows. Returns whether the panel accepted the transfer:
 * false means no strip_sent give is coming for it. */
bool
send_fb_rows(int y0, int y1) {
    gfx_color_t* const slot = strip_bounce[strip_bounce_next];
    strip_bounce_next = (strip_bounce_next + 1) % STRIP_BOUNCE_SLOTS;
    memcpy(slot, fb + (size_t)y0 * GFX_WIDTH, (size_t)(y1 - y0) * GFX_WIDTH * sizeof(gfx_color_t));
#if CONFIG_LAUNCHER_DEVELOPMENT
    send_audit_capture(0, y0, GFX_WIDTH, y1 - y0, slot);
    dev_bytes_sent += (int64_t)(y1 - y0) * GFX_WIDTH * sizeof(gfx_color_t);
#endif
    const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y0, GFX_WIDTH, y1, slot);
    if (err == ESP_OK) {
        return true;
    }
    note_send_failure(err);
    return false;
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* The strip row send_indexed_rows() should border, or -1 for a clean send.
 * A file static rather than a parameter: send_heal_strips() takes
 * send_indexed_rows() as a callback with send_fb_rows()'s signature. */
static int indexed_overlay_row = -1;
#endif

/* send_fb_rows()'s GFX_LAYOUT_INDEXED counterpart: expands rows [y0, y1)
 * from the index image through the installed LUT, into the same bounce
 * slots, instead of copying pixels already sitting in `fb`. Same return
 * contract as send_fb_rows(). */
static bool
send_indexed_rows(int y0, int y1) {
    gfx_color_t* const slot = strip_bounce[strip_bounce_next];
    strip_bounce_next = (strip_bounce_next + 1) % STRIP_BOUNCE_SLOTS;

    const gfx_indexed_frame_t frame = indexed_frame();
    for (int y = y0; y < y1; y++) {
        gfx_indexed_expand_panel_row(&frame, y, slot + (size_t)(y - y0) * GFX_WIDTH, GFX_WIDTH);
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (indexed_overlay_row >= 0) {
        mark_indexed_strip_overlay(slot, indexed_overlay_row);
    }
    dev_bytes_sent += (int64_t)(y1 - y0) * GFX_WIDTH * sizeof(gfx_color_t);
#endif
    const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y0, GFX_WIDTH, y1, slot);
    if (err == ESP_OK) {
        return true;
    }
    note_send_failure(err);
    return false;
}

static void
send_full_row(int row, int* queued) {
    const int y = row * STRIP_HEIGHT;

#if CONFIG_LAUNCHER_DEVELOPMENT
    if (send_row_with_overlays(row, y)) {
        return;
    }
#endif

    if (send_fb_rows(y, y + STRIP_HEIGHT)) {
        (*queued)++;
    }
}

/* Third, cheapest send path: a full-width box is already contiguous in
 * `fb`, so it sends exactly the panel's bytes with no packing. Needs the
 * box's real sub-strip Y extent (cell_y0/cell_y1, gfx_dirty.h), not the
 * coarse strip grid. Measured ~10% fewer pixels per frame on scenes that
 * dirty many short spans, 0% where strips are genuinely full-height.
 * Declines whenever either overlay layer is on: their save/restore
 * machinery assumes send_row_with_overlays()'s full STRIP_HEIGHT box. */
static bool
send_partial_band(int y0, int y1, int* queued) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (overlay_any_on()) {
        return false;
    }
#endif
    y0 = mathi_even_floor(y0);
    y1 = mathi_even_ceil(y1);
    if (send_fb_rows(y0, y1)) {
        (*queued)++;
    }
    return true;
}

/* See gfx_dirty.h file comment */

/* Yellow; see gather_and_send()'s comment */
static void
send_run(int row, int run_start, int run_end, int box_x0, int box_x1, int box_y0, int box_y1, int split_n,
         const int* split_x0, const int* split_x1, int* queued) {
    if (split_n == 0) {
        gather_and_send(box_x0, box_y0, box_x1, box_y1, row, run_start, run_end, false, queued, gfx_rgb(0xFFFF00));
        return;
    }

    for (int i = 0; i < split_n; i++) {
        gather_and_send(split_x0[i], box_y0, split_x1[i], box_y1, row, run_start, run_end, true, queued,
                        gfx_rgb(0xFFFF00));
    }
}

/* Cells merge into transactions; gaps remain. Falls back to full row for
 * large parts. */
static void
send_one_row(int row, int* queued) {
    int run_start[GRID_COLS], run_end[GRID_COLS];
    int box_x0[GRID_COLS], box_x1[GRID_COLS];
    int box_y0[GRID_COLS], box_y1[GRID_COLS];
    int split_n[GRID_COLS];
    int split_x0[GRID_COLS][LEAF_REFINE_MAX_RUNS];
    int split_x1[GRID_COLS][LEAF_REFINE_MAX_RUNS];
    const int n = collect_dirty_runs(row, run_start, run_end);

    for (int r = 0; r < n; r++) {
        run_box(row, run_start[r], run_end[r], &box_x0[r], &box_x1[r], &box_y0[r], &box_y1[r]);
        split_n[r] = plan_run(row, run_start[r], run_end[r], box_y0[r], box_y1[r], split_x0[r], split_x1[r]);

        if (split_n[r] == 0) {
            const size_t area = (size_t)(box_x1[r] - box_x0[r]) * (size_t)(box_y1[r] - box_y0[r]);
            if (area > GATHER_MAX_PIXELS) {
                /* See send_partial_band(). Full-width box means row's only
                 * run: safe to return. */
                if (box_x0[r] == 0 && box_x1[r] == GFX_WIDTH && box_y1[r] - box_y0[r] < STRIP_HEIGHT
                    && send_partial_band(box_y0[r], box_y1[r], queued)) {
#if CONFIG_LAUNCHER_DEVELOPMENT
                    dev_strips_sent_partial++;
#endif
                    return;
                }
#if CONFIG_LAUNCHER_DEVELOPMENT
                dev_strips_sent_full++;
#endif
                send_full_row(row, queued);
                return;
            }
        }
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    if (n > 0) {
        dev_strips_sent_gathered++;
    }
#endif
    for (int r = 0; r < n; r++) {
        send_run(row, run_start[r], run_end[r], box_x0[r], box_x1[r], box_y0[r], box_y1[r], split_n[r], split_x0[r],
                 split_x1[r], queued);
    }
}

/* Queues this present's heal strips through `send_rows`, after its dirty
 * sends so a strip carries whatever they just put on the panel. */
static void
send_heal_strips(bool (*send_rows)(int y0, int y1), int* queued) {
    if (panel_clock_applied_hz != GFX_PANEL_CLOCK_FAST_HZ) {
        gfx_heal_reset(&heal);
        return;
    }
    gfx_heal_queue_rolling(&heal, heal_rolling_rows);
    gfx_heal_strip_t strips[GFX_HEAL_MAX_STRIPS];
    const int n = gfx_heal_plan(&heal, heal_budget_pixels, GFX_WIDTH, strips, GFX_HEAL_MAX_STRIPS);
    for (int i = 0; i < n; i++) {
        if (!send_rows(strips[i].y0, strips[i].y1)) {
            continue;
        }
        (*queued)++;
#if CONFIG_LAUNCHER_DEVELOPMENT
        dev_heal_bytes_sent += (int64_t)(strips[i].y1 - strips[i].y0) * GFX_WIDTH * sizeof(gfx_color_t);
#endif
    }
}

/* GFX_LAYOUT_INDEXED's own send loop, whole dirty STRIP_HEIGHT strips,
 * full width, rather than send_one_row()'s per-run gathering: the index
 * image is small enough that expanding a strip nothing changed in costs
 * little, and every dirty strip still goes through gfx_dirty.h's own
 * tracker unmodified. No interlace or partial-clear here: both are
 * independent app opt-ins the RGB565 path alone offers. */
static void
run_present_indexed(void) {
    int queued = 0;
#if CONFIG_LAUNCHER_DEVELOPMENT
    /* A dirty row is about to be sent whole anyway, so only rows that went
     * quiet need the clean resend. */
    for (int row = 0; row < STRIP_COUNT; row++) {
        if (dirty_row_is_dirty(row)) {
            overlay_bordered_rows &= ~(1u << row);
        }
    }
    send_overlay_bordered_rows_clean(send_indexed_rows, &queued);
#endif
    for (int row = 0; row < STRIP_COUNT; row++) {
        if (!dirty_row_is_dirty(row)) {
            continue;
        }
#if CONFIG_LAUNCHER_DEVELOPMENT
        if (overlay_any_on()) {
            overlay_bordered_rows |= 1u << row;
            indexed_overlay_row = row;
        }
#endif
        if (send_indexed_rows(row * STRIP_HEIGHT, (row + 1) * STRIP_HEIGHT)) {
            queued++;
        }
#if CONFIG_LAUNCHER_DEVELOPMENT
        indexed_overlay_row = -1;
#endif
        dirty_row_sent(row);
    }
    dirty_frame_sent();
    send_heal_strips(send_indexed_rows, &queued);
    for (int i = 0; i < queued; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
    if (present_send_failed) {
        mark_all_dirty_now();
    }
}

/* Sends every dirty strip row this present owns. Returns the cells of the
 * rows interlace left for the next present, still dirty. */
static uint32_t
send_dirty_rows(int* queued) {
    uint32_t remaining_cell_dirty = 0;

    for (int row = 0; row < STRIP_COUNT; row++) {
        if (!dirty_row_is_dirty(row)) {
            continue; /* unchanged: the panel is still showing it */
        }

        if (interlace_on && (row % 2) != frame_parity) {
            remaining_cell_dirty |= cell_dirty & (((1u << GRID_COLS) - 1u) << (row * GRID_COLS));
            continue;
        }

#if CONFIG_LAUNCHER_DEVELOPMENT
        if (overlay_any_on()) {
            overlay_bordered_rows |= 1u << row;
        }
#endif
        send_one_row(row, queued);
        dirty_row_sent(row);
    }

    return remaining_cell_dirty;
}

/* The real send, run on the present task (async) or on the caller
 * (gfx_set_present_async(false)), either way, on whichever core called it,
 * since strip_sent is an ordinary FreeRTOS semaphore and the panel's own
 * strip-sent interrupt is core-agnostic about who it wakes. */
static void
run_present_normal(void) {
    panel_clock_apply();
    present_send_failed = false;
    if (current_mode.layout == GFX_LAYOUT_INDEXED) {
        run_present_indexed();
        return;
    }

    int queued = 0;
    if (interlace_on) {
        frame_parity = !frame_parity;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    send_overlay_bordered_rows_clean(send_fb_rows, &queued);
#endif

    const uint32_t remaining_cell_dirty = send_dirty_rows(&queued);

    dirty_frame_sent();
    if (interlace_on) {
        cell_dirty = remaining_cell_dirty;
    }

    if (partial_clear_on && !gfx_box_is_empty(drawn_bbox)) {
        prev_bbox = drawn_bbox;
    } else if (!partial_clear_on) {
        prev_bbox = GFX_BOX_EMPTY;
    }
    drawn_bbox = GFX_BOX_EMPTY;

    send_heal_strips(send_fb_rows, &queued);

    /* Wait for queued full-width sends to drain. */
    for (int i = 0; i < queued; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    send_audit_check();
#endif
    if (present_send_failed) {
        mark_all_dirty_now();
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Bypasses gfx_present()'s dirty tracking: every strip through the same
 * bounce copy and queue a full-band send takes, one strip_sent per strip. */
static void
run_present_raw_full(void) {
    panel_clock_apply();
    int queued = 0;
    for (int row = 0; row < STRIP_COUNT; row++) {
        if (send_fb_rows(row * STRIP_HEIGHT, (row + 1) * STRIP_HEIGHT)) {
            queued++;
        }
    }
    for (int i = 0; i < queued; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
}
#endif

/* Dispatches to the present task when async, runs directly otherwise; see
 * gfx_set_present_async(). Shared by gfx_present_begin() and the raw-full
 * test helper below, which only differ in present_task_mode. */
static void
dispatch_present(void) {
    if (present_async_on) {
        xTaskNotifyGive(present_task_handle);
        return;
    }
    /* Synchronous: the send happens now, on the caller's own core, before
     * gfx_present_begin() returns; gfx_present_wait() then has nothing
     * left to wait for. */
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (present_task_mode == PRESENT_TASK_RAW_FULL) {
        run_present_raw_full();
        return;
    }
#endif
    run_present_normal();
}

void
gfx_present_begin(void) {
    frame_watch_presented();
    gfx_present_guard_begin();
    if (band_is_transient()) {
        return; /* gfx_band_run() sends and waits after frame() returns */
    }
    present_task_mode = PRESENT_TASK_NORMAL;
    dispatch_present();
}

void
gfx_present_wait(void) {
    if (!band_is_transient() && present_async_on) {
        xSemaphoreTake(present_done_sem, portMAX_DELAY);
    }
    gfx_present_guard_end();
}

#else /* !ESP_PLATFORM */

void
gfx_present_begin(void) {
    gfx_present_guard_begin();
}

void
gfx_present_wait(void) {
    /* No panel on a host build; draining the dirty tracker here is what
     * lets a host test assert the same "sequencing leaves it clean"
     * property a real present provides; see suite_gfx_present_guard.c.
     * A transient band frame drains its dirty tracker in gfx_band_run(). */
    if (!band_is_transient()) {
        dirty_frame_sent();
    }
    gfx_present_guard_end();
}

#endif /* ESP_PLATFORM: the presentation pipeline */

void
gfx_present(void) {
    gfx_present_begin();
    gfx_present_wait();
}

void
gfx_heal_mark(int x, int y, int w, int h) {
    GFX_PRESENT_GUARD();
    (void)x;
    (void)w;
    if (gfx_heal_active()) {
        gfx_heal_queue_rows(&heal, y, y + h);
    }
}

void
gfx_heal_set_budget(int pixels_per_present) {
    GFX_PRESENT_GUARD();
    heal_budget_pixels = pixels_per_present < 0 ? 0 : pixels_per_present;
}

void
gfx_heal_set_rolling(int rows_per_present) {
    GFX_PRESENT_GUARD();
    heal_rolling_rows = rows_per_present < 0 ? 0 : rows_per_present;
}

void
gfx_heal_restore_defaults(void) {
    GFX_PRESENT_GUARD();
    gfx_heal_reset(&heal);
    heal_budget_pixels = GFX_HEAL_DEFAULT_BUDGET_PIXELS;
    heal_rolling_rows = 0;
}

bool
gfx_heal_active(void) {
    return panel_clock_requested_hz == GFX_PANEL_CLOCK_FAST_HZ;
}

bool
gfx_set_panel_clock_hz(int hz) {
    if (!panel_clock_valid(hz)) {
        return false;
    }
    panel_clock_requested_hz = hz;
    return true;
}

int
gfx_panel_clock_hz(void) {
    return panel_clock_requested_hz;
}

void
gfx_set_present_async(bool on) {
    present_async_on = on;
}

bool
gfx_present_async_enabled(void) {
    return present_async_on;
}

#if CONFIG_LAUNCHER_DEVELOPMENT
#ifdef ESP_PLATFORM
/* Every send runs on the present task (or the caller, under
 * gfx_set_present_async(false)): never call this while a present is
 * already in flight; it is a test helper, not part of the app-facing
 * pipeline, so it has no begin/wait split of its own. */
void
gfx_present_raw_full_frame_for_test(void) {
    assert(current_mode.layout == GFX_LAYOUT_FULL_FB);
    gfx_present_guard_begin();
    present_task_mode = PRESENT_TASK_RAW_FULL;
    dispatch_present();
    if (present_async_on) {
        xSemaphoreTake(present_done_sem, portMAX_DELAY);
    }
    gfx_present_guard_end();
}
#endif /* ESP_PLATFORM */
#endif
