#include "gfx/gfx.h"
#include "build_variant.h"
#include "gfx/gfx_dirty.h"
#include "gfx/gfx_fb_guard.h"
#include "gfx/gfx_font_roles.h"
#include "gfx/gfx_full_redraw.h"
#include "gfx/gfx_glow.h"
#include "gfx/gfx_heal.h"
#include "gfx/gfx_present_guard.h"
#include "gfx/gfx_target.h"
#include "util/intmath.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "board/board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

/* Carries GFX_DIRTY_WIDTH/HEIGHT for ESP-IDF independence and aligns with
 * gfx.h's BSP values. */
_Static_assert(GFX_WIDTH == GFX_DIRTY_WIDTH && GFX_HEIGHT == GFX_DIRTY_HEIGHT,
               "gfx_dirty.h's screen dimensions must match gfx.h's");
_Static_assert(GFX_HEIGHT == GFX_HEAL_SCREEN_ROWS, "gfx_heal.h's screen height must match gfx.h's");
_Static_assert(GFX_HEAL_STRIP_ROWS <= STRIP_HEIGHT, "a heal strip must fit one strip bounce slot");

#ifdef ESP_PLATFORM
static const char* TAG = "gfx";
#endif

static gfx_color_t* fb;

/* GFX_LAYOUT_FULL_FB until gfx_init() sets real geometry below, or an app's
 * gfx_mode_enter() grants something else. gfx_present_begin()/_wait() read
 * this to know whether there is a framebuffer to send at all. */
static gfx_mode_t current_mode;

/* The band ring's own buffers - declared here, not with the rest of the
 * mode/band implementation further down, so current_target() below can
 * reach them. band_render_active is true only between a successful
 * gfx_band_next() and the matching gfx_band_submit(); outside that window
 * band mode has no valid target at all, matching gfx_fb_guard.h. */
static gfx_color_t* band_buf[GFX_BAND_SLOTS];
static gfx_band_ring_t band_ring;
static int band_current_slot;
static bool band_render_active;
static int band_render_row0;
static int band_render_height;

/* gfx_band_force_all_dirty itself lives in gfx_full_redraw.h, for the same
 * reason gfx_dirty.h's all_dirty does - a host suite needs its own copy.
 * band_frame_force_all is this frame's own captured value, taken once by
 * gfx_band_frame_begin() so a later gfx_invalidate() call mid-frame
 * affects the NEXT frame, not this one. */

/* GFX_PIXFMT_INDEXED8's own state - the app writes indices, run_present_
 * indexed() below expands them through whichever LUT is installed. Not a
 * gfx_target.h render target: no drawing primitive writes through it. */
static uint8_t* indexed_image;
static int indexed_grid_w, indexed_grid_h, indexed_cell_size;
static gfx_color_t indexed_lut256[GFX_INDEXED_PALETTE_SIZE];
static bool indexed_dither16_on;

/* Lever 2: which of gfx_dither_mode_t's five is installed for 16-colour
 * mode - meaningless while indexed_dither16_on is false (256 mode keeps
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

static gfx_indexed_frame_t
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

/* True only for the RGB565 band mode, where an app's own frame() drives
 * gfx_band_next()/_submit() itself - see gfx_present_begin() below. */
static inline bool
band_is_app_driven(void) {
    return current_mode.layout == GFX_LAYOUT_BANDS && current_mode.pixfmt == GFX_PIXFMT_RGB565;
}

static bool band_frame_force_all;

/* A readback's copy of one whole band-mode frame: gfx_band_submit() fills
 * it during a frame gfx_readback_begin() forced, since the band ring keeps
 * nothing once a band is sent. PSRAM, and only while a readback is open. */
static gfx_color_t* band_snapshot;
static int band_snapshot_bands;
static bool band_snapshot_filling;
static bool band_snapshot_complete;

/* What every pixel-writing primitive below actually draws into: the whole
 * framebuffer, or the band currently being rendered - see gfx_target.h for
 * why a target carries its own row range rather than every primitive
 * checking band_render_active for itself. */
static inline gfx_target_t
current_target(void) {
    if (band_render_active) {
        return (gfx_target_t){band_buf[band_current_slot], band_render_row0, band_render_height, GFX_WIDTH};
    }
    return (gfx_target_t){fb, 0, GFX_HEIGHT, GFX_WIDTH};
}

static bool present_async_on = true;

/* The panel clock choice, written by gfx_set_panel_clock_hz() from any task.
 * The send side reopens the link at this rate before a present's first send,
 * when nothing is in flight. */
static volatile int panel_clock_requested_hz = GFX_QSPI_HZ;

/* Filled on the caller's side of a present, drained on the send side. */
static gfx_heal_t heal;
static int heal_budget_pixels = GFX_HEAL_DEFAULT_BUDGET_PIXELS;
static int heal_rolling_rows;

static bool
panel_clock_valid(int hz) {
    return hz == GFX_PANEL_CLOCK_SLOW_HZ || hz == GFX_PANEL_CLOCK_FAST_HZ;
}

#ifdef ESP_PLATFORM
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static SemaphoreHandle_t strip_sent;

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

/* Copied from the Waveshare BSP (Apache-2.0, (c) 2026 Waveshare Team),
 * where it is a private static - needed here because gfx brings the
 * panel up itself rather than calling bsp_display_new(), which offers no
 * way to reach the init sequence at all. Command 0x11 (sleep out) carries
 * a 120 ms settle, dominating a full init's cost. */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* The V2 revision (CO5300 panel). From Waveshare's own esp-idf colour-bar
 * example for this board. */
static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, NULL, 0, 100},
    {0x29, NULL, 0, 0},
};
#endif

/* Current clip rectangle, as inclusive-exclusive bounds. */
static struct {
    int x0, y0, x1, y1;
} clip;

#ifdef ESP_PLATFORM
/* Scratch space for gather_and_send(), bounded by GATHER_MAX_PIXELS,
 * allocated with MALLOC_CAP_DMA. Misalignment causes DMA errors. */
static gfx_color_t* gather_buf;

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
 * so once draw_bitmap() returns, the strip before it is off the bus. */
#define STRIP_BOUNCE_SLOTS       2
static gfx_color_t* strip_bounce[STRIP_BOUNCE_SLOTS];
static int strip_bounce_next;
#endif

/*
 * Panel plumbing - device-only. A host build never brings a panel up or
 * presents to one; see gfx_init()/gfx_present() below for the host side of
 * each.
 */

#ifdef ESP_PLATFORM
static bool IRAM_ATTR
on_strip_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* event, void* user_context) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(strip_sent, &woken);
    return woken == pdTRUE;
}

/* Common to every panel driver this file brings up: claims SPI2 for the
 * QSPI lines board.h names, with the same pad-strength opt-in either way. */
static esp_err_t
qspi_bus_up(void) {
    const spi_bus_config_t bus = {
        .sclk_io_num = BSP_LCD_PCLK,
        .data0_io_num = BSP_LCD_DATA0,
        .data1_io_num = BSP_LCD_DATA1,
        .data2_io_num = BSP_LCD_DATA2,
        .data3_io_num = BSP_LCD_DATA3,
        .max_transfer_sz = GFX_WIDTH * STRIP_HEIGHT * sizeof(gfx_color_t),
    };

    esp_err_t err = spi_bus_initialize(BSP_LCD_SPI_NUM, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

#if defined(CONFIG_LAUNCHER_GFX_QSPI_STRONG_PADS) && CONFIG_LAUNCHER_GFX_QSPI_STRONG_PADS
    /* AFTER spi_bus_initialize(), which is what configures these pads - set
     * before it and the driver overwrites the setting. Does not bring 80 MHz
     * back inside the panel's rating; see the option's help text. */
    {
        static const gpio_num_t qspi_pads[] = {
            BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
        };
        for (size_t i = 0; i < sizeof(qspi_pads) / sizeof(qspi_pads[0]); i++) {
            const esp_err_t derr = gpio_set_drive_capability(qspi_pads[i], GPIO_DRIVE_CAP_3);
            if (derr != ESP_OK) {
                ESP_LOGW(TAG, "drive capability on pad %d: %s", (int)qspi_pads[i], esp_err_to_name(derr));
            }
        }
    }
#endif
    return ESP_OK;
}

/* The panel io and driver objects at `hz`. Creating them sends nothing to
 * the panel, which is what lets a clock change reopen them without
 * re-running bring-up. */
static esp_err_t
panel_open_sh8601(int hz) {
    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, on_strip_sent, NULL);
    io_config.pclk_hz = hz;
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &panel_io);
    if (err != ESP_OK) {
        return err;
    }

    sh8601_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC, /* no dedicated reset line - see board_detect() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    return esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel);
}

static esp_err_t
panel_open_co5300(int hz) {
    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, on_strip_sent, NULL);
    io_config.pclk_hz = hz;
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &panel_io);
    if (err != ESP_OK) {
        return err;
    }

    co5300_vendor_config_t vendor = {
        .init_cmds = co5300_init_cmds,
        .init_cmds_size = sizeof(co5300_init_cmds) / sizeof(co5300_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC, /* no dedicated reset line - see board_detect() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    err = esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel);
    if (err != ESP_OK) {
        return err;
    }
    return esp_lcd_panel_set_gap(panel, BOARD_PANEL_X_GAP, 0);
}

#if CONFIG_LAUNCHER_QEMU
/* Stands in for a panel where none exists: a strip is "sent" the moment it
 * is queued, so everything above the link runs as it does on the board. */
