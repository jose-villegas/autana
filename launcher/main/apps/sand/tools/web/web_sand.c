/* web_sand: the browser build's driver, running the real sand simulation and painter compiled to WebAssembly.
 *
 * It does app_sand.c's frame for a web page: the same simulation calls,
 * fixed-step accumulators, brush list, paint clocks and row painter, with
 * no panel, microui or IMU behind it. The grid is always the panel's own
 * portrait shape at one of the device's cell sizes, so the painter's
 * device-sized buffers hold it. Landscape is the panel turned a quarter
 * turn, the way a player holds the board: only gravity, touch and the
 * final copy out are rotated.
 *
 * Every frame repaints the whole grid. A canvas upload costs nothing like
 * the SPI bus does, so the dirty-row bookkeeping the device needs has no
 * job here. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "apps/sand/material.h"
#include "apps/sand/material_palette.h"
#include "apps/sand/sand.h"
#include "apps/sand/sand_brushes.h"
#include "apps/sand/sand_dither_tables.h"
#include "apps/sand/sand_limits.h"
#include "apps/sand/sand_paint_clock.h"
#include "apps/sand/sand_paint_row.h"
#include "apps/sand/sand_palette256.h"
#include "gfx/gfx.h"
#include "gfx/gfx_indexed.h"
#include "input/tilt.h"

/* The scale JS sends gravity in: one g, matching the IMU's own counts. */
#define WEB_COUNTS_PER_G        4096

/* app_sand.c's brush radii and step rates; a brush keeps its on-screen size at every cell size. */
#define POUR_RADIUS_PX          10
#define ERASE_RADIUS_PX         16
#define ERASE_EMITTER_RADIUS_PX 32
#define DETONATE_RADIUS_PX      50
#define SHAKE_DEADZONE          40
#define SIM_HZ                  60
#define SIM_STEP_MS             (1000 / SIM_HZ)
#define SIM_MAX_CATCHUP         2
#define POUR_HZ                 60
#define POUR_STEP_MS            (1000 / POUR_HZ)

/* sand_ui.h's PAINT/ERASE/DETONATE as plain ints, the form that crosses the wasm boundary. */
#define WEB_MODE_PAINT          0
#define WEB_MODE_ERASE          1
#define WEB_MODE_DETONATE       2
#define WEB_MODE_COUNT          3
#define WEB_RADIUS_MIN          2
#define WEB_RADIUS_MAX          64

typedef enum {
    WEB_COLOR_FULL,
    WEB_COLOR_256,
    WEB_COLOR_16,
    WEB_COLOR_MODE_COUNT,
} web_color_mode_t;

static uint8_t grid[GRID_W_MAX * GRID_H_MAX];
static impulse_t impulses[GRID_W_MAX * GRID_H_MAX];
static uint8_t index_image[GRID_W_MAX * GRID_H_MAX];
static gfx_color_t panel[GFX_WIDTH * GFX_HEIGHT];
static uint8_t pixels[GFX_WIDTH * GFX_HEIGHT * 4];

static sand_t sim;
static tilt_t tilt;
static sand_paint_row_state_t paint_state;
static sand_paint_frame_t paint_frame = SAND_PAINT_FRAME_INIT;
static sand_paint_clock_t paint_clock = SAND_PAINT_CLOCK_INIT;
static int wood_leaf_top5_down;

static int grid_w, grid_h, cell_px;
static bool landscape;
static bool ready;
static int brush_index;
static web_color_mode_t color_mode = WEB_COLOR_FULL;
static gfx_dither_mode_t dither_mode = GFX_DITHER_CELL_BAYER2;
static int gravity_x, gravity_y = WEB_COUNTS_PER_G;

/* One radius per mode, kept across a quality or orientation change: those rebuild the grid, not the brush. */
static int radius_px[WEB_MODE_COUNT] = {
    [WEB_MODE_PAINT] = POUR_RADIUS_PX,
    [WEB_MODE_ERASE] = ERASE_RADIUS_PX,
    [WEB_MODE_DETONATE] = DETONATE_RADIUS_PX,
};