static esp_err_t
null_panel_draw_bitmap(esp_lcd_panel_t* self, int x0, int y0, int x1, int y1, const void* pixels) {
    xSemaphoreGive(strip_sent);
    return ESP_OK;
}

static esp_err_t
null_panel_del(esp_lcd_panel_t* self) {
    return ESP_OK;
}

static esp_err_t
panel_open_null(void) {
    static esp_lcd_panel_t null_panel = {
        .draw_bitmap = null_panel_draw_bitmap,
        .del = null_panel_del,
    };
    panel = &null_panel;
    return ESP_OK;
}
#endif

static esp_err_t
panel_open(int hz) {
#if CONFIG_LAUNCHER_QEMU
    if (board_variant() == BOARD_VARIANT_UNKNOWN) {
        return panel_open_null();
    }
#endif
    ESP_LOGI(TAG, "panel QSPI at %d MHz", hz / 1000000);
    if (board_variant() == BOARD_VARIANT_CO5300_CST) {
        return panel_open_co5300(hz);
    }
    return panel_open_sh8601(hz);
}

/* SH8601 panel - the original (pre-V2) revision. */
static esp_err_t
panel_bring_up_sh8601(int hz) {
    esp_err_t err = qspi_bus_up();
    if (err != ESP_OK) {
        return err;
    }
    err = panel_open(hz);
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "on");
    return ESP_OK;
}

/* CO5300 panel - the V2 revision only. */
static esp_err_t
panel_bring_up_co5300(int hz) {
    esp_err_t err = qspi_bus_up();
    if (err != ESP_OK) {
        return err;
    }
    err = panel_open(hz);
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "on");
    return ESP_OK;
}

static int panel_clock_applied_hz;

/* VERY important: only with nothing queued on the link - deleting the io
 * waits out its transactions, but the strip_sent a caller is owed is lost. */
static void
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

/* Picks the driver the detected board revision actually needs - see
 * board_variant_t. */
static esp_err_t
panel_bring_up(int hz) {
    if (board_variant() == BOARD_VARIANT_CO5300_CST) {
        return panel_bring_up_co5300(hz);
    }
    return panel_bring_up_sh8601(hz);
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
    if (panel_bring_up(hz) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the display");
        return false;
    }
    return true;
}
#endif /* ESP_PLATFORM - panel plumbing */

#ifdef ESP_PLATFORM
/* Defined far below, alongside every other send-path function; the task
 * loop only needs to call them. */
static void run_present_normal(void);
#if CONFIG_LAUNCHER_DEVELOPMENT
static void run_present_raw_full(void);
#endif

/* Runs entirely on core 1. Bring-up happens here, once, so the strip-sent
 * interrupt esp_lcd installs lands on this core - see panel_bring_up().
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
#endif /* ESP_PLATFORM */

bool
gfx_init(void) {
#ifdef ESP_PLATFORM
    present_bringup_sem = xSemaphoreCreateBinary();
    present_done_sem = xSemaphoreCreateBinary();
    if (present_bringup_sem == NULL || present_done_sem == NULL) {
        ESP_LOGE(TAG, "Could not create the present task's semaphores");
        return false;
    }

    StackType_t* const present_stack =
        heap_caps_malloc(PRESENT_TASK_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

    /* Bring-up (board_detect(), panel_bring_up()) runs on that task - see
     * present_task_fn(). Its own ESP_LOGE already named the failure. */
    xSemaphoreTake(present_bringup_sem, portMAX_DELAY);
    if (!present_bringup_ok) {
        return false;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Framebuffer state post SD probe & panel bring-up; paired with HEAPMARK
     * in main.c. See heap_mark() comment. */
    ESP_LOGI(TAG, "HEAPMARK %-18s free %6u largest %6u", "before framebuffer",
             (unsigned)heap_caps_get_free_size(BOARD_FRAMEBUFFER_CAPS),
             (unsigned)heap_caps_get_largest_free_block(BOARD_FRAMEBUFFER_CAPS));
#endif

    /* PSRAM: the internal pool has no room for it. It never goes to the
     * panel directly - see strip_bounce. */
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = heap_caps_malloc(bytes, BOARD_FRAMEBUFFER_CAPS);
    if (fb == NULL) {
        ESP_LOGE(TAG,
                 "Could not allocate %u byte framebuffer "
                 "(largest free %s block is %u bytes)",
                 (unsigned)bytes, BOARD_FRAMEBUFFER_POOL_NAME,
                 (unsigned)heap_caps_get_largest_free_block(BOARD_FRAMEBUFFER_CAPS));
        return false;
    }

    const size_t gather_bytes = (size_t)GATHER_WINDOW_MAX_PIXELS * sizeof(gfx_color_t);
    gather_buf = heap_caps_malloc(gather_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (gather_buf == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte gather buffer", (unsigned)gather_bytes);
        return false;
    }

    const size_t strip_bytes = (size_t)GFX_WIDTH * STRIP_HEIGHT * sizeof(gfx_color_t);
    for (int i = 0; i < STRIP_BOUNCE_SLOTS; i++) {
        strip_bounce[i] = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (strip_bounce[i] == NULL) {
            ESP_LOGE(TAG, "Could not allocate %u byte strip buffer", (unsigned)strip_bytes);
            return false;
        }
    }

    current_mode.layout = GFX_LAYOUT_FULL_FB;
    current_mode.resolution = GFX_RESOLUTION_FULL;
    current_mode.width = GFX_WIDTH;
    current_mode.height = GFX_HEIGHT;
    gfx_fb_guard_set_available(true);

    gfx_clear_clip();
    gfx_mark_all_dirty();

    ESP_LOGI(TAG,
             "%dx%d framebuffer at %p, %u bytes in %s; heap free %u, "
             "largest %s block %u",
             GFX_WIDTH, GFX_HEIGHT, (void*)fb, (unsigned)bytes, BOARD_FRAMEBUFFER_POOL_NAME,
             (unsigned)esp_get_free_heap_size(), BOARD_FRAMEBUFFER_POOL_NAME,
             (unsigned)heap_caps_get_largest_free_block(BOARD_FRAMEBUFFER_CAPS));
    return true;
#else
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = malloc(bytes);
    if (fb == NULL) {
        return false;
    }
    current_mode.layout = GFX_LAYOUT_FULL_FB;
    current_mode.resolution = GFX_RESOLUTION_FULL;
    current_mode.width = GFX_WIDTH;
    current_mode.height = GFX_HEIGHT;
    gfx_fb_guard_set_available(true);
    gfx_clear_clip();
    gfx_mark_all_dirty();
    return true;
#endif
}

gfx_color_t*
gfx_framebuffer(void) {
    /* gfx can't guess intent. "Everything" wastes resources. Raw writers use
     * gfx_mark_dirty(). Be cautious. */
    GFX_PRESENT_GUARD();
    /* fb is already NULL in band mode - the right answer for a caller that
     * checks. This call is only the loud dev-time signal that one reached
     * for the framebuffer at all while it does not exist. */
    (void)GFX_REQUIRE_FRAMEBUFFER();
    return fb;
}

/* Dirty tracking */

static bool partial_clear_on;
static bool interlace_on;
#ifdef ESP_PLATFORM
static int frame_parity; /* read only inside run_present_normal(), below */
#endif
static bool prev_bbox_valid;
static int prev_bbox_x0, prev_bbox_y0, prev_bbox_x1, prev_bbox_y1;
static bool drawn_bbox_valid;
static int drawn_bbox_x0, drawn_bbox_y0, drawn_bbox_x1, drawn_bbox_y1;

void
gfx_set_partial_clear(bool on) {
    GFX_PRESENT_GUARD();
    if (!on) {
        prev_bbox_valid = false;
    }
    partial_clear_on = on;
}

bool
gfx_partial_clear_enabled(void) {
    return partial_clear_on;
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
    prev_bbox_valid = false;
    gfx_band_force_all();
}

/* Guard-free body of gfx_mark_all_dirty(), also called from the send path
 * itself (already past the guard by definition - a present is in flight)
 * when a rejected draw_bitmap() means this frame never reached the panel. */
static void
mark_all_dirty_now(void) {
    dirty_mark_all();
    drawn_bbox_valid = false;
    prev_bbox_valid = false;
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
 * on the pass that follows - see gfx_full_redraw_pending() below. Sets
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
 * not yet cleared it for the pass that follows - see
 * gfx_full_redraw_clear_pending(). */
bool
gfx_full_redraw_pending(void) {
    return gfx_full_redraw_is_pending();
}

/* Ends the window gfx_request_full_redraw() opened. The shell calls this
 * once it has read the flag and decided whether to invoke an app's
 * invalidate(), before that pass's frame() runs - see main.c's
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
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > GFX_WIDTH) {
        x1 = GFX_WIDTH;
    }
    if (y1 > GFX_HEIGHT) {
        y1 = GFX_HEIGHT;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    if (drawn_bbox_valid) {
        if (x0 < drawn_bbox_x0) {
            drawn_bbox_x0 = x0;
        }
        if (y0 < drawn_bbox_y0) {
            drawn_bbox_y0 = y0;
        }
        if (x1 > drawn_bbox_x1) {
            drawn_bbox_x1 = x1;
        }
        if (y1 > drawn_bbox_y1) {
            drawn_bbox_y1 = y1;
        }
    } else {
        drawn_bbox_x0 = x0;
        drawn_bbox_y0 = y0;
        drawn_bbox_x1 = x1;
        drawn_bbox_y1 = y1;
        drawn_bbox_valid = true;
    }
}

bool
gfx_region_dirty(int x, int y, int w, int h) {
    GFX_PRESENT_GUARD();
    (void)x;
    (void)w;
    return dirty_region_dirty(y, h);
}

/* Colour */

/* Pack 0xRRGGBB to RGB565, byte-swapped. QSPI needs high and low bytes
 * swapped, but LVGL's port doesn't handle it. */
gfx_color_t
gfx_rgb(uint32_t rgb) {
    return GFX_RGB(rgb);
}

/* Clipping */

void
gfx_set_clip(int x, int y, int w, int h) {
    GFX_PRESENT_GUARD();
    int x1 = x + w;
    int y1 = y + h;

    clip.x0 = x < 0 ? 0 : x;
    clip.y0 = y < 0 ? 0 : y;
    clip.x1 = x1 > GFX_WIDTH ? GFX_WIDTH : x1;
    clip.y1 = y1 > GFX_HEIGHT ? GFX_HEIGHT : y1;
}

void
gfx_clear_clip(void) {
    GFX_PRESENT_GUARD();
    clip.x0 = 0;
    clip.y0 = 0;
    clip.x1 = GFX_WIDTH;
    clip.y1 = GFX_HEIGHT;
}

/* Primitives */

/* Ignores clip rect; clears the whole target (a bounding box in full-fb
 * mode's partial-clear path, or the whole target buffer otherwise) and
 * marks it dirty - dirty tracking is meaningless while a band is the
 * target, since band mode resends every band every frame regardless, so
 * that half is skipped entirely there. */
void
gfx_clear(gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (!band_render_active && partial_clear_on && prev_bbox_valid) {
        for (int y = prev_bbox_y0; y < prev_bbox_y1; y++) {
            gfx_color_t* dst = fb + (size_t)y * GFX_WIDTH + prev_bbox_x0;
            for (int x = prev_bbox_x0; x < prev_bbox_x1; x++) {
                *dst++ = color;
            }
        }
        dirty_mark(prev_bbox_x0, prev_bbox_y0, prev_bbox_x1 - prev_bbox_x0, prev_bbox_y1 - prev_bbox_y0);
        drawn_bbox_valid = false;
        return;
    }

    const gfx_target_t target = current_target();
    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t* words = (uint32_t*)target.buf;
    const int count = (target.stride * target.height) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
    }

    if (!band_render_active) {
        gfx_mark_all_dirty();
    }
}

void
gfx_pixel(int x, int y, gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (x < clip.x0 || x >= clip.x1) {
        return;
    }
    const gfx_target_t target = current_target();
    int y0 = y, y1 = y + 1;
    gfx_target_clip_y(target, clip.y0, clip.y1, &y0, &y1);
    if (y0 >= y1) {
        return;
    }
    gfx_target_row(target, y)[x] = color;
    if (!band_render_active) {
        mark_band(y, y + 1);
    }
}

/* Cohen-Sutherland outcodes: one bit per edge the point lies outside of. */
enum { OUT_LEFT = 1, OUT_RIGHT = 2, OUT_TOP = 4, OUT_BOTTOM = 8 };

static int
outcode(int x, int y) {
    int code = 0;
    if (x < clip.x0) {
        code |= OUT_LEFT;
    } else if (x >= clip.x1) {
        code |= OUT_RIGHT;
    }
    if (y < clip.y0) {
        code |= OUT_TOP;
    } else if (y >= clip.y1) {
        code |= OUT_BOTTOM;
    }
    return code;
}

/* Clipping affects error term, differs by pixel. Caller takes fast path if
 * both ends inside. */
static bool
clip_line(int* x0, int* y0, int* x1, int* y1) {
    int c0 = outcode(*x0, *y0);
    int c1 = outcode(*x1, *y1);

    for (int pass = 0; pass < 8; pass++) {
        if ((c0 | c1) == 0) {
            return true; /* both ends inside */
        }
        if ((c0 & c1) != 0) {
            return false; /* both beyond the same edge */
        }

        const int out = c0 ? c0 : c1;
        int x, y;

        /* Clips to last pixel inside, not boundary. */
        if (out & OUT_BOTTOM) {
            y = clip.y1 - 1;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & OUT_TOP) {
            y = clip.y0;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & OUT_RIGHT) {
            x = clip.x1 - 1;
            y = *y0 + (int)(((int64_t)(*y1 - *y0) * (x - *x0)) / (*x1 - *x0));
        } else {
            x = clip.x0;
            y = *y0 + (int)(((int64_t)(*y1 - *y0) * (x - *x0)) / (*x1 - *x0));
        }

        if (out == c0) {
            *x0 = x;
            *y0 = y;
            c0 = outcode(x, y);
        } else {
            *x1 = x;
            *y1 = y;
            c1 = outcode(x, y);
        }
    }
    return false;
}

/* One pixel of a line. */
static void
plot(int x, int y, gfx_color_t color, unsigned flags) {
    if (x < clip.x0 || x >= clip.x1) {
        return;
    }
    const gfx_target_t target = current_target();
    int y0 = y, y1 = y + 1;
    gfx_target_clip_y(target, clip.y0, clip.y1, &y0, &y1);
    if (y0 >= y1) {
        return;
    }
    gfx_color_t* const dst = &gfx_target_row(target, y)[x];

    *dst = (flags & GFX_LINE_ADD) ? gfx_color_add(*dst, color) : color;
}

/* Bresenham, treats both axes alike, no case analysis. */
static void
walk(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    const int dx = im_abs(x1 - x0);
    const int dy = -im_abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    bool first = true;

    for (;;) {
        if (!(first && (flags & GFX_LINE_OPEN))) {
            plot(x0, y0, color, flags);
        }
        first = false;

        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Box intersects clip, marked once. Overestimating costs bus time;
 * underestimating leaves stale. */
static void
draw_line(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    /* Only pay for clipping when some of the line is actually outside. */
    if (outcode(x0, y0) | outcode(x1, y1)) {
        if (!clip_line(&x0, &y0, &x1, &y1)) {
            return;
        }
    }

    int bx0 = im_min(x0, x1), bx1 = im_max(x0, x1) + 1;
    int by0 = im_min(y0, y1), by1 = im_max(y0, y1) + 1;

    if (bx0 < clip.x0) {
        bx0 = clip.x0;
    }
    if (by0 < clip.y0) {
        by0 = clip.y0;
    }
    if (bx1 > clip.x1) {
        bx1 = clip.x1;
    }
    if (by1 > clip.y1) {
        by1 = clip.y1;
    }

    walk(x0, y0, x1, y1, color, flags);

    if (!band_render_active && bx0 < bx1 && by0 < by1) {
        dirty_mark(bx0, by0, bx1 - bx0, by1 - by0);
    }
}

void
gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    draw_line(x0, y0, x1, y1, color, 0);
}

void
gfx_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    draw_line(x0, y0, x1, y1, color, flags);
}

void
gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    int x0, y0, x1, y1;
    gfx_target_fill_rect(current_target(), clip.x0, clip.y0, clip.x1, clip.y1, x, y, w, h, color, &x0, &y0, &x1, &y1);

    if (!band_render_active) {
        mark_band(y0, y1); /* already clipped above */
    }
}

/*
 * Dithered fake transparency
 *
 * gfx_fill_rect_blend() (further down) is a REAL per-pixel blend, but pays
 * for a framebuffer read - affordable at glyph scale, not a whole frame
 * (its own comment). Dithering fakes transparency instead: ordered (Bayer)
 * dithering picks WHICH pixels to draw via a per-pixel threshold, no
 * framebuffer read, no float math.
 */

/* gfx_fill_rect() uses `alpha` (0-255) for coverage, avoiding framebuffer
 * reads. gfx_dither_covers() in gfx_color.h. Returns if 0. */
void
gfx_fill_rect_dither(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (alpha == 0) {
        return;
    }

    const gfx_target_t target = current_target();
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip.x0) {
        x0 = clip.x0;
    }
    if (x1 > clip.x1) {
        x1 = clip.x1;
    }
    gfx_target_clip_y(target, clip.y0, clip.y1, &y0, &y1);

    for (int row = y0; row < y1; row++) {
        gfx_color_t* dst = gfx_target_row(target, row);
        for (int col = x0; col < x1; col++) {
            if (gfx_dither_covers(col, row, alpha)) {
                dst[col] = color;
            }
        }
    }

    if (!band_render_active) {
        mark_band(y0, y1);
    }
}

/* Per-pixel blend, reads framebuffer. Efficient for glyphs, not full-frame.
 * Alpha 0 no-op, 255 matches gfx_fill_rect(). */
void
gfx_fill_rect_blend(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (alpha == 0) {
        return;
    }

    const gfx_target_t target = current_target();
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip.x0) {
        x0 = clip.x0;
    }
    if (x1 > clip.x1) {
        x1 = clip.x1;
    }
    gfx_target_clip_y(target, clip.y0, clip.y1, &y0, &y1);

    for (int row = y0; row < y1; row++) {
        gfx_color_t* dst = gfx_target_row(target, row);
        for (int col = x0; col < x1; col++) {
            dst[col] = gfx_color_mix(dst[col], color, alpha);
        }
    }

    if (!band_render_active) {
        mark_band(y0, y1);
    }
}