static uint32_t sim_accumulator_q8;
static uint32_t pour_accumulator_ms;
static int sim_speed_q8 = 256;

static int
screen_w(void) {
    return landscape ? GFX_HEIGHT : GFX_WIDTH;
}

static int
screen_h(void) {
    return landscape ? GFX_WIDTH : GFX_HEIGHT;
}

/* `cell_px_in` below the panel's finest cell size takes that size, so the grid always fits the painter. */
EMSCRIPTEN_KEEPALIVE
int
web_init(int cell_px_in, int landscape_in) {
    cell_px = cell_px_in < CELL_MIN ? CELL_MIN : cell_px_in;
    landscape = landscape_in != 0;
    grid_w = GFX_WIDTH / cell_px;
    grid_h = GFX_HEIGHT / cell_px;

    memset(panel, 0, sizeof panel);
    memset(index_image, 0, sizeof index_image);
    sim_accumulator_q8 = 0;
    pour_accumulator_ms = 0;

    sand_init(&sim, grid, grid_w, grid_h, (uint32_t)rand());
    sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
    sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&sim, impulses, grid_w * grid_h);
    sand_paint_row_state_init(&paint_state);
    tilt_reset(&tilt, WEB_COUNTS_PER_G);

    ready = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int
web_screen_w(void) {
    return screen_w();
}

EMSCRIPTEN_KEEPALIVE
int
web_screen_h(void) {
    return screen_h();
}

EMSCRIPTEN_KEEPALIVE
int
web_brush_count(void) {
    return SAND_BRUSH_COUNT;
}

EMSCRIPTEN_KEEPALIVE
void
web_set_brush(int index) {
    if (index >= 0 && index < SAND_BRUSH_COUNT) {
        brush_index = index;
    }
}

/* Clamped and stored for `mode` only, like sand_ui_set_radius(); returns what was kept. */
EMSCRIPTEN_KEEPALIVE
int
web_set_radius(int mode, int px) {
    if (mode < 0 || mode >= WEB_MODE_COUNT) {
        return 0;
    }
    radius_px[mode] = px < WEB_RADIUS_MIN ? WEB_RADIUS_MIN : (px > WEB_RADIUS_MAX ? WEB_RADIUS_MAX : px);
    return radius_px[mode];
}

EMSCRIPTEN_KEEPALIVE
int
web_radius(int mode) {
    return (mode >= 0 && mode < WEB_MODE_COUNT) ? radius_px[mode] : 0;
}

/* Q8, 256 = real time. Only the physics step rate scales: the colour clocks are not the simulation. */
EMSCRIPTEN_KEEPALIVE
void
web_set_sim_speed(int speed_q8) {
    sim_speed_q8 = speed_q8 > 0 ? speed_q8 : 256;
}

EMSCRIPTEN_KEEPALIVE
void
web_set_color_mode(int mode) {
    if (mode >= 0 && mode < WEB_COLOR_MODE_COUNT) {
        color_mode = (web_color_mode_t)mode;
    }
}

EMSCRIPTEN_KEEPALIVE
void
web_set_dither_mode(int mode) {
    if (mode >= 0 && mode < GFX_DITHER_MODE_COUNT) {
        dither_mode = (gfx_dither_mode_t)mode;
    }
}

EMSCRIPTEN_KEEPALIVE
const char*
web_brush_name(int index) {
    return (index >= 0 && index < SAND_BRUSH_COUNT) ? material_name(sand_brushes[index].cell) : "";
}

/* Brush `index`'s palette-tile colour as RGB888, for the page's own swatch buttons. */
EMSCRIPTEN_KEEPALIVE
uint32_t
web_brush_swatch(int index) {
    if (index < 0 || index >= SAND_BRUSH_COUNT) {
        return 0;
    }
    return gfx_color_rgb888(material_brush_color(sand_brushes[index].cell));
}