void
gfx_glow_curve(const int16_t* y_q4, int count, int x0, int x1, int quarter_turns, int erase_px,
               const gfx_glow_style_t* style) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    const gfx_target_t target = current_target();
    x0 = im_max(x0, 0);
    x1 = im_min(x1, count);
    for (int chunk = x0; chunk < x1; chunk += GFX_GLOW_CHUNK) {
        const gfx_glow_box_t box =
            gfx_glow_draw_columns(target, clip.x0, clip.y0, clip.x1, clip.y1, GFX_WIDTH, GFX_HEIGHT, y_q4, count, chunk,
                                  im_min(chunk + GFX_GLOW_CHUNK, x1), quarter_turns, erase_px, style);
        if (!band_render_active && box.x1 > box.x0) {
            gfx_mark_dirty(box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
        }
    }
}

/* Cheap by construction, not by luck: alpha is one value for the whole
 * call, and the Bayer pattern repeats every 4 pixels, so the per-pixel
 * decision collapses to four booleans per row - a fully-covered row is a
 * plain memcpy, an untouched row costs nothing. Phase-locked to absolute
 * panel coordinates like every other dithered draw in gfx.h, so
 * overlapping dithered shapes stay in register with each other. First
 * user: the boot animation's photograph crossfade (boot_anim.c's
 * draw_image()). */
void
gfx_blit_dither(int x, int y, int w, int h, const gfx_color_t* src, int src_stride, uint8_t alpha) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (alpha == 0) {
        return;
    }

    const gfx_target_t target = current_target();
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip.x0) {
        x0 = clip.x0;
    }
    if (x1 > clip.x1) {
        x1 = clip.x1;
    }
    gfx_target_clip_y(target, clip.y0, clip.y1, &y0, &y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    const int level = gfx_dither_level(alpha);

    for (int row = y0; row < y1; row++) {
        const uint8_t* cells = gfx_dither4x4[row & 3];
        const bool p[4] = {level > cells[0], level > cells[1], level > cells[2], level > cells[3]};

        if (!p[0] && !p[1] && !p[2] && !p[3]) {
            continue;
        }

        gfx_color_t* dst = gfx_target_row(target, row);
        const gfx_color_t* s = src + (size_t)(row - y) * (size_t)src_stride + (x0 - x);

        if (p[0] && p[1] && p[2] && p[3]) {
            memcpy(dst + x0, s, (size_t)(x1 - x0) * sizeof *dst);
            continue;
        }

        int col = x0;
        gfx_color_t* dp = dst + col;
        const gfx_color_t* sp = s;
        for (; col < x1 && (col & 3) != 0; col++, dp++, sp++) {
            if (p[col & 3]) {
                *dp = *sp;
            }
        }
        for (; col + 4 <= x1; col += 4, dp += 4, sp += 4) {
            if (p[0]) {
                dp[0] = sp[0];
            }
            if (p[1]) {
                dp[1] = sp[1];
            }
            if (p[2]) {
                dp[2] = sp[2];
            }
            if (p[3]) {
                dp[3] = sp[3];
            }
        }
        for (; col < x1; col++, dp++, sp++) {
            if (p[col & 3]) {
                *dp = *sp;
            }
        }
    }

    if (!band_render_active) {
        mark_band(y0, y1);
    }
}

/*
 * Text
 *
 * One font-aware path (gfx_text_font(), gfx_font_width()) everything else
 * delegates to, passing gfx_font_ui(). See gfx_font.h for why gfx_font_t
 * exists: honouring microui's mu_Font is a later task needing a font to
 * point AT. gfx_font_ui() (gfx_font_roles.h) wraps font8x8_basic.h's
 * public-domain bitmap data (gfx_font_8x8's comment, gfx_font.h).
 */

void
gfx_text(int x, int y, const char* text, gfx_color_t color) {
    gfx_text_scaled(x, y, text, color, GFX_GLYPH_SCALE);
}

void
gfx_text_scaled(int x, int y, const char* text, gfx_color_t color, int scale) {
    gfx_text_turned(x, y, text, color, scale, 0);
}

/* One glyph pixel, solid. Coverage varies per pixel in an 8bpp atlas, so
 * draw_glyph_font()'s 8bpp path still draws one at a time; its 1bpp path
 * batches runs instead (gfx_font_row_run_rect()). */
static void
draw_rotated_font_pixel(const gfx_font_t* font, int x, int y, int row, int col, int scale, int turn,
                        gfx_color_t color) {
    int px, py;
    switch (turn) {
        case 1:
            px = font->cell_h - 1 - row;
            py = col;
            break;
        case 2:
            px = font->cell_w - 1 - col;
            py = font->cell_h - 1 - row;
            break;
        case 3:
            px = row;
            py = font->cell_w - 1 - col;
            break;
        default:
            px = col;
            py = row;
            break;
    }
    gfx_fill_rect(x + px * scale, y + py * scale, scale, scale, color);
}

/* 8bpp atlas; 0-255 coverage; blends via gfx_fill_rect_blend() instead of
 * solid. */
static void
draw_rotated_font_pixel_blend(const gfx_font_t* font, int x, int y, int row, int col, int scale, int turn,
                              gfx_color_t color, uint8_t coverage) {
    int px, py;
    switch (turn) {
        case 1:
            px = font->cell_h - 1 - row;
            py = col;
            break;
        case 2:
            px = font->cell_w - 1 - col;
            py = font->cell_h - 1 - row;
            break;
        case 3:
            px = row;
            py = font->cell_w - 1 - col;
            break;
        default:
            px = col;
            py = row;
            break;
    }
    gfx_fill_rect_blend(x + px * scale, y + py * scale, scale, scale, color, coverage);
}

/* Draws `font` glyph or nothing if `ch` is out of range or `font->bpp`
 * unsupported. Use separate loops for layouts. */
static void
draw_glyph_font(const gfx_font_t* font, int x, int y, unsigned char ch, gfx_color_t color, int scale, int turn) {
    if (ch < font->first || (unsigned)(ch - font->first) >= font->count) {
        return;
    }

    if (font->bpp == 1) {
        /* One filled rect per coalesced box of set bits, not one per run
         * per row - gfx_font_glyph_run_boxes() merges a vertical stroke's
         * identical run across every row it spans into one box, so
         * gfx_font_run_box_rect() covers it with one gfx_fill_rect() call
         * regardless of which glyph axis a turn maps onto the screen's
         * narrow one. */
        gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
        const int n = gfx_font_glyph_run_boxes(font, ch, boxes, GFX_FONT_RUN_BOXES_MAX);
        for (int i = 0; i < n; i++) {
            int rx, ry, rw, rh;
            gfx_font_run_box_rect(font, x, y, boxes[i].row0, boxes[i].row1, boxes[i].col0, boxes[i].col1, scale, turn,
                                  &rx, &ry, &rw, &rh);
            gfx_fill_rect(rx, ry, rw, rh, color);
        }
        return;
    }

    if (font->bpp == 8) {
        const size_t cell_pixels = (size_t)font->cell_w * font->cell_h;
        const uint8_t* glyph = font->atlas + (size_t)(ch - font->first) * cell_pixels;

        for (int row = 0; row < font->cell_h; row++) {
            const uint8_t* glyph_row = glyph + (size_t)row * font->cell_w;
            for (int col = 0; col < font->cell_w; col++) {
                const uint8_t coverage = glyph_row[col];
                if (coverage == 0) {
                    continue;
                }
                draw_rotated_font_pixel_blend(font, x, y, row, col, scale, turn, color, coverage);
            }
        }
        return;
    }
}

/* draw_glyph_font()'s bpp==1 path, halo variant: each run is
 * gfx_font_row_run_rect_dilated() instead of gfx_font_row_run_rect() -
 * see that function's own comment for why this covers the same area as
 * UI_TEXT_OUTLINED's 8 unit-offset copies. Never called for a bpp==8
 * font - see gfx_text_font_halo()'s own comment. */
static void
draw_glyph_font_halo(const gfx_font_t* font, int x, int y, unsigned char ch, gfx_color_t color, int scale, int turn) {
    if (ch < font->first || (unsigned)(ch - font->first) >= font->count) {
        return;
    }
    assert(font->bpp == 1);

    gfx_font_run_box_t boxes[GFX_FONT_RUN_BOXES_MAX];
    const int n = gfx_font_glyph_run_boxes(font, ch, boxes, GFX_FONT_RUN_BOXES_MAX);
    for (int i = 0; i < n; i++) {
        int rx, ry, rw, rh;
        gfx_font_run_box_rect_dilated(font, x, y, boxes[i].row0, boxes[i].row1, boxes[i].col0, boxes[i].col1, scale,
                                      turn, &rx, &ry, &rw, &rh);
        gfx_fill_rect(rx, ry, rw, rh, color);
    }
}

void
gfx_text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns, const gfx_font_t* font) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    static const int step[4][2] = {
        {1, 0},  /* upright:        left to right */
        {0, 1},  /* quarter turn:   top to bottom */
        {-1, 0}, /* upside down:    right to left */
        {0, -1}, /* three quarters: bottom to top */
    };

    /* A quarter turn of 1 or 3 swaps which cell dimension becomes the
     * on-screen row extent - see gfx_font_row_run_rect()'s own comment. */
    const int char_h = (turn & 1) ? font->cell_w * scale : font->cell_h * scale;
    const gfx_target_t target = current_target();

    for (const char* p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        /* One command replayed into several bands (ui_replay_band()) walks
         * every character again per band; skipping one whose own row
         * extent misses the current target entirely turns that back into
         * one walk's worth of work overall, the same as it costs in
         * GFX_LAYOUT_FULL_FB, where the target spans the full screen and
         * this is never false. */
        if (gfx_target_row_range_overlaps(target, y, y + char_h)) {
            draw_glyph_font(font, x, y, ch, color, scale, turn);
        }
        const int adv = gfx_font_advance(font, ch, scale);
        x += step[turn][0] * adv;
        y += step[turn][1] * adv;
    }
}

void
gfx_text_turned(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns) {
    gfx_text_font(x, y, text, color, scale, quarter_turns, gfx_font_ui());
}

/* gfx_text_font()'s own loop, drawing each character's halo
 * (draw_glyph_font_halo()) rather than its ink - see gfx.h's own comment.
 * UI_TEXT_OUTLINED is the only caller and only ever styles gfx_font_ui(),
 * a bpp==1 font - draw_glyph_font_halo() asserts that rather than
 * drawing a bpp==8 font's halo wrong. */
void
gfx_text_font_halo(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                   const gfx_font_t* font) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    static const int step[4][2] = {
        {1, 0},
        {0, 1},
        {-1, 0},
        {0, -1},
    };

    const int char_h = (turn & 1) ? font->cell_w * scale : font->cell_h * scale;
    const gfx_target_t target = current_target();

    for (const char* p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        /* y - 1, + 1: the halo reaches one pixel beyond the ink on every
         * side (gfx_font_row_run_rect_dilated()), so the row range this
         * character can possibly touch is one pixel taller too. */
        if (gfx_target_row_range_overlaps(target, y - 1, y + char_h + 1)) {
            draw_glyph_font_halo(font, x, y, ch, color, scale, turn);
        }
        const int adv = gfx_font_advance(font, ch, scale);
        x += step[turn][0] * adv;
        y += step[turn][1] * adv;
    }
}

/*
 * A second copy of the three glyph functions rather than one core threaded
 * with an alpha parameter, deliberately: gfx_text_font() is the single
 * font-aware path every text call in the tree goes through, and keeping it
 * provably unchanged beats trusting a compiler to fold an `alpha == 255`
 * check back out of it at every call site forever.
 */

static void
draw_rotated_font_pixel_dither(const gfx_font_t* font, int x, int y, int row, int col, int scale, int turn,
                               gfx_color_t color, uint8_t alpha) {
    int px, py;
    switch (turn) {
        case 1:
            px = font->cell_h - 1 - row;
            py = col;
            break;
        case 2:
            px = font->cell_w - 1 - col;
            py = font->cell_h - 1 - row;
            break;
        case 3:
            px = row;
            py = font->cell_w - 1 - col;
            break;
        default:
            px = col;
            py = row;
            break;
    }
    gfx_fill_rect_dither(x + px * scale, y + py * scale, scale, scale, color, alpha);
}

static void
draw_glyph_font_dither(const gfx_font_t* font, int x, int y, unsigned char ch, gfx_color_t color, int scale, int turn,
                       uint8_t alpha) {
    if (ch < font->first || (unsigned)(ch - font->first) >= font->count) {
        return;
    }

    if (font->bpp == 1) {
        const uint8_t* glyph = font->atlas + (size_t)(ch - font->first) * font->cell_h;

        for (int row = 0; row < font->cell_h; row++) {
            const uint8_t bits = glyph[row];
            if (bits == 0) {
                continue;
            }
            for (int col = 0; col < font->cell_w; col++) {
                if (bits & (1 << col)) {
                    draw_rotated_font_pixel_dither(font, x, y, row, col, scale, turn, color, alpha);
                }
            }
        }
        return;
    }

    if (font->bpp == 8) {
        const size_t cell_pixels = (size_t)font->cell_w * font->cell_h;
        const uint8_t* glyph = font->atlas + (size_t)(ch - font->first) * cell_pixels;

        for (int row = 0; row < font->cell_h; row++) {
            const uint8_t* glyph_row = glyph + (size_t)row * font->cell_w;
            for (int col = 0; col < font->cell_w; col++) {
                const uint8_t coverage = glyph_row[col];
                if (coverage == 0) {
                    continue;
                }
                const uint8_t folded = coverage < alpha ? coverage : alpha;
                draw_rotated_font_pixel_dither(font, x, y, row, col, scale, turn, color, folded);
            }
        }
        return;
    }
}

/* gfx_text_font() with dithered glyphs for translucent effect. Used in
 * boot_anim.c for title shadow. */
void
gfx_text_font_dither(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                     const gfx_font_t* font, uint8_t alpha) {
    GFX_PRESENT_GUARD();
    if (!GFX_REQUIRE_FRAMEBUFFER()) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    static const int step[4][2] = {
        {1, 0},
        {0, 1},
        {-1, 0},
        {0, -1},
    };

    for (const char* p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        draw_glyph_font_dither(font, x, y, ch, color, scale, turn, alpha);
        const int adv = gfx_font_advance(font, ch, scale);
        x += step[turn][0] * adv;
        y += step[turn][1] * adv;
    }
}

/* Present */

#if CONFIG_LAUNCHER_DEVELOPMENT
/* See gfx.h for "why not always compiled". Used by gfx_set_debug_overlay()
 * below. */
static bool debug_overlay_on;

bool
gfx_debug_overlay(void) {
    return debug_overlay_on;
}

static bool leaf_overlay_on;

bool
gfx_debug_leaf_overlay(void) {
    return leaf_overlay_on;
}

static inline bool
overlay_any_on(void) {
    return debug_overlay_on || leaf_overlay_on;
}

/* See send_partial_band() for third path. Exists for device test. Not reset
 * by gfx_present(). */
static int dev_strips_sent_full;
static int dev_strips_sent_gathered;
static int dev_strips_sent_partial;

/* Actual panel-format bytes queued, every send path alike (full-fb gather/
 * strip, and GFX_PIXFMT_INDEXED8's own whole-strip send) - what the three
 * counts above cannot answer by themselves for a mode with no strip/gather
 * distinction at all. Exists for a device test comparing send cost across
 * pixel formats. Not reset by gfx_present(). */
static int64_t dev_bytes_sent;
static int64_t dev_heal_bytes_sent;

void
gfx_reset_strip_send_counts(void) {
    dev_strips_sent_full = 0;
    dev_strips_sent_gathered = 0;
    dev_strips_sent_partial = 0;
    dev_bytes_sent = 0;
    dev_heal_bytes_sent = 0;
}

void
gfx_get_strip_send_counts(int* full_bands, int* gathered, int* partial_bands) {
    if (full_bands) {
        *full_bands = dev_strips_sent_full;
    }
    if (gathered) {
        *gathered = dev_strips_sent_gathered;
    }
    if (partial_bands) {
        *partial_bands = dev_strips_sent_partial;
    }
}

int64_t
gfx_get_heal_bytes_sent(void) {
    return dev_heal_bytes_sent;
}

int64_t
gfx_get_bytes_sent(void) {
    return dev_bytes_sent;
}

static void
mark_rect_border(gfx_color_t* buf, int stride, int w, int h, gfx_color_t colour) {
    for (int col = 0; col < w; col++) {
        buf[col] = colour;
        buf[(size_t)(h - 1) * stride + col] = colour;
    }
    for (int row = 0; row < h; row++) {
        buf[(size_t)row * stride] = colour;
        buf[(size_t)row * stride + (w - 1)] = colour;
    }
}

/* Used for full-width send border. No scratch copy - direct to fb. Inverse
 * save/restore. */
#define BORDER_PIXELS      (2 * (COL_WIDTH + STRIP_HEIGHT))

#define LEAF_BORDER_PIXELS (2 * (LEAF_W + LEAF_H))

static void
save_border(const gfx_color_t* buf, int stride, int w, int h, gfx_color_t* out) {
    int i = 0;
    for (int col = 0; col < w; col++) {
        out[i++] = buf[col];
        out[i++] = buf[(size_t)(h - 1) * stride + col];
    }
    for (int row = 0; row < h; row++) {
        out[i++] = buf[(size_t)row * stride];
        out[i++] = buf[(size_t)row * stride + (w - 1)];
    }
}

static void
restore_border(gfx_color_t* buf, int stride, int w, int h, const gfx_color_t* saved) {
    int i = 0;
    for (int col = 0; col < w; col++) {
        buf[col] = saved[i++];
        buf[(size_t)(h - 1) * stride + col] = saved[i++];
    }
    for (int row = 0; row < h; row++) {
        buf[(size_t)row * stride] = saved[i++];
        buf[(size_t)row * stride + (w - 1)] = saved[i++];
    }
}

/* 6240 px combined, borrowed from gather_buf's front rather than
 * malloc'd separately: a separate allocation competes for the one
 * contiguous 41 KB block an app needs, and a debug overlay must
 * never be why an app cannot start. gather_buf is idle here -
 * gather_and_send() waits for its own transfer before returning, and the
 * frame loop is single-threaded. The _Static_assert ties the fit to
 * GATHER_MAX_PIXELS, so breaking it is a compile error rather than a
 * silent DMA overflow. */
#define OVERLAY_CELL_SAVE_PIXELS (GRID_COLS * BORDER_PIXELS)
#define OVERLAY_LEAF_SAVE_PIXELS (LEAF_RECTS_PER_ROW_MAX * LEAF_BORDER_PIXELS)
_Static_assert(OVERLAY_CELL_SAVE_PIXELS + OVERLAY_LEAF_SAVE_PIXELS <= GATHER_MAX_PIXELS,
               "overlay save/restore scratch must fit inside gather_buf");