/* `ax`/`ay`/`az` is screen-axis gravity in WEB_COUNTS_PER_G; a page with
 * no sensor sends one g straight down. In landscape the screen's +x is the
 * panel's +y and the screen's +y is the panel's -x. */
EMSCRIPTEN_KEEPALIVE
void
web_step(uint32_t dt_ms, int ax, int ay, int az) {
    if (!ready) {
        return;
    }
    tilt_update(&tilt, landscape ? -ay : ax, landscape ? ax : ay, az, 0, dt_ms);
    const int gx = tilt_x(&tilt);
    const int gy = tilt_y(&tilt);
    gravity_x = gx;
    gravity_y = gy;
    const int shake = tilt_shake(&tilt);
    const int jostle = shake > SHAKE_DEADZONE ? shake : 0;

    material_set_gravity(gx, gy);
    material_shine_direction(gx, gy, &paint_frame.shine_ux_q8, &paint_frame.shine_uy_q8);
    material_wood_leaf_wind_axis(gx, gy, &paint_frame.wood_leaf_wind_ux_q8, &paint_frame.wood_leaf_wind_uy_q8);
    material_wood_leaf_top5(gx, gy, &wood_leaf_top5_down, paint_frame.wood_leaf_top5);
    sand_paint_clock_wind(&paint_clock, &paint_frame, dt_ms);
    sand_paint_clock_foam(&paint_clock, dt_ms);
    (void)sand_paint_clock_shine(&paint_clock, &paint_frame, dt_ms);
    (void)sand_paint_clock_cullet(&paint_clock, dt_ms);
    (void)sand_paint_clock_glass(&paint_clock, gx, gy);
    (void)sand_paint_clock_wood_leaf(&paint_clock, &paint_frame, dt_ms);

    if (tilt_in_free_fall(&tilt)) {
        return;
    }

    /* Flow and speed are both Q8. The catch-up cap scales with speed so a
     * deliberate 4x is not clipped back to two steps as if it were lag. */
    const uint32_t flow_q8 = ((uint32_t)tilt_strength(&tilt) * (uint32_t)sim_speed_q8) >> 8;
    sim_accumulator_q8 += dt_ms * flow_q8;
    int steps_cap = (int)(((uint32_t)SIM_MAX_CATCHUP * (uint32_t)sim_speed_q8) / 256);
    steps_cap = steps_cap < 1 ? 1 : steps_cap;
    int steps = (int)(sim_accumulator_q8 / (SIM_STEP_MS * 256));
    if (steps > steps_cap) {
        steps = steps_cap;
        sim_accumulator_q8 = 0;
    } else {
        sim_accumulator_q8 -= (uint32_t)steps * SIM_STEP_MS * 256;
    }
    for (int i = 0; i < steps; i++) {
        sand_step(&sim, gx, gy, jostle);
    }
}

/* Screen pixels to the panel cell under them. */
static void
cell_at(int x_px, int y_px, int* cx, int* cy) {
    const int px = landscape ? GFX_WIDTH - 1 - y_px : x_px;
    const int py = landscape ? x_px : y_px;
    *cx = px / cell_px;
    *cy = py / cell_px;
}

static int
radius_cells(int px) {
    return (px + cell_px / 2) / cell_px;
}

/* One call per frame, like app_sand.c's handle_pour_input(): `pressed` is
 * true only on the frame the pointer went down, and `source` places one
 * persistent emitter per press instead of pouring. */
EMSCRIPTEN_KEEPALIVE
void
web_input(int mode, int down, int pressed, int source, int x_px, int y_px, uint32_t dt_ms) {
    if (!ready) {
        return;
    }
    int cx, cy;
    cell_at(x_px, y_px, &cx, &cy);

    if (mode == WEB_MODE_DETONATE) {
        pour_accumulator_ms = 0;
        if (pressed) {
            sand_explode(&sim, cx, cy, radius_cells(radius_px[WEB_MODE_DETONATE]));
        }
        return;
    }
    if (!down) {
        pour_accumulator_ms = 0;
        return;
    }
    if (mode == WEB_MODE_PAINT && source) {
        if (pressed) {
            (void)sand_add_emitter(&sim, cx, cy, sand_brushes[brush_index].cell);
        }
        return;
    }

    pour_accumulator_ms += dt_ms;
    int applications = (int)(pour_accumulator_ms / POUR_STEP_MS);
    if (applications > SIM_MAX_CATCHUP) {
        applications = SIM_MAX_CATCHUP;
        pour_accumulator_ms = 0;
    } else {
        pour_accumulator_ms -= (uint32_t)applications * POUR_STEP_MS;
    }
    for (int i = 0; i < applications; i++) {
        if (mode == WEB_MODE_ERASE) {
            sand_erase(&sim, cx, cy, radius_cells(radius_px[WEB_MODE_ERASE]));
            sand_remove_emitters(&sim, cx, cy, radius_cells(ERASE_EMITTER_RADIUS_PX));
        } else {
            sand_spawn_cell_share(&sim, cx, cy, radius_cells(radius_px[WEB_MODE_PAINT]), sand_brushes[brush_index].cell,
                                  sand_brushes[brush_index].share_pct);
        }
    }
}

EMSCRIPTEN_KEEPALIVE
void
web_clear(void) {
    if (ready) {
        sand_clear(&sim);
    }
}

/* Where JS reads the frame out of wasm memory; static, so it never moves. */
EMSCRIPTEN_KEEPALIVE
uint8_t*
web_pixels_ptr(void) {
    return pixels;
}

/* The 256 and 16 modes go through the panel's own index-image expansion,
 * so what the page shows is what the board sends. */
static void
expand_index_image(void) {
    const gfx_indexed_frame_t frame = {
        .image = index_image,
        .grid_w = grid_w,
        .grid_h = grid_h,
        .cell_size = cell_px,
        .dither16_on = color_mode == WEB_COLOR_16,
        .dither_mode = dither_mode,
        .table = color_mode == WEB_COLOR_16 ? sand_dither_table_for(dither_mode) : sand_palette256_lut,
    };
    for (int y = 0; y < grid_h * cell_px; y++) {
        gfx_indexed_expand_panel_row(&frame, y, panel + (size_t)y * GFX_WIDTH, grid_w * cell_px);
    }
}

static void
copy_out(void) {
    const int sw = screen_w();
    const int sh = screen_h();
    for (int y = 0; y < sh; y++) {
        uint8_t* out = pixels + (size_t)y * sw * 4;
        for (int x = 0; x < sw; x++) {
            const int px = landscape ? GFX_WIDTH - 1 - y : x;
            const int py = landscape ? x : y;
            const uint32_t rgb = gfx_color_rgb888(panel[(size_t)py * GFX_WIDTH + px]);
            out[0] = (uint8_t)(rgb >> 16);
            out[1] = (uint8_t)(rgb >> 8);
            out[2] = (uint8_t)rgb;
            out[3] = 255;
            out += 4;
        }
    }
}

EMSCRIPTEN_KEEPALIVE
void
web_render(void) {
    if (!ready) {
        return;
    }
    const bool indexed = color_mode != WEB_COLOR_FULL;
    sand_paint_update_local_depth_gravity(&paint_state, gravity_x, gravity_y, grid_w, grid_h);
    for (int cy = 0; cy < grid_h; cy++) {
        sand_paint_row_n(&paint_state, &paint_frame, panel, indexed ? index_image + (size_t)cy * grid_w : NULL, cy,
                         grid + (size_t)cy * grid_w, cell_px, grid_w, grid_h, 0, grid_w, true);
    }
    if (indexed) {
        expand_index_image();
    }
    copy_out();
}