static inline gfx_color_t (*overlay_cell_save(void))[BORDER_PIXELS] {
    return (gfx_color_t(*)[BORDER_PIXELS])gather_buf;
}

static inline gfx_color_t (*overlay_leaf_save(void))[LEAF_BORDER_PIXELS] {
    return (gfx_color_t(*)[LEAF_BORDER_PIXELS])(gather_buf + OVERLAY_CELL_SAVE_PIXELS);
}

/* Shared by send_full_row() and gather_and_send(), never live at once:
 * the frame loop is single-threaded. Unlike the cell/leaf scratch above,
 * this cannot borrow gather_buf - which IS the destination leaf borders
 * draw into using this list's rects, so it must survive alongside
 * gather_buf, not overlap it. Malloc'd on enable/disable (1024 bytes)
 * rather than static: too big for app_main()'s stack, and a permanent
 * .bss reservation fares no better given this repo's history of
 * static-growth OOMs. */
static dirty_leaf_rect_t* leaf_rect_scratch;

void
gfx_set_debug_overlay(bool on) {
    GFX_PRESENT_GUARD();
    debug_overlay_on = on;
}

void
gfx_set_leaf_overlay(bool on) {
    GFX_PRESENT_GUARD();
    if (on) {
        if (leaf_rect_scratch == NULL) {
            leaf_rect_scratch = malloc(sizeof(*leaf_rect_scratch) * LEAF_RECTS_PER_ROW_MAX);
            if (leaf_rect_scratch == NULL) {
                ESP_LOGE(TAG,
                         "overlay: could not allocate %u-byte leaf "
                         "rect scratch - staying off",
                         (unsigned)(sizeof(*leaf_rect_scratch) * LEAF_RECTS_PER_ROW_MAX));
                return;
            }
        }
        leaf_overlay_on = true;
    } else {
        leaf_overlay_on = false;
        free(leaf_rect_scratch);
        leaf_rect_scratch = NULL;
    }
}
#endif

/* Device-only path for QSPI panel send. Host build is no-op. */
#ifdef ESP_PLATFORM

#if CONFIG_LAUNCHER_DEVELOPMENT
/* `send_shadow` holds every pixel handed to the panel, so after a present
 * it must equal `fb`. A difference is a pixel never sent, or one copied out
 * of PSRAM wrong (also counted on its own). A glitch this stays silent on
 * lies past the send buffers, on the QSPI link. */
static gfx_color_t* send_shadow;
static bool send_audit_primed;
static int64_t send_audit_uncovered_px;
static int64_t send_audit_copy_fault_px;
static int send_audit_first_x = -1, send_audit_first_y = -1;
static int64_t send_audit_log_at_us;

#define SEND_AUDIT_LOG_US 1000000

bool
gfx_send_audit(void) {
    return send_shadow != NULL;
}

void
gfx_set_send_audit(bool on) {
    GFX_PRESENT_GUARD();
    if (on == (send_shadow != NULL)) {
        return;
    }
    if (!on) {
        heap_caps_free(send_shadow);
        send_shadow = NULL;
        return;
    }
    send_shadow = heap_caps_malloc((size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t), BOARD_FRAMEBUFFER_CAPS);
    if (send_shadow == NULL) {
        ESP_LOGE(TAG, "send audit: no room for the shadow framebuffer - staying off");
        return;
    }
    send_audit_primed = false;
}

static int
count_differing_px(const gfx_color_t* a, const gfx_color_t* b, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        count += a[i] != b[i];
    }
    return count;
}

/* `buf` is what is about to go to the panel for [x0, x0+w) x [y0, y0+h),
 * already copied out of `fb`. */
static void
send_audit_capture(int x0, int y0, int w, int h, const gfx_color_t* buf) {
    if (send_shadow == NULL) {
        return;
    }
    for (int r = 0; r < h; r++) {
        const gfx_color_t* src = buf + (size_t)r * w;
        const gfx_color_t* fb_row = fb + (size_t)(y0 + r) * GFX_WIDTH + x0;
        if (memcmp(src, fb_row, (size_t)w * sizeof(gfx_color_t)) != 0) {
            send_audit_copy_fault_px += count_differing_px(src, fb_row, w);
        }
        memcpy(send_shadow + (size_t)(y0 + r) * GFX_WIDTH + x0, src, (size_t)w * sizeof(gfx_color_t));
    }
}

/* Runs after a full-framebuffer present has drained, while `fb` still
 * holds exactly what that present was meant to send. An overlay or
 * interlace legitimately leaves the shadow out of step, so the audit
 * pauses under either and re-primes after: everything is marked dirty and
 * the next present resends it. */
static void
send_audit_scan_row(int y) {
    const gfx_color_t* fb_row = fb + (size_t)y * GFX_WIDTH;
    const gfx_color_t* shadow_row = send_shadow + (size_t)y * GFX_WIDTH;
    if (memcmp(fb_row, shadow_row, (size_t)GFX_WIDTH * sizeof(gfx_color_t)) == 0) {
        return;
    }
    for (int x = 0; x < GFX_WIDTH; x++) {
        if (fb_row[x] == shadow_row[x]) {
            continue;
        }
        if (send_audit_first_x < 0) {
            send_audit_first_x = x;
            send_audit_first_y = y;
        }
        send_audit_uncovered_px++;
        /* Resent in full next time, so one gap is counted once. */
        send_audit_primed = false;
    }
}

static void
send_audit_log_if_due(void) {
    const int64_t now = esp_timer_get_time();
    if (now < send_audit_log_at_us || (send_audit_uncovered_px == 0 && send_audit_copy_fault_px == 0)) {
        return;
    }
    send_audit_log_at_us = now + SEND_AUDIT_LOG_US;
    ESP_LOGW(TAG,
             "send audit: %lld px on the panel differ from fb (first at %d,%d), %lld px read back wrong from PSRAM",
             (long long)send_audit_uncovered_px, send_audit_first_x, send_audit_first_y,
             (long long)send_audit_copy_fault_px);
    send_audit_uncovered_px = 0;
    send_audit_copy_fault_px = 0;
    send_audit_first_x = -1;
    send_audit_first_y = -1;
}

static void
send_audit_check(void) {
    if (send_shadow == NULL) {
        return;
    }
    if (overlay_any_on() || interlace_on) {
        send_audit_primed = false;
        return;
    }
    if (!send_audit_primed) {
        send_audit_primed = true;
        dirty_mark_all();
        return;
    }

    for (int y = 0; y < GFX_HEIGHT; y++) {
        send_audit_scan_row(y);
    }

    send_audit_log_if_due();
}
#endif

/* A rejected esp_lcd_panel_draw_bitmap() queues nothing, so its strip_sent
 * give never comes - every send site below checks this instead of taking
 * the semaphore unconditionally. present_send_failed lets one present
 * notice a mid-frame rejection and force a full resend once, at the end,
 * rather than re-deriving which region was affected at each call site. */
static bool present_send_failed;

#if CONFIG_LAUNCHER_DEVELOPMENT
#define SEND_FAILURE_KINDS_MAX 4
static esp_err_t send_failure_kinds[SEND_FAILURE_KINDS_MAX];
static int send_failure_kind_count;
#endif

static void
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

/* gather_buf is shared and about to be overwritten, so every queued
 * transfer, not just the most recent, must drain first. strip_sent is a
 * plain counter with no transfer identity: taking it once is not the
 * same as waiting for THIS gather, since whichever transfer finishes
 * first satisfies whichever Take() runs. Draining exactly *queued first
 * empties the queue, so the one Take() after this draw_bitmap()
 * unambiguously waits for it - SPI transactions on one device complete
 * in queued order. */
static void
gather_and_send(int x0, int y0, int x1, int y1, int row, int run_start, int run_end, bool refined, int* queued,
                gfx_color_t border) {
    x0 = even_floor(x0);
    y0 = even_floor(y0);
    x1 = even_ceil(x1);
    y1 = even_ceil(y1);
    const int w = x1 - x0;
    const int h = y1 - y0;

    for (int j = 0; j < *queued; j++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
    *queued = 0;

    for (int r = 0; r < h; r++) {
        memcpy(gather_buf + (size_t)r * w, fb + (size_t)(y0 + r) * GFX_WIDTH + x0, (size_t)w * sizeof(gfx_color_t));
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    send_audit_capture(x0, y0, w, h, gather_buf);
    if (debug_overlay_on && refined) {
        /* One border around the whole packed box. */
        mark_rect_border(gather_buf, w, w, h, border);
    } else if (debug_overlay_on) {
        for (int col = run_start; col < run_end; col++) {
            const int idx = row * GRID_COLS + col;
            gfx_color_t* at = gather_buf + (size_t)(cell_y0[idx] - y0) * w + (cell_x0[idx] - x0);
            mark_rect_border(at, w, cell_x1[idx] - cell_x0[idx], cell_y1[idx] - cell_y0[idx], border);
        }
    }
    if (leaf_overlay_on) {
        const int n = dirty_leaf_rects(row, x0, y0, x1, y1, leaf_rect_scratch, LEAF_RECTS_PER_ROW_MAX);
        for (int i = 0; i < n; i++) {
            const dirty_leaf_rect_t* r = &leaf_rect_scratch[i];
            gfx_color_t* at = gather_buf + (size_t)(r->y0 - y0) * w + (r->x0 - x0);
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
    const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, gather_buf);
    if (err != ESP_OK) {
        note_send_failure(err);
        return;
    }
    xSemaphoreTake(strip_sent, portMAX_DELAY);
}

/* Queues framebuffer rows [y0, y1) through the next strip_bounce slot. At
 * most STRIP_HEIGHT rows. Returns whether the panel accepted the transfer -
 * false means no strip_sent give is coming for it. */
static bool
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

/* send_fb_rows()'s GFX_PIXFMT_INDEXED8 counterpart: expands rows [y0, y1)
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
    /* leaf_rect_scratch never NULL: gfx_set_leaf_overlay() allocates */
    int leaf_n = 0;
    if (leaf_overlay_on) {
        leaf_n = dirty_leaf_rects(row, 0, y, GFX_WIDTH, y + STRIP_HEIGHT, leaf_rect_scratch, LEAF_RECTS_PER_ROW_MAX);
    }

    /* Cyan borders, green leaves, or both. Skip if leaf_overlay_on and no
     * dirty leaves. Transfer immediately for debugging. */
    if (debug_overlay_on || leaf_n > 0) {
        /* Save phase: prevent overwriting shared pixels. */
        if (debug_overlay_on) {
            for (int col = 0; col < GRID_COLS; col++) {
                gfx_color_t* cell = fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
                save_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, overlay_cell_save()[col]);
            }
        }
        for (int i = 0; i < leaf_n; i++) {
            const dirty_leaf_rect_t* r = &leaf_rect_scratch[i];
            gfx_color_t* at = fb + (size_t)r->y0 * GFX_WIDTH + r->x0;
            save_border(at, GFX_WIDTH, r->x1 - r->x0, r->y1 - r->y0, overlay_leaf_save()[i]);
        }

        if (debug_overlay_on) {
            for (int col = 0; col < GRID_COLS; col++) {
                gfx_color_t* cell = fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
                mark_rect_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, gfx_rgb(0x00FFFF));
            }
        }
        for (int i = 0; i < leaf_n; i++) {
            const dirty_leaf_rect_t* r = &leaf_rect_scratch[i];
            gfx_color_t* at = fb + (size_t)r->y0 * GFX_WIDTH + r->x0;
            mark_rect_border(at, GFX_WIDTH, r->x1 - r->x0, r->y1 - r->y0, gfx_rgb(0x00FF00));
        }

        if (send_fb_rows(y, y + STRIP_HEIGHT)) {
            xSemaphoreTake(strip_sent, portMAX_DELAY);
        }

        /* Restore in the reverse order of saving. */
        for (int i = leaf_n - 1; i >= 0; i--) {
            const dirty_leaf_rect_t* r = &leaf_rect_scratch[i];
            gfx_color_t* at = fb + (size_t)r->y0 * GFX_WIDTH + r->x0;
            restore_border(at, GFX_WIDTH, r->x1 - r->x0, r->y1 - r->y0, overlay_leaf_save()[i]);
        }
        if (debug_overlay_on) {
            for (int col = GRID_COLS - 1; col >= 0; col--) {
                gfx_color_t* cell = fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
                restore_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, overlay_cell_save()[col]);
            }
        }
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
 * machinery assumes send_full_row()'s full STRIP_HEIGHT box. */
static bool
send_partial_band(int y0, int y1, int* queued) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (overlay_any_on()) {
        return false;
    }
#endif
    y0 = even_floor(y0);
    y1 = even_ceil(y1);
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
                 * run - safe to return. */
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

/* GFX_PIXFMT_INDEXED8's own send loop - whole dirty STRIP_HEIGHT strips,
 * full width, rather than send_one_row()'s per-run gathering: the index
 * image is small enough that expanding a strip nothing changed in costs
 * little, and every dirty strip still goes through gfx_dirty.h's own
 * tracker unmodified. No interlace or partial-clear here - both are
 * independent app opt-ins the RGB565 path alone offers. */
static void
run_present_indexed(void) {
    int queued = 0;
    for (int row = 0; row < STRIP_COUNT; row++) {
        if (!dirty_row_is_dirty(row)) {
            continue;
        }
        if (send_indexed_rows(row * STRIP_HEIGHT, (row + 1) * STRIP_HEIGHT)) {
            queued++;
        }
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

/* The real send, run on the present task (async) or on the caller
 * (gfx_set_present_async(false)) - either way, on whichever core called it,
 * since strip_sent is an ordinary FreeRTOS semaphore and the panel's own
 * strip-sent interrupt is core-agnostic about who it wakes. */
static void
run_present_normal(void) {
    panel_clock_apply();
    present_send_failed = false;
    if (current_mode.pixfmt == GFX_PIXFMT_INDEXED8) {
        run_present_indexed();
        return;
    }

    int queued = 0;
    if (interlace_on) {
        frame_parity = !frame_parity;
    }

    uint32_t remaining_cell_dirty = 0;

    for (int row = 0; row < STRIP_COUNT; row++) {
        if (!dirty_row_is_dirty(row)) {
            continue; /* unchanged - the panel is still showing it */
        }

        if (interlace_on && (row % 2) != frame_parity) {
            remaining_cell_dirty |= cell_dirty & (((1u << GRID_COLS) - 1u) << (row * GRID_COLS));
            continue;
        }

        send_one_row(row, &queued);
        dirty_row_sent(row);
    }

    dirty_frame_sent();
    if (interlace_on) {
        cell_dirty = remaining_cell_dirty;
    }

    if (partial_clear_on && drawn_bbox_valid) {
        prev_bbox_x0 = drawn_bbox_x0;
        prev_bbox_y0 = drawn_bbox_y0;
        prev_bbox_x1 = drawn_bbox_x1;
        prev_bbox_y1 = drawn_bbox_y1;
        prev_bbox_valid = true;
    } else if (!partial_clear_on) {
        prev_bbox_valid = false;
    }
    drawn_bbox_valid = false;

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

/* Dispatches to the present task when async, runs directly otherwise - see
 * gfx_set_present_async(). Shared by gfx_present_begin() and the raw-full
 * test helper below, which only differ in present_task_mode. */
static void
dispatch_present(void) {
    if (present_async_on) {
        xTaskNotifyGive(present_task_handle);
        return;
    }
    /* Synchronous: the send happens now, on the caller's own core, before
     * gfx_present_begin() returns - gfx_present_wait() then has nothing
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
    gfx_present_guard_begin();
    if (band_is_app_driven()) {
        return; /* the band ring sends and waits inside frame() itself */
    }
    present_task_mode = PRESENT_TASK_NORMAL;
    dispatch_present();
}

void
gfx_present_wait(void) {
    if (!band_is_app_driven() && present_async_on) {
        xSemaphoreTake(present_done_sem, portMAX_DELAY);
    }
    gfx_present_guard_end();
}

#else /* !ESP_PLATFORM */

/* collect_dirty_runs()/plan_run()/run_box() (gfx_dirty.h) back send_one_row()
 * and friends below, which exist only on the device - a host build's own
 * copy of the header (this file includes it directly, same as any suite
 * that does) would otherwise trip -Wunused-function. See
 * suite_gfx_present_guard.c's touch_unused_dirty_symbols() for the same
 * fix applied to a smaller subset of this header. */
static void __attribute__((unused))
touch_unused_dirty_run_symbols(void) {
    (void)collect_dirty_runs;
    (void)plan_run;
    (void)run_box;
}

void
gfx_present_begin(void) {
    gfx_present_guard_begin();
}

void
gfx_present_wait(void) {
    /* No panel on a host build; draining the dirty tracker here is what
     * lets a host test assert the same "sequencing leaves it clean"
     * property a real present provides - see suite_gfx_present_guard.c.
     * The app-driven RGB565 band ring has no dirty tracker to drain - it
     * already settled inside frame(). */
    if (!band_is_app_driven()) {
        dirty_frame_sent();
    }
    gfx_present_guard_end();
}

#endif /* ESP_PLATFORM - the presentation pipeline */

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

/* Mode and the band ring */

#ifdef ESP_PLATFORM
static bool
alloc_full_framebuffer(void) {
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = heap_caps_malloc(bytes, BOARD_FRAMEBUFFER_CAPS);
    if (fb == NULL) {
        ESP_LOGE(TAG, "Could not reallocate the %u byte framebuffer leaving band mode", (unsigned)bytes);
        return false;
    }
    return true;
}

static void
free_full_framebuffer(void) {
    heap_caps_free(fb);
    fb = NULL;
}

static bool
alloc_band_buffers(int band_height) {
    const size_t bytes = (size_t)GFX_WIDTH * (size_t)band_height * sizeof(gfx_color_t);
    for (int i = 0; i < GFX_BAND_SLOTS; i++) {
        band_buf[i] = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (band_buf[i] == NULL) {
            ESP_LOGE(TAG, "Could not allocate %u byte band buffer", (unsigned)bytes);
            return false;
        }
    }
    return true;
}

static bool
alloc_indexed_image(int grid_w, int grid_h) {
    const size_t bytes = (size_t)grid_w * (size_t)grid_h;
    indexed_image = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (indexed_image == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte index image", (unsigned)bytes);
        return false;
    }
    return true;
}

static void
free_indexed_image(void) {
    heap_caps_free(indexed_image);
    indexed_image = NULL;
}
#else
static bool
alloc_full_framebuffer(void) {
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = malloc(bytes);
    return fb != NULL;
}

static void
free_full_framebuffer(void) {
    free(fb);
    fb = NULL;
}

static bool
alloc_band_buffers(int band_height) {
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
#ifdef ESP_PLATFORM
    band_snapshot = heap_caps_malloc(bytes, BOARD_FRAMEBUFFER_CAPS);
#else
    band_snapshot = malloc(bytes);
#endif
    band_snapshot_bands = 0;
    band_snapshot_filling = false;
    band_snapshot_complete = false;
    return band_snapshot != NULL;
}

static void
free_band_snapshot(void) {
#ifdef ESP_PLATFORM
    heap_caps_free(band_snapshot);
#else
    free(band_snapshot);
#endif
    band_snapshot = NULL;
    band_snapshot_filling = false;
    band_snapshot_complete = false;
}

static void
free_band_buffers(void) {
    for (int i = 0; i < GFX_BAND_SLOTS; i++) {
#ifdef ESP_PLATFORM
        heap_caps_free(band_buf[i]);
#else
        free(band_buf[i]);
#endif
        band_buf[i] = NULL;
    }
}

static void
reset_mode_to_full_fb(void) {
    current_mode.layout = GFX_LAYOUT_FULL_FB;
    current_mode.resolution = GFX_RESOLUTION_FULL;
    current_mode.interlace_x = false;
    current_mode.interlace_y = false;
    current_mode.width = GFX_WIDTH;
    current_mode.height = GFX_HEIGHT;
    current_mode.band_height = 0;
    current_mode.pixfmt = GFX_PIXFMT_RGB565;
    current_mode.index_grid_w = 0;
    current_mode.index_grid_h = 0;
    current_mode.cell_size = 0;
}

/* Only GFX_RESOLUTION_FULL is wired to real rendering, so the system-wide
 * resolution cap a future Settings app would own (roadmap section 8,
 * decision 1) is not a variable yet - hardcoding it here is the one place
 * that changes once it is. */
const gfx_mode_t*
gfx_mode_enter(const gfx_mode_request_t* request) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_FULL_FB);

    const gfx_mode_t granted = gfx_mode_resolve(request, GFX_RESOLUTION_FULL, GFX_WIDTH, GFX_HEIGHT, GFX_BAND_HEIGHT);

    if (granted.layout == GFX_LAYOUT_BANDS && granted.pixfmt == GFX_PIXFMT_INDEXED8) {
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
    if (current_mode.layout == GFX_LAYOUT_BANDS) {
        if (current_mode.pixfmt == GFX_PIXFMT_INDEXED8) {
            free_indexed_image();
        } else {
            free_band_buffers();
        }
        if (alloc_full_framebuffer()) {
            gfx_fb_guard_set_available(true);
        }
#ifdef ESP_PLATFORM
        else {
            /* Nothing downstream can draw without a framebuffer - the same
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
}

const gfx_mode_t*
gfx_mode_current(void) {
    return &current_mode;
}

void
gfx_band_frame_begin(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_BANDS);
#ifdef ESP_PLATFORM
    panel_clock_apply();
#endif
    band_render_active = false;
    gfx_fb_guard_set_available(false);
    gfx_band_ring_begin(&band_ring, current_mode.height / current_mode.band_height);

    /* Captured once per frame, not read live from gfx_band_dirty(): a
     * gfx_invalidate() call mid-frame (an app's own BOOT-menu toggle, say)
     * must not retroactively force bands this frame already skipped. */
    band_frame_force_all = gfx_band_take_force_all();

    if (band_snapshot != NULL && !band_snapshot_complete) {
        band_snapshot_bands = 0;
        band_snapshot_filling = band_frame_force_all;
    }
}

/* Band mode's own "does [row0, row1) need touching this frame" query -
 * reuses gfx_dirty.h's cell tracker (dirty_band_extent()), the same one
 * gfx_present() consults for full-fb sends, fed by the ordinary
 * gfx_mark_dirty() calls an app and ui.c already make. A forced frame
 * (gfx_invalidate(), gfx_mode_enter()) always reports the full width
 * dirty, without needing every cell actually marked. */
bool
gfx_band_dirty(int row0, int row1, int* out_x0, int* out_x1) {
    if (band_frame_force_all) {
        *out_x0 = 0;
        *out_x1 = GFX_WIDTH;
        return true;
    }
    return dirty_band_extent(row0, row1, out_x0, out_x1);
}

/* The band gfx_band_next() just handed out needs no redraw this frame
 * (gfx_band_dirty() said so) - advances past it without rendering or
 * sending anything, leaving whatever the panel already shows there. */
void
gfx_band_skip(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_BANDS);
    band_render_active = false;
    gfx_fb_guard_set_available(false);
    gfx_band_ring_skip(&band_ring);
}

bool
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
        dirty_frame_sent();
        return false;
    }
    band_current_slot = gfx_band_ring_slot(&band_ring);
    band_render_row0 = gfx_band_ring_row0(&band_ring, current_mode.band_height);
    band_render_height = current_mode.band_height;
    band_render_active = true;
    gfx_fb_guard_set_available(true);
    return true;
}

gfx_color_t*
gfx_band_buffer(void) {
    GFX_PRESENT_GUARD();
    return band_buf[band_current_slot];
}

int
gfx_band_row0(void) {
    GFX_PRESENT_GUARD();
    return band_render_row0;
}

int
gfx_band_height(void) {
    GFX_PRESENT_GUARD();
    return band_render_height;
}

int
gfx_band_count(void) {
    GFX_PRESENT_GUARD();
    return band_ring.band_count;
}

void
gfx_band_submit(void) {
    GFX_PRESENT_GUARD();
    assert(current_mode.layout == GFX_LAYOUT_BANDS);
#if CONFIG_LAUNCHER_DEVELOPMENT
    /* The same cyan the full-fb overlay borders a sent cell with
     * (gfx_set_debug_overlay(), send_full_row()) - drawn through the band
     * draw target, into the buffer about to be sent, so a band
     * gfx_band_skip() left alone carries no border at all. No per-leaf
     * breakdown yet (send_full_row()'s green leaves): this is the
     * band-level "was it sent" signal only. */
    if (overlay_any_on()) {
        const gfx_color_t cyan = gfx_rgb(0x00FFFF);
        const int row0 = band_render_row0;
        const int row1 = row0 + band_render_height;
        gfx_line(0, row0, GFX_WIDTH - 1, row0, cyan);
        gfx_line(0, row1 - 1, GFX_WIDTH - 1, row1 - 1, cyan);
        gfx_line(0, row0, 0, row1 - 1, cyan);
        gfx_line(GFX_WIDTH - 1, row0, GFX_WIDTH - 1, row1 - 1, cyan);
    }
#endif
    if (band_snapshot_filling) {
        memcpy(band_snapshot + (size_t)band_render_row0 * GFX_WIDTH, band_buf[band_current_slot],
               (size_t)band_render_height * GFX_WIDTH * sizeof(gfx_color_t));
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
    /* Nothing queued, so nothing will ever mark this band's strip_sent -
     * settle the ring in place and force every band next frame instead of
     * leaving gfx_band_next() waiting on a give that is never coming. */
    gfx_band_force_all();
    gfx_band_ring_settle(&band_ring);
    gfx_band_ring_skip(&band_ring);
}

uint8_t*
gfx_indexed_image(void) {
    GFX_PRESENT_GUARD();
    return indexed_image;
}

gfx_readback_t
gfx_readback_begin(void) {
    GFX_PRESENT_GUARD();
    if (!band_is_app_driven()) {
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
    if (current_mode.layout == GFX_LAYOUT_FULL_FB) {
        memcpy(out_row, fb + (size_t)y * GFX_WIDTH, GFX_WIDTH * sizeof(gfx_color_t));
    } else if (current_mode.pixfmt == GFX_PIXFMT_INDEXED8) {
        const gfx_indexed_frame_t frame = indexed_frame();
        gfx_indexed_expand_panel_row(&frame, y, out_row, GFX_WIDTH);
    } else {
        assert(band_snapshot_complete);
        memcpy(out_row, band_snapshot + (size_t)y * GFX_WIDTH, GFX_WIDTH * sizeof(gfx_color_t));
    }
}

void
gfx_readback_end(void) {
    GFX_PRESENT_GUARD();
    free_band_snapshot();
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

/* Installs `table` for `mode` and selects it as the active one -
 * GFX_PIXFMT_INDEXED8's own dither pattern while indexed_dither16_on is
 * true (gfx_indexed_set_dither16()); meaningless in 256 mode, which never
 * consults it. `table` must be sized for `mode` - see gfx_dither_mode_t's
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

unsigned
gfx_present_guard_trip_count(void) {
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    return gfx_present_guard_trips;
#else
    return 0;
#endif
}

bool
gfx_present_in_flight(void) {
    return gfx_present_guard_in_flight;
}

unsigned
gfx_fb_guard_trip_count(void) {
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    return gfx_fb_guard_trips;
#else
    return 0;
#endif
}

#if CONFIG_LAUNCHER_DEVELOPMENT
#ifdef ESP_PLATFORM
/* Every send runs on the present task (or the caller, under
 * gfx_set_present_async(false)) - never call this while a present is
 * already in flight; it is a test helper, not part of the app-facing
 * pipeline, so it has no begin/wait split of its own. */
void
gfx_present_raw_full_frame_for_test(void) {
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
