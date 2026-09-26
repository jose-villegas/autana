/*
 * app_sand - falling sand, poured with a finger and steered by tilting.
 *
 * Three pieces, each of which knows nothing about the others:
 *   sand.c  the automaton   - pure, host-tested, no idea a screen exists
 *   imu.c   the QMI8658     - raw counts, no idea what they will be used for
 *   here    the wiring      - grid size, colours, axis mapping, rendering
 *
 * WHY THE GRID IS COARSER THAN THE SCREEN
 *
 * A cell per pixel would be 368 x 448 = 165 KB of grid and four times the
 * work of the finest quality every step, so a cell is a square block of
 * `cell` x `cell` pixels, and `cell` is chosen from
 * the options screen rather than fixed: ULTRA (2 px) gives a 184 x 224 grid, or
 * 41 KB; HIGH (3 px) gives 122 x 149, or 18 KB; NORMAL (4 px, the default)
 * gives 92 x 112, or 10 KB; LOW (6 px) gives 61 x 74, or about 4.5 KB;
 * VERY LOW (8 px) gives 46 x 56, or about 2.5 KB. All five still read as
 * grains rather than bricks - the choice trades fineness for the step budget
 * a finer grid costs, not for whether it looks right.
 *
 * Every allocation below is sized for the finest quality (2 px) regardless of
 * which one is active, so switching quality on the menu never reallocates
 * anything - it just changes how much of the same buffer is in use. That
 * matters for the same reason sand_exit() keeps the grid between visits: an
 * allocation that only ever happens once cannot fail because the heap
 * fragmented while something else was running.
 *
 * `cell` need not divide 368 or 448 evenly - grid_w/grid_h floor, so a
 * remainder just leaves an unredrawn margin at most cell-1 px wide along the
 * right and bottom edges, not an out-of-bounds write. At 2 px it divides
 * both evenly and there is no margin at all, but at 3 px it does not: 122 * 3
 * = 366 and 149 * 3 = 447, leaving a 2 px strip on the right and a 1 px strip
 * on the bottom that the grid never touches. At 6 px the margin is the
 * largest of any tier: 61 * 6 = 366 and 74 * 6 = 444, a 2 px strip on the
 * right and a 4 px strip on the bottom. That is harmless only because
 * the colour of an empty cell and the menu's background are the same value,
 * 0x0A0C14 - see COL_BACKGROUND - so the untouched strip is indistinguishable
 * from the screen around it. start_sim() still clears the screen explicitly
 * before the first frame rather than leaning on that coincidence alone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app.h"
#include "apps/sand/app_sand_test.h"
#include "build_variant.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "icons_dither.h"
#include "icons_sand.h"
#include "input/imu.h"
#include "input/imu_rotation.h"
#include "input/tilt.h"
#include "material_palette.h"
#include "palette.h"
#include "row_runs.h"
#include "sand.h"
#include "sand_colour_state.h"
#include "sand_heal.h"
#include "sand_limits.h"
#include "sand_menu.h"
#include "sand_mode_swatches.h"
#include "sand_paint.h"
#include "sand_paint_row.h"
#include "sand_palette256.h"
#include "sand_swatch.h"
#include "sand_ui.h"
#include "ui/brush_screen.h"
#include "ui/options_screen.h"
#include "ui/palette_screen.h"
#include "ui/title_screen.h"
#include "ui/ui.h"
#include "ui/ui_anchor.h"

static const char* TAG = "sand";

#define COL_BACKGROUND 0x0A0C14

typedef struct {
    const char* name;
    int cell;
} quality_t;

static const quality_t qualities[] = {
    {"ULTRA", 2}, {"HIGH", 3}, {"NORMAL", 4}, {"LOW", 6}, {"VERY LOW", 8},
};
#define QUALITY_COUNT   ((int)(sizeof(qualities) / sizeof(qualities[0])))
#define QUALITY_DEFAULT 2 /* NORMAL */

static int quality = QUALITY_DEFAULT;

/* FULL is today's RGB565 framebuffer path, byte-identical to before this
 * option existed. 256 and 16 both run GFX_PIXFMT_INDEXED8 (gfx_mode.h); 16 also
 * turns on its ordered dither against a shared 16-colour table. */
typedef enum {
    SAND_COLOR_FULL,
    SAND_COLOR_256,
    SAND_COLOR_16,
    SAND_COLOR_COUNT,
} sand_color_mode_t;

static const char* const color_names[SAND_COLOR_COUNT] = {"FULL", "256", "16"};

static sand_color_mode_t color_mode = SAND_COLOR_256;

/* The DITHER launch option, next to COLOUR once it is 16 -
 * gfx_dither_mode_t (gfx_indexed.h) directly, no sand-side mirror: "dither" has one
 * spelling either side of the app/engine line. CELL_BAYER2 by default, the
 * maintainer's own pick. */
static const char* const dither_names[GFX_DITHER_MODE_COUNT] = {
    "NONE", "CELL CHECKER", "CELL BAYER2", "PIXEL CHECKER2", "PIXEL BAYER4",
};

static gfx_dither_mode_t dither_mode = GFX_DITHER_CELL_BAYER2;

_Static_assert((int)ICON_DITHER_COUNT == (int)GFX_DITHER_MODE_COUNT && (int)ICON_DITHER_NONE == (int)GFX_DITHER_NONE
                   && (int)ICON_DITHER_CELL_CHECKER == (int)GFX_DITHER_CELL_CHECKER
                   && (int)ICON_DITHER_CELL_BAYER2 == (int)GFX_DITHER_CELL_BAYER2
                   && (int)ICON_DITHER_PIXEL_CHECKER2 == (int)GFX_DITHER_PIXEL_CHECKER2
                   && (int)ICON_DITHER_PIXEL_BAYER4 == (int)GFX_DITHER_PIXEL_BAYER4,
               "the options screen shows dither swatch i beside dither_names[i]");

/* sand_color_mode_t and sand_colour_state.h's own sand_colour_mode_t share
 * an ordinal order (FULL, 256, 16) by construction - one cast, not a
 * second enum's worth of call-site plumbing. */
_Static_assert((int)SAND_COLOR_FULL == (int)SAND_COLOUR_FULL && (int)SAND_COLOR_256 == (int)SAND_COLOUR_256
                   && (int)SAND_COLOR_16 == (int)SAND_COLOUR_16,
               "sand_color_mode_t must stay ordinal-compatible with sand_colour_mode_t");

/* Whether GFX_PIXFMT_INDEXED8 is actually active right now - see
 * sand_colour_state.h. The rest of the app asks sand_colour_indexed_active()
 * rather than color_mode directly: a mode request can be denied, and the
 * palette/brush screens suspend it without changing color_mode at all. */
static sand_colour_state_t colour_state;

/* Built once, at the first indexed entry, not per sand_enter() - the
 * generated tables never change at runtime. One per CLASS-kind dither mode
 * (NONE, both PIXEL ones); the CELL modes carry their own phase formula and
 * never classify at all - see gfx_indexed_cell_repaint()'s own comment. */
static uint8_t none_class[GFX_INDEXED_PALETTE_SIZE];
static uint8_t dither16_class[GFX_INDEXED_PALETTE_SIZE];
static uint8_t checker2_class[GFX_INDEXED_PALETTE_SIZE];
static bool dither_classes_ready;

/* Indexed repaint dispatch is selected when entering indexed mode. */
static sand_paint_row_state_t paint_row_state;
static sand_paint_frame_t paint_frame = {
    .shine_ux_q8 = 181,
    .shine_uy_q8 = 181,
    .wood_leaf_wind_ux_q8 = 256,
    .wood_leaf_wind_sign = 1,
    .wood_leaf_top5 = {{0, -1}, {-1, -1}, {1, -1}, {-1, 0}, {1, 0}},
    .repaint_kind = GFX_INDEXED_REPAINT_RAW,
};

/* Set whenever the PANEL, not just the simulation, needs every visited
 * cell resent regardless of whether its own index moved -
 * mark_sand_fully_dirty()'s call sites (an overlay just closed, the board
 * turned) can leave the index image already holding the value about to be
 * recomputed while the panel shows something else entirely (the overlay,
 * the wrong turn's pixels). Captured once per draw_dirty_rows() pass and
 * cleared there, the same idiom gfx.c's own band_force_all_dirty uses for
 * gfx_invalidate(). */
static bool indexed_force_full_repaint;

/* Once per start_sim(), not once per frame - see the emitter-marker/mode-
 * label skip in sand_frame(). */
static bool overlays_skipped_reason_logged;

/* The START button's own tap, deferred out of draw_menu()'s UI build:
 * start_sim() can free the framebuffer (SAND_GFX_ENTER_INDEXED), and
 * draw_menu()'s own ui_end() - still to come, same call - draws through it
 * once mu_button() returns. sand_frame() applies it at the top of its next
 * pass instead, a full UI build later. */
static bool pending_start;

/* The title and options screens' state. The launch options themselves stay
 * in quality/color_mode/dither_mode above; this only holds them while the
 * options screen edits a draft - see adopt_options(). */
static sand_menu_t menu;

/* Built from the palettes on the first visit to the options screen. */
static sand_mode_swatch_t mode_swatches[SAND_COLOUR_MODE_COUNT];
static bool mode_swatches_ready;

static int cell, grid_w, grid_h, block_cols, block_rows;

/* Two heal strips a present: enough to keep up with a pour's settling
 * bands without costing more than a seventh of a full frame. */
#define SAND_HEAL_BUDGET_PIXELS (GFX_WIDTH * 64)
static sand_heal_t heal_policy;

/* Default pour brush radius, in px - seeds sand_ui_t.radius_px; the value
 * actually in force is whatever the brush screen's slider last set (see
 * sand_ui_radius()). */
#define POUR_RADIUS_PX            10

#define POUR_HZ                   60
#define POUR_STEP_MS              (1000 / POUR_HZ)

/* Default erase brush radius, in px - see POUR_RADIUS_PX's own comment. */
#define ERASE_RADIUS_PX           16

#define ERASE_EMITTER_RADIUS_PX   32

/* Default BOOM brush radius, in px - see POUR_RADIUS_PX's own comment. */
#define DETONATE_RADIUS_PX        50

#define APP_IMPULSE_MAX           2048

#define SAND_IMPULSE_BUDGET_BYTES 12288

_Static_assert((unsigned long)APP_IMPULSE_MAX * sizeof(impulse_t) <= SAND_IMPULSE_BUDGET_BYTES,
               "APP_IMPULSE_MAX * sizeof(impulse_t) exceeds SAND_IMPULSE_BUDGET_BYTES - "
               "these are two independently-chosen constants that must agree. This "
               "assert passing is NOT proof detonate works on real hardware - this "
               "exact budget already failed a live device flash once at a larger "
               "value (24,576 bytes) that this same assert also happily passed, "
               "because the real failure was the budget being sized against total "
               "free heap instead of the largest contiguous block a single malloc() "
               "call actually needs - see SAND_IMPULSE_BUDGET_BYTES's own comment "
               "for that incident. Shrink APP_IMPULSE_MAX, or raise "
               "SAND_IMPULSE_BUDGET_BYTES only after a fresh device capture of "
               "heap_caps_get_largest_free_block() at the point impulse_buf is "
               "allocated - never from arithmetic alone.");

/* Selected from the palette panel, not cycled - a cycle's cost grows with
 * material count, a panel's doesn't. PAINT/ERASE/DETONATE is the brush
 * screen's segmented control, for the same reason: a HOLD's 600ms tax is
 * too slow for a control used this often. Only paintable materials get a
 * tile - burning wood is a STATE, not a material (reaction_t.burn_decay).
 * Whole CELLS, not ids: an extended material isn't nameable by id alone
 * (MATX() in material.h). */
static const sand_brush_t brushes[] = {
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_SAND, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_WATER, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_STONE, 0)), SAND_BRUSH_SOLID(CELL_MAKE(MAT_GAS, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_FIRE, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_WOOD, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_OIL, 0)),   SAND_BRUSH_SOLID(CELL_MAKE(MAT_LAVA, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_ACID, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_GLASS, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_SNOW, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_DIRT, 0)),
    SAND_BRUSH_SOLID(MATX(MATX_ICE)),          SAND_BRUSH_SPARSE(MATX(MATX_PLANT), SAND_BRUSH_SHARE_PLANT),
    SAND_BRUSH_SOLID(GUNPOWDER_CELL(0)), /* dry, tone 0 - see material_brush_color()'s own
                         * comment (material_palette.h) for why the panel tile itself paints a different code */
};
#define BRUSH_COUNT ((int)(sizeof(brushes) / sizeof(brushes[0])))

_Static_assert(PALETTE_FITS(BRUSH_COUNT), "the palette panel for BRUSH_COUNT brushes is taller than the "
                                          "screen at some orientation - see palette_cols()/PALETTE_TILE "
                                          "in palette.h");

/* brush_mode_t per brush - sand_init() does not reset this, so a brush can
 * still show Water as its source with no tap present. */
static uint8_t brush_mode[BRUSH_COUNT];

static sand_ui_t ui = {
    .brushes = brushes,
    .modes = brush_mode,
    .brush_count = BRUSH_COUNT,
    /* The three values PAINT/ERASE/DETONATE already used before each mode
     * had a slider of its own, so the brush screen opens on what the app
     * has always done rather than on a fresh set of numbers. */
    .radius_px = {[SAND_MODE_PAINT] = POUR_RADIUS_PX,
                  [SAND_MODE_ERASE] = ERASE_RADIUS_PX,
                  [SAND_MODE_DETONATE] = DETONATE_RADIUS_PX},
};

/* Duration mode label stays after significant change, balancing readability
 * and non-obtrusiveness. */
#define LABEL_MS        1800

#define LABEL_MARGIN    18
#define LABEL_SCALE     2

#define SHAKE_DEADZONE  40

#define SIM_HZ          60
#define SIM_STEP_MS     (1000 / SIM_HZ)

#define SIM_MAX_CATCHUP 2

static uint8_t* grid;
static uint8_t* dirty_rows;    /* GRID_H_MAX bytes: which rows changed -
                                   * only the first grid_h are in use at any
                                   * quality below ULTRA */
static uint8_t* sleep_blocks;  /* BLOCK_COLS_MAX*BLOCK_ROWS_MAX bytes:
                                   * settled blocks to skip - see
                                   * sand_enable_sleeping() */
static uint8_t* step_stamps;   /* sized for the largest grid - see
                                   * sand_enable_step_stamps() */
static void* lane_scratch;     /* sized for the largest grid - see
                                   * sand_enable_lane_scratch() */
static impulse_t* impulse_buf; /* APP_IMPULSE_MAX entries: grains in
                                   * flight from DETONATE - see
                                   * sand_enable_impulses(). */

static uint16_t* row_run_x0;
static uint16_t* row_run_x1;
static uint8_t* row_run_n;

/* GRID_H_MAX entries each: the sim's own changed-column span per row - see
 * sand_track_dirty_cols(). x0 > x1 (the sentinel sand_track_dirty_cols()
 * seeds) means no span was ever narrowed for that row this frame, so
 * draw_dirty_rows() repaints it full-width, exactly as before this existed. */
static uint16_t* dirty_x0;
static uint16_t* dirty_x1;
static sand_t sim;
static tilt_t tilt;
static bool failed;
static uint32_t label_left_ms; /* countdown for the mode label */

static bool input_ready;

/* The shell's quarter turn as of the last frame either overlay panel
 * (palette or brush) was drawn - both can be left open while the board
 * rotates, and each detects the change against this the same way. */
static int panel_drawn_quarter;

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Rolling averages, purely for the log line - a release build has nobody
 * watching the serial console to read them, so it carries none of this. */
static uint32_t frames;
static int64_t step_us_total;
static int64_t draw_us_total;
static int64_t rows_redrawn_total;
static int64_t pixels_repainted_total;
static int64_t steps_total;

static int64_t pour_step_us_total, pour_draw_us_total;
static uint32_t pour_frames;
static int64_t idle_step_us_total, idle_draw_us_total;
static uint32_t idle_frames;
static int64_t split_log_at_us;

static int64_t pour_awake_total, idle_awake_total;

/* Measure occupied cells in blocks; confirms step_one_row() cost per row, not
 * unit. */
static int64_t pour_awake_cells_total, idle_awake_cells_total;

#endif
static uint32_t sim_accumulator_q8;
static uint32_t pour_accumulator_ms;

/* Setup */

/* dither_mode's own generated table - sand_palette256.h ships one per
 * GFX_DITHER_* (report_shading_palette.sh). */
static const gfx_color_t*
sand_dither_table_for(gfx_dither_mode_t mode) {
    switch (mode) {
        case GFX_DITHER_NONE: return sand_dither_none_lut;
        case GFX_DITHER_CELL_CHECKER: return sand_dither_cell_checker;
        case GFX_DITHER_CELL_BAYER2: return sand_dither_cell_bayer2;
        case GFX_DITHER_PIXEL_CHECKER2: return sand_dither_pixel_checker2;
        case GFX_DITHER_PIXEL_BAYER4:
        default: return sand_palette16_dither_rgb;
    }
}

/* Actually issues the gfx_mode_enter() call SAND_GFX_ENTER_INDEXED asks
 * for, sized to the grid start_sim() computed. Rolls the state back via
 * sand_colour_grant_failed() if gfx could not grant it - a launch option
 * never blocks a player from playing at all, it just stays FULL. */
static void
apply_gfx_enter_indexed(void) {
    gfx_mode_request_t req = {0};
    req.layout = GFX_LAYOUT_BANDS;
    req.resolution = GFX_RESOLUTION_FULL;
    req.pixfmt = GFX_PIXFMT_INDEXED8;
    req.index_grid_w = grid_w;
    req.index_grid_h = grid_h;
    req.cell_size = cell;

    const gfx_mode_t* granted = gfx_mode_enter(&req);
    if (granted->layout != GFX_LAYOUT_BANDS) {
        ESP_LOGW(TAG, "COLOUR %s unavailable this session - staying FULL", color_names[color_mode]);
        sand_colour_grant_failed(&colour_state);
        return;
    }

    memset(gfx_indexed_image(), 0, (size_t)grid_w * (size_t)grid_h);
    gfx_indexed_set_lut(sand_palette256.entries);
    gfx_indexed_set_dither16(color_mode == SAND_COLOR_16);
    if (color_mode == SAND_COLOR_16) {
        gfx_indexed_set_dither(dither_mode, sand_dither_table_for(dither_mode));
    }
    if (!dither_classes_ready) {
        gfx_indexed_classify(sand_dither_none_lut, 1, none_class);
        gfx_indexed_dither16_classify(sand_palette16_dither_rgb, dither16_class);
        gfx_indexed_classify(sand_dither_pixel_checker2,
                             GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX, checker2_class);
        dither_classes_ready = true;
    }
    /* Color and dither modes stay fixed during a run, so indexed repaint
     * dispatch is resolved when entering indexed mode. */
    if (color_mode != SAND_COLOR_16) {
        paint_frame.repaint_kind = GFX_INDEXED_REPAINT_RAW;
        paint_frame.repaint_class_table = NULL;
        paint_frame.repaint_cell_table = NULL;
    } else {
        paint_frame.repaint_class_table = NULL;
        paint_frame.repaint_cell_table = NULL;
        switch (dither_mode) {
            case GFX_DITHER_NONE:
                paint_frame.repaint_kind = GFX_INDEXED_REPAINT_CLASS;
                paint_frame.repaint_class_table = none_class;
                break;
            case GFX_DITHER_CELL_CHECKER:
                paint_frame.repaint_kind = GFX_INDEXED_REPAINT_CELL_CHECKER;
                paint_frame.repaint_cell_table = sand_dither_cell_checker;
                break;
            case GFX_DITHER_CELL_BAYER2:
                paint_frame.repaint_kind = GFX_INDEXED_REPAINT_CELL_BAYER2;
                paint_frame.repaint_cell_table = sand_dither_cell_bayer2;
                break;
            case GFX_DITHER_PIXEL_CHECKER2:
                paint_frame.repaint_kind = GFX_INDEXED_REPAINT_CLASS;
                paint_frame.repaint_class_table = checker2_class;
                break;
            case GFX_DITHER_PIXEL_BAYER4:
            default:
                paint_frame.repaint_kind = GFX_INDEXED_REPAINT_CLASS;
                paint_frame.repaint_class_table = dither16_class;
                break;
        }
    }
    /* Belt and suspenders past the memset above: entering indexed mode
     * always owes a full repaint, whatever the index image happens to
     * hold - see indexed_force_full_repaint's own comment. */
    indexed_force_full_repaint = true;
}

/* Runs whatever gfx_mode_enter()/exit() call `action` names - the one place
 * that turns a sand_colour_state.h verdict into a real gfx call. */
static void
apply_gfx_action(sand_gfx_action_t action) {
    switch (action) {
        case SAND_GFX_ENTER_INDEXED: apply_gfx_enter_indexed(); break;
        case SAND_GFX_EXIT_TO_FULL: gfx_mode_exit(); break;
        case SAND_GFX_NONE: break;
    }
}

static sand_options_t
current_options(void) {
    return (sand_options_t){
        .quality = quality,
        .color = (sand_colour_mode_t)color_mode,
        .dither = (int)dither_mode,
    };
}

static void
sand_enter(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    frames = 0;
    step_us_total = 0;
    draw_us_total = 0;
    rows_redrawn_total = 0;
    pixels_repainted_total = 0;
    steps_total = 0;
    pour_step_us_total = 0;
    pour_draw_us_total = 0;
    pour_frames = 0;
    idle_step_us_total = 0;
    idle_draw_us_total = 0;
    idle_frames = 0;
    pour_awake_total = 0;
    idle_awake_total = 0;
    pour_awake_cells_total = 0;
    idle_awake_cells_total = 0;
    split_log_at_us = esp_timer_get_time() + 2000000;
#endif
    /* Every path back to the menu leaves indexed mode first - the menu has
     * no indexed draw path and would touch a framebuffer that does not
     * exist. Idempotent: a plain FULL entry asks for nothing. */
    apply_gfx_action(sand_colour_on_enter_menu(&colour_state));
    ui.screen = SAND_UI_MENU;
    sand_menu_init(&menu);

    /* A tap that outlived its own app session (START, then home before the
     * deferred frame ran) must not restart the sim before the menu it
     * belonged to ever draws again - see pending_start's own comment. */
    pending_start = false;

    ui_invalidate();
}

/* Seeds every row's run-tracking as one full-width span, as if the whole row
 * were occupied. Shared with close_overlay_screen()'s full redraw (through
 * mark_sand_fully_dirty()), which forces the panel to clear the framebuffer fully on the first frame
 * after closing, not trusting the sand's narrower real extent. */
static void
seed_row_runs_full_width(void) {
    for (int i = 0; i < grid_h; i++) {
        row_run_x0[i * ROW_MAX_RUNS] = 0;
        row_run_x1[i * ROW_MAX_RUNS] = (uint16_t)grid_w;
        row_run_n[i] = 1;
    }
}

/* An explicit [0, grid_w) span on every row, NOT the (grid_w, 0) sentinel:
 * the sentinel is the empty span a union starts from, so a sim mark landing
 * before the next draw would shrink a sentinel row to just the moved cells
 * and leave an overlay's pixels (palette panel, mode label) unrepainted. */
static void
reset_dirty_cols_full_width(void) {
    for (int i = 0; i < grid_h; i++) {
        dirty_x0[i] = 0;
        dirty_x1[i] = (uint16_t)grid_w;
    }
}

static void
mark_sand_fully_dirty(void) {
    seed_row_runs_full_width();
    memset(dirty_rows, 1, (size_t)grid_h);
    reset_dirty_cols_full_width();
    gfx_mark_all_dirty();
    indexed_force_full_repaint = true;
}

#if CONFIG_LAUNCHER_SELFTEST
bool
sand_app_alloc_selfcheck(size_t* out_largest_free, bool* out_impulses_ok) {
    uint8_t* t_dirty = malloc(GRID_H_MAX);
    uint8_t* t_blocks = malloc((size_t)BLOCK_COLS_MAX * BLOCK_ROWS_MAX);
    uint8_t* t_grid = malloc((size_t)GRID_W_MAX * GRID_H_MAX);
    uint16_t* t_x0 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* t_x1 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* t_n = malloc(GRID_H_MAX * sizeof(uint8_t));
    uint16_t* t_dcx0 = malloc(GRID_H_MAX * sizeof(uint16_t));
    uint16_t* t_dcx1 = malloc(GRID_H_MAX * sizeof(uint16_t));
    impulse_t* t_imp = malloc((size_t)APP_IMPULSE_MAX * sizeof(impulse_t));
    uint8_t* t_stamps = malloc(sand_step_stamp_bytes(GRID_W_MAX, GRID_H_MAX));
    void* t_lanes = malloc(sand_lane_scratch_bytes(GRID_W_MAX, GRID_H_MAX));

    const bool essential_ok =
        (t_dirty && t_blocks && t_stamps && t_lanes && t_grid && t_x0 && t_x1 && t_n && t_dcx0 && t_dcx1);
    if (out_impulses_ok) {
        *out_impulses_ok = (t_imp != NULL);
    }
    if (out_largest_free) {
        *out_largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }

    free(t_lanes);
    free(t_stamps);
    free(t_imp);
    free(t_dcx1);
    free(t_dcx0);
    free(t_n);
    free(t_x1);
    free(t_x0);
    free(t_grid);
    free(t_blocks);
    free(t_dirty);
    return essential_ok;
}

#endif /* CONFIG_LAUNCHER_SELFTEST */

/* Allocated after the grid and the blast buffer - see the ordering note in
 * start_sim(). */
static bool
alloc_grid_bookkeeping(void) {
    if (step_stamps == NULL) {
        step_stamps = malloc(sand_step_stamp_bytes(GRID_W_MAX, GRID_H_MAX));
    }
    if (lane_scratch == NULL) {
        lane_scratch = malloc(sand_lane_scratch_bytes(GRID_W_MAX, GRID_H_MAX));
    }
    if (row_run_x0 == NULL) {
        row_run_x0 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(*row_run_x0));
    }
    if (row_run_x1 == NULL) {
        row_run_x1 = malloc(GRID_H_MAX * ROW_MAX_RUNS * sizeof(*row_run_x1));
    }
    if (row_run_n == NULL) {
        row_run_n = malloc(GRID_H_MAX * sizeof(*row_run_n));
    }
    if (dirty_x0 == NULL) {
        dirty_x0 = malloc(GRID_H_MAX * sizeof(*dirty_x0));
    }
    if (dirty_x1 == NULL) {
        dirty_x1 = malloc(GRID_H_MAX * sizeof(*dirty_x1));
    }
    return step_stamps != NULL && lane_scratch != NULL && row_run_x0 != NULL && row_run_x1 != NULL && row_run_n != NULL
           && dirty_x0 != NULL && dirty_x1 != NULL;
}

static void
start_sim(void) {
    cell = qualities[quality].cell;
    grid_w = GFX_WIDTH / cell;
    grid_h = GFX_HEIGHT / cell;
    block_cols = (grid_w + SAND_BLOCK_W - 1) / SAND_BLOCK_W;
    block_rows = (grid_h + SAND_BLOCK_H - 1) / SAND_BLOCK_H;

    sim_accumulator_q8 = 0;
    pour_accumulator_ms = 0;
    sand_paint_row_state_init(&paint_row_state);
    sand_heal_init(&heal_policy, GFX_HEIGHT);
    gfx_heal_set_budget(SAND_HEAL_BUDGET_PIXELS);
    ui.brush = 0;
    ui.mode = SAND_MODE_PAINT;
    label_left_ms = 0;
    failed = false;
    input_ready = false;
    overlays_skipped_reason_logged = false;

    if (dirty_rows == NULL) {
        dirty_rows = malloc(GRID_H_MAX);
    }
    if (sleep_blocks == NULL) {
        sleep_blocks = malloc((size_t)BLOCK_COLS_MAX * BLOCK_ROWS_MAX);
    }
    if (grid == NULL) {
        grid = malloc((size_t)GRID_W_MAX * GRID_H_MAX);
    }
    /* impulse_buf allocates LAST, deliberately: grid needs the single
     * largest contiguous heap run, so it must pick first. Reordering does
     * not create more contiguous space, only decides who gets first pick -
     * moving impulse_buf ahead of grid produced a WORSE failure (no
     * memory for the grid at all). Do not reorder without a fresh device
     * capture showing it helps. */
    if (impulse_buf == NULL) {
        impulse_buf = malloc((size_t)APP_IMPULSE_MAX * sizeof(*impulse_buf));
        /* LOUD, NOT FATAL, unlike the buffers below: sand_enable_impulses
         * (NULL, ...) safely disables just DETONATE, so failing here alone
         * shouldn't strand a player who never wanted it behind a "no
         * memory" screen. Logs largest_free_block, not total free heap -
         * total free heap tells the wrong story here (see
         * SAND_IMPULSE_BUDGET_BYTES); largest block is what actually
         * predicts whether this allocation succeeds. */
        if (impulse_buf == NULL) {
            ESP_LOGE(TAG,
                     "Could not allocate the %d-entry blast buffer "
                     "(%u bytes) - detonate will be a no-op this "
                     "session; largest free block is %u",
                     APP_IMPULSE_MAX, (unsigned)((size_t)APP_IMPULSE_MAX * sizeof(*impulse_buf)),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }
    const bool bookkeeping_ok = alloc_grid_bookkeeping();
    if (grid == NULL || dirty_rows == NULL || sleep_blocks == NULL || !bookkeeping_ok) {
        ESP_LOGE(TAG,
                 "Could not allocate a %d x %d grid (%d bytes); "
                 "largest free block is %u",
                 GRID_W_MAX, GRID_H_MAX, GRID_W_MAX * GRID_H_MAX,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        failed = true;
        ui.screen = SAND_UI_RUNNING;
        return;
    }

    seed_row_runs_full_width();

    sand_init(&sim, grid, grid_w, grid_h, (uint32_t)esp_timer_get_time());
    sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
    sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);

    sand_track_dirty_rows(&sim, dirty_rows);
    sand_track_dirty_cols(&sim, dirty_x0, dirty_x1);

    sand_enable_sleeping(&sim, sleep_blocks);
    sand_enable_step_stamps(&sim, step_stamps);
    sand_enable_lane_scratch(&sim, lane_scratch);

    /* Enabled unconditionally, not just once BOOM is selected, so an
     * allocation failure shows up at start_sim() rather than on the first
     * tap in BOOM mode. */
    sand_enable_impulses(&sim, impulse_buf, APP_IMPULSE_MAX);
    tilt_reset(&tilt, IMU_COUNTS_PER_G);

    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - falling back to fixed downward gravity");
    }

    ESP_LOGI(TAG, "%d x %d grid, %d bytes, %d px cells", grid_w, grid_h, grid_w * grid_h, cell);

    apply_gfx_action(sand_colour_on_start_sim(&colour_state, (sand_colour_mode_t)color_mode));
    if (!sand_colour_indexed_active(&colour_state)) {
        gfx_clear(material_palette()[SAND_EMPTY]);
    }
    /* Explicit full-width spans for the first real draw: the menu that
     * called this can still paint its START button after the clear, and a
     * sentinel span would shrink to the first pour's cells and keep that
     * button. sand_invalidate() applies this before that first frame()
     * runs - this frame's own output is still the menu, drawn above. */
    gfx_request_full_redraw();

    ui.screen = SAND_UI_RUNNING;
}

#if CONFIG_LAUNCHER_SELFTEST
/* Test-only: starts the simulation and pours one brush's worth of sand at
 * the grid's centre, bypassing the menu's START button and a real touch
 * drag - a device suite exercising sand_frame()'s own dirty tracking has
 * no way to reach either through microui. Forces FULL regardless of
 * whatever a colour-mode test left color_mode at - a caller wanting the
 * real framebuffer to draw into (suite_sand_full_redraw.c, say) must not
 * silently start indexed instead. Returns the mode to restore afterward. */
int
sand_app_enter_running_for_test(void) {
    const int previous_mode = color_mode;
    color_mode = SAND_COLOR_FULL;
    start_sim();
    sand_spawn_cell(&sim, grid_w / 2, grid_h / 2, 3, brushes[0].cell);
    return previous_mode;
}

void
sand_app_restore_colour_mode_for_test(int mode) {
    color_mode = (sand_color_mode_t)mode;
}

/* Drives the crash's own reproduction with the real functions: enter, start
 * a sim in `mode`, then return to the menu the only real way that happens -
 * a fresh sand_enter() - and reports whether indexed mode survived it. Only
 * a boolean crosses back to the caller; the assertion belongs to the test.
 * Restores the mode that was selected before this test. */
/* Test-only: which of brushes[] a pour spawns. */
void
sand_app_select_brush_for_test(int brush) {
    ui.brush = brush;
}

bool
sand_app_test_survives_indexed_then_menu(int mode) {
    const int previous_mode = color_mode;
    color_mode = (sand_color_mode_t)mode;
    sand_enter();
    start_sim();
    sand_enter();
    const bool ok = !sand_colour_indexed_active(&colour_state);
    color_mode = (sand_color_mode_t)previous_mode;
    return ok;
}
#endif /* CONFIG_LAUNCHER_SELFTEST */

static void
sand_exit(void) {
    /* Other apps assume GFX_LAYOUT_FULL_FB/RGB565. */
    apply_gfx_action(sand_colour_on_exit_app(&colour_state));

    /* Grid is kept between visits (the app's largest allocation) so
     * re-entry cannot fail to heap fragmentation from whatever ran while
     * this app was closed. */
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (frames > 0) {
        ESP_LOGI(TAG,
                 "%lu frames, %lld sim steps, step %lld us, draw %lld us, "
                 "%lld of %d rows redrawn per frame, %lld px repainted per frame",
                 (unsigned long)frames, (long long)steps_total, (long long)(step_us_total / frames),
                 (long long)(draw_us_total / frames), (long long)(rows_redrawn_total / frames), grid_h,
                 (long long)(pixels_repainted_total / frames));
    }
#endif
}

/* Drawing */

#define SHINE_STEP_MS 40
#define SHINE_STEP_PX 2

static uint32_t shine_elapsed_ms;

/* The 5 gravity-relative directions material_wood_near_leaf() checks -
 * see material_wood_leaf_top5(). Recomputed once a frame, not per cell. */
static int wood_leaf_top5_down;

/* A sweep that always travels the same way still reads as one shine, even
 * with gusts dropping out - real wind swings direction. Interval jittered
 * (material_grain_hash of the flip count, not a real RNG) so the swings
 * are not metronomic. */
#define WOOD_LEAF_WIND_FLIP_BASE_MS   1200u
#define WOOD_LEAF_WIND_FLIP_JITTER_MS 1800u

static uint32_t wood_leaf_wind_flip_elapsed_ms;
static uint32_t wood_leaf_wind_flip_due_ms = WOOD_LEAF_WIND_FLIP_BASE_MS;
static unsigned wood_leaf_wind_flip_count;

#define FOAM_PHASE_MS 90

static uint32_t foam_elapsed_ms;

#define CULLET_PHASE_MS 250

static uint32_t cullet_elapsed_ms;

/* time_ms itself runs continuously for a smooth blend, but redraw cadence
 * is separately throttled by WOOD_LEAF_WAKE_MS - dirtying every wood-near-
 * leaf row every frame defeated the dirty-row system for a whole tree, the
 * same reasoning LOCAL_DEPTH_WAKE_MS already applies to liquid depth. */
#define WOOD_LEAF_WAKE_MS 40

static uint32_t wood_leaf_wake_elapsed_ms;

#define GLASS_PHASE_SHIFT 7

static int glass_last_phase;

/* A pool's INTERIOR - the bulk of its rows - is unaffected: a row with
 * any interior cell is already gated in, rim or not. Widening the gate
 * can only ADD the handful of edge-only rows a tighter condition would
 * skip; it cannot double the marked-row count the way gating on "any
 * liquid" from scratch would if the array gated on nothing at all. */
#define LOCAL_DEPTH_WAKE_MS 120

static uint32_t local_depth_wake_elapsed_ms;

static void
paint_row(gfx_color_t* fb, uint8_t* index_row, int cy, const uint8_t* row, int wx0, int wx1, bool force_full) {
    switch (cell) {
        case 2:
            sand_paint_row_n(&paint_row_state, &paint_frame, fb, index_row, cy, row, 2, grid_w, grid_h, wx0, wx1,
                             force_full);
            break;
        case 3:
            sand_paint_row_n(&paint_row_state, &paint_frame, fb, index_row, cy, row, 3, grid_w, grid_h, wx0, wx1,
                             force_full);
            break;
        case 4:
            sand_paint_row_n(&paint_row_state, &paint_frame, fb, index_row, cy, row, 4, grid_w, grid_h, wx0, wx1,
                             force_full);
            break;
        case 6:
            sand_paint_row_n(&paint_row_state, &paint_frame, fb, index_row, cy, row, 6, grid_w, grid_h, wx0, wx1,
                             force_full);
            break;
        case 8:
            sand_paint_row_n(&paint_row_state, &paint_frame, fb, index_row, cy, row, 8, grid_w, grid_h, wx0, wx1,
                             force_full);
            break;
        /* Unreachable for any cell size in qualities[]; falls back to size 2 to
     * avoid out-of-bounds writes. */
        default:
            sand_paint_row_n(&paint_row_state, &paint_frame, fb, index_row, cy, row, 2, grid_w, grid_h, wx0, wx1,
                             force_full);
            break;
    }
}

/* wx0/wx1: the columns actually worth repainting - see draw_dirty_row()'s
 * row_paint_span() call.
 * Run detection stays full-row, so row_run_x0/x1/n keeps seeing the row's
 * true shape, not just the part just repainted. `index_image` is NULL for
 * the RGB565 path; otherwise GFX_PIXFMT_INDEXED8's own index image, and
 * `fb` goes unused - see sand_paint_row_n()'s own comment. */
static int
draw_one_row(gfx_color_t* fb, uint8_t* index_image, int cy, uint16_t* cur_x0, uint16_t* cur_x1, int wx0, int wx1,
             bool force_full) {
    const uint8_t* row = &grid[cy * grid_w];
    uint8_t* index_row = index_image != NULL ? index_image + cy * grid_w : NULL;

    paint_row(fb, index_row, cy, row, wx0, wx1, force_full);

    int run_x0[ROW_MAX_RUNS], run_x1[ROW_MAX_RUNS];
    const int n = row_runs_find(row, grid_w, SAND_EMPTY, run_x0, run_x1);
    if (n < 0) {
        int x0, x1;
        row_runs_span_fallback(row, grid_w, SAND_EMPTY, &x0, &x1);
        cur_x0[0] = (uint16_t)x0;
        cur_x1[0] = (uint16_t)x1;
        return 1;
    }

    for (int i = 0; i < n; i++) {
        cur_x0[i] = (uint16_t)run_x0[i];
        cur_x1[i] = (uint16_t)run_x1[i];
    }
    return n;
}

/* Advances the travelling shine, and says whether it moved. */
static bool
advance_shine(uint32_t dt_ms) {
    shine_elapsed_ms += dt_ms;
    if (shine_elapsed_ms < SHINE_STEP_MS) {
        return false;
    }
    const uint32_t steps = shine_elapsed_ms / SHINE_STEP_MS;
    shine_elapsed_ms -= steps * SHINE_STEP_MS;
    paint_frame.shine_offset =
        (int)(((unsigned)paint_frame.shine_offset + steps * SHINE_STEP_PX) & (SAND_PAINT_SHINE_PERIOD - 1));
    return true;
}

static unsigned cullet_phase_index;

static bool
advance_cullet(uint32_t dt_ms) {
    cullet_elapsed_ms += dt_ms;
    if (cullet_elapsed_ms < CULLET_PHASE_MS) {
        return false;
    }
    const uint32_t steps = cullet_elapsed_ms / CULLET_PHASE_MS;
    cullet_elapsed_ms -= steps * CULLET_PHASE_MS;
    cullet_phase_index += steps;
    material_set_cullet_phase(cullet_phase_index);
    return true;
}

static bool
advance_wood_leaf_phase(uint32_t dt_ms) {
    paint_frame.wood_leaf_time_ms += dt_ms;
    wood_leaf_wake_elapsed_ms += dt_ms;
    if (wood_leaf_wake_elapsed_ms < WOOD_LEAF_WAKE_MS) {
        return false;
    }
    const uint32_t steps = wood_leaf_wake_elapsed_ms / WOOD_LEAF_WAKE_MS;
    wood_leaf_wake_elapsed_ms -= steps * WOOD_LEAF_WAKE_MS;
    return true;
}

static void
advance_wood_leaf_wind_sign(uint32_t dt_ms) {
    wood_leaf_wind_flip_elapsed_ms += dt_ms;
    if (wood_leaf_wind_flip_elapsed_ms < wood_leaf_wind_flip_due_ms) {
        return;
    }
    wood_leaf_wind_flip_elapsed_ms -= wood_leaf_wind_flip_due_ms;
    paint_frame.wood_leaf_wind_sign = -paint_frame.wood_leaf_wind_sign;
    wood_leaf_wind_flip_count++;
    wood_leaf_wind_flip_due_ms =
        WOOD_LEAF_WIND_FLIP_BASE_MS
        + material_grain_hash((int)wood_leaf_wind_flip_count, 0) % WOOD_LEAF_WIND_FLIP_JITTER_MS;
}

static bool
advance_local_depth_wake(uint32_t dt_ms) {
    local_depth_wake_elapsed_ms += dt_ms;
    if (local_depth_wake_elapsed_ms < LOCAL_DEPTH_WAKE_MS) {
        return false;
    }
    const uint32_t steps = local_depth_wake_elapsed_ms / LOCAL_DEPTH_WAKE_MS;
    local_depth_wake_elapsed_ms -= steps * LOCAL_DEPTH_WAKE_MS;
    return true;
}

static int
gravity_bearing_q16(int gx, int gy) {
    const int64_t ax = gx < 0 ? -(int64_t)gx : (int64_t)gx;
    const int64_t ay = gy < 0 ? -(int64_t)gy : (int64_t)gy;
    const int64_t denom = ax + ay;
    if (denom == 0) {
        return 0; /* flat or free fall: no bearing to report */
    }
    const int64_t p_q16 = ((int64_t)gx << 16) / denom; /* -65536..65536 */
    return (int)(gy < 0 ? (p_q16 - 65536) : (65536 - p_q16));
}

static bool
advance_glass_phase(int gx, int gy) {
    const int phase = gravity_bearing_q16(gx, gy) >> GLASS_PHASE_SHIFT;
    const bool changed = phase != glass_last_phase;
    glass_last_phase = phase;
    material_set_glass_phase(phase);
    return changed;
}

/* One row per bit here; unlike dirty_rows[] this scratch never survives
 * past the call it was set in, so it needs no reset elsewhere. */
static bool wake_hit[GRID_H_MAX];

static void
mark_wake_hits(bool moved, uint8_t flag) {
    if (!moved) {
        return;
    }
    for (int cy = 0; cy < grid_h; cy++) {
        if (paint_row_state.row_flags[cy] & flag) {
            dirty_rows[cy] = 1;
            wake_hit[cy] = true;
        }
    }
}

/* The span of row cy worth repainting: the sim's own changed-column union,
 * widened one column either way for the edge and wood/leaf neighbour
 * checks, plus a wake tick's own flagged-cell span when one fires. No span
 * recorded at all falls back to the row's full width, as before. */
static void
row_paint_span(int cy, int* out_x0, int* out_x1) {
    int x0 = dirty_x0[cy];
    int x1 = dirty_x1[cy];

    /* Sentinel x0/x1 (grid_w, 0) is already the correct identity element
     * for a min/max union - it never wins against a real span below. */
    if (wake_hit[cy]) {
        if (paint_row_state.row_flag_x0[cy] < x0) {
            x0 = paint_row_state.row_flag_x0[cy];
        }
        if (paint_row_state.row_flag_x1[cy] > x1) {
            x1 = paint_row_state.row_flag_x1[cy];
        }
    }

    if (x0 >= x1) {
        *out_x0 = 0;
        *out_x1 = grid_w;
        return;
    }
    x0 -= 1;
    x1 += 1;
    *out_x0 = x0 < 0 ? 0 : x0;
    *out_x1 = x1 > grid_w ? grid_w : x1;
}

/* Clipped to the span actually repainted: a send range outside
 * [wx0,wx1) provably did not change (sand_paint_row_n()'s own comment).
 * Indexed modes narrow further, to row_changed_x0/x1 - a cell visited but
 * left untouched dithers the same as before, not merely unpainted.
 * Returns the pixels marked. */
static int64_t
mark_row_sends(int cy, int wx0, int wx1, const uint16_t* send_x0, const uint16_t* send_x1, int send_n, bool indexed,
               bool healing) {
    int64_t pixels = 0;
    for (int i = 0; i < send_n; i++) {
        int sx0, sx1;
        if (!sand_paint_clip_send(send_x0[i], send_x1[i], wx0, wx1, indexed, paint_row_state.row_changed_x0[cy],
                                  paint_row_state.row_changed_x1[cy], &sx0, &sx1)) {
            continue;
        }
        gfx_mark_dirty(sx0 * cell, cy * cell, (sx1 - sx0) * cell, cell);
        if (healing) {
            sand_heal_note_rows(&heal_policy, cy * cell, (cy + 1) * cell);
        }
#if CONFIG_LAUNCHER_DEVELOPMENT
        pixels += (int64_t)(sx1 - sx0) * cell * cell;
#endif
    }
    return pixels;
}

/* Repaints dirty row `cy` and marks what changed since its last paint;
 * returns the pixels marked. */
static int64_t
draw_dirty_row(gfx_color_t* fb, uint8_t* index_image, int cy, bool force_full, bool indexed, bool healing) {
    dirty_rows[cy] = 0;

    int wx0, wx1;
    row_paint_span(cy, &wx0, &wx1);
    dirty_x0[cy] = (uint16_t)grid_w;
    dirty_x1[cy] = 0;

    uint16_t cur_x0[ROW_MAX_RUNS], cur_x1[ROW_MAX_RUNS];
    const int cur_n = draw_one_row(fb, index_image, cy, cur_x0, cur_x1, wx0, wx1, force_full);

    uint16_t* prev_x0 = &row_run_x0[cy * ROW_MAX_RUNS];
    uint16_t* prev_x1 = &row_run_x1[cy * ROW_MAX_RUNS];
    const int prev_n = row_run_n[cy];

    uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
    const int send_n = row_runs_reconcile(cur_x0, cur_x1, cur_n, prev_x0, prev_x1, prev_n, send_x0, send_x1);

    const int64_t pixels = mark_row_sends(cy, wx0, wx1, send_x0, send_x1, send_n, indexed, healing);

    for (int i = 0; i < cur_n; i++) {
        prev_x0[i] = cur_x0[i];
        prev_x1[i] = cur_x1[i];
    }
    row_run_n[cy] = (uint8_t)cur_n;
    return pixels;
}

static void
draw_dirty_rows(bool shine_moved, bool local_depth_woke, bool cullet_moved, bool glass_moved, bool wood_leaf_moved) {
    const bool indexed = sand_colour_indexed_active(&colour_state);
    gfx_color_t* fb = indexed ? NULL : gfx_framebuffer();
    uint8_t* index_image = indexed ? gfx_indexed_image() : NULL;

    /* Captured once, then cleared, so a request made mid-frame (the next
     * mark_sand_fully_dirty()) affects the NEXT pass, not this one - see
     * indexed_force_full_repaint's own comment and gfx.c's identical
     * band_force_all_dirty idiom. */
    const bool force_full = indexed_force_full_repaint;
    indexed_force_full_repaint = false;
    const bool healing = gfx_heal_active();

    memset(wake_hit, 0, (size_t)grid_h * sizeof(*wake_hit));
    mark_wake_hits(shine_moved, SAND_PAINT_ROW_FLAG_SHINE);
    mark_wake_hits(local_depth_woke, SAND_PAINT_ROW_FLAG_LIQUID);
    mark_wake_hits(cullet_moved, SAND_PAINT_ROW_FLAG_CULLET);
    mark_wake_hits(glass_moved, SAND_PAINT_ROW_FLAG_GLASS);
    mark_wake_hits(wood_leaf_moved, SAND_PAINT_ROW_FLAG_WOOD_LEAF);

#if CONFIG_LAUNCHER_DEVELOPMENT
    int redrawn = 0;
    int64_t pixels_repainted = 0;
#endif

    /* Gravity-UP reverses this loop's order: cur_row[]/prev_row[]'s pointer
     * swap only means "row painted before this one" if rows are visited
     * surface-first. Reversing the loop, not the array's meaning, is safe
     * since nothing else here depends on row order - dirty_rows[cy] and
     * friends index by cy directly, and gfx_mark_dirty() only unions a
     * bounding box. */
    const bool reverse_rows = paint_row_state.local_depth_v_reverse;

    for (int i = 0; i < grid_h; i++) {
        const int cy = reverse_rows ? (grid_h - 1 - i) : i;
        if (!dirty_rows[cy]) {
            continue;
        }
        const int64_t row_pixels = draw_dirty_row(fb, index_image, cy, force_full, indexed, healing);
#if CONFIG_LAUNCHER_DEVELOPMENT
        redrawn++;
        pixels_repainted += row_pixels;
#else
        (void)row_pixels;
#endif
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    rows_redrawn_total += redrawn;
    pixels_repainted_total += pixels_repainted;
#endif
}

static void
heal_settled_rows(void) {
    if (!gfx_heal_active()) {
        return;
    }
    sand_heal_span_t spans[SAND_HEAL_MAX_SPANS];
    const int n = sand_heal_step(&heal_policy, spans, SAND_HEAL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        gfx_heal_mark(0, spans[i].y0, GFX_WIDTH, spans[i].y1 - spans[i].y0);
    }
}

#define EMITTER_MARKER_COLOR 0xFF3EC8

#define EMITTER_MARKER_PX    12

static void
draw_emitter_markers(void) {
    const gfx_color_t marker = gfx_rgb(EMITTER_MARKER_COLOR);
    const int count = sand_emitter_count(&sim);

    for (int i = 0; i < count; i++) {
        int ex, ey;
        cell_t ecell;
        if (!sand_emitter_at(&sim, i, &ex, &ey, &ecell)) {
            continue; /* not expected - see sand_emitter_count()'s contract */
        }
        (void)ecell; /* the marker's colour is fixed, not the material's */

        const int mid_x = ex * cell + cell / 2;
        const int mid_y = ey * cell + cell / 2;
        const int px = mid_x - EMITTER_MARKER_PX / 2;
        const int py = mid_y - EMITTER_MARKER_PX / 2;
        gfx_fill_rect(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX, marker);
        gfx_mark_dirty(px, py, EMITTER_MARKER_PX, EMITTER_MARKER_PX);
    }
}

static int
gravity_quarter_turn(int gx, int gy) {
    const int ax = gx < 0 ? -gx : gx;
    const int ay = gy < 0 ? -gy : gy;

    if (ay >= ax) {
        return (gy >= 0) ? 0 : 2; /* down is down : board upside down */
    }
    return (gx >= 0) ? 3 : 1; /* down is to the right : to the left */
}

/* Computes its own turn (gravity_quarter_turn()) rather than inheriting the
 * shell's, unlike draw_palette(): that one goes through microui, which the
 * shell's transform reaches; this one calls gfx_text_turned() straight onto
 * the canvas, bypassing microui entirely, the same way the sand grid itself
 * is painted. Canvas draws don't get the shell's transform for free - only
 * chrome does. */
static void
draw_mode_label(int gx, int gy) {
    char text_buf[24];
    const char* text;
    if (ui.mode == SAND_MODE_DETONATE) {
        text = "BOOM";
    } else if (ui.mode == SAND_MODE_ERASE) {
        text = "ERASE";
    } else if (ui.modes[ui.brush] == BRUSH_SPAWN) {
        snprintf(text_buf, sizeof text_buf, "%s SOURCE", material_name(brushes[ui.brush].cell));
        text = text_buf;
    } else {
        text = material_name(brushes[ui.brush].cell);
    }
    const int len = (int)strlen(text);
    const int span = len * 8 * LABEL_SCALE;
    const int tall = 8 * LABEL_SCALE;

    const int turn = gravity_quarter_turn(gx, gy);

    const int upright_w = (turn % 2 == 0) ? GFX_WIDTH : GFX_HEIGHT;
    const int upright_h = (turn % 2 == 0) ? GFX_HEIGHT : GFX_WIDTH;
    const mu_Rect upright = ui_anchor_rect((mu_Rect){0, 0, upright_w, upright_h}, UI_ANCHOR_TOP, UI_ANCHOR_TOP, 0,
                                           LABEL_MARGIN, span, tall);
    const mu_Rect box = ui_transform_rect(ui_transform_quarter_turn(turn, GFX_WIDTH, GFX_HEIGHT), upright);
    int x = 0;
    int y = 0;
    ui_text_glyph0_origin(gfx_font_ui(), box, turn, LABEL_SCALE, &x, &y);

    gfx_color_t ink;
    if (ui.mode == SAND_MODE_DETONATE) {
        ink = gfx_rgb(0xFF3B3B);
    } else if (ui.mode == SAND_MODE_ERASE) {
        ink = gfx_rgb(0xFF8A5C);
    } else {
        ink = material_brush_color(brushes[ui.brush].cell);
    }

    gfx_text_turned(x, y, text, ink, LABEL_SCALE, turn);
}

/* APPLY EXACTLY ONCE PER REPAINT OF WHAT IS UNDERNEATH, never per frame.
 * gfx_fill_rect_blend() mixes with the destination it reads, and the sand
 * behind a panel is frozen, so a second application lands on the first's own
 * output and the picture walks toward black a frame at a time. The backdrop
 * is only fresh the frame a panel opens and on a turn taken while it is
 * open; both call this, nothing else may. Cost rules out per frame anyway -
 * this reads all 368x448 pixels. */
#define PANEL_SCRIM_ALPHA 110

static void
dim_backdrop(void) {
    gfx_fill_rect_blend(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x000000), PANEL_SCRIM_ALPHA);
}

static void
draw_palette(const input_t* input) {
    mu_Context* ctx = ui_context();
    ui_begin(input);
    palette_screen_draw(ctx, &ui);
    ui_end(UI_NO_BACKGROUND);
}

static void
draw_brush_screen(const input_t* input) {
    mu_Context* ctx = ui_context();
    ui_begin(input);
    brush_screen_draw(ctx, &ui);
    ui_end(UI_NO_BACKGROUND);
}

/* Frame */

/* State sand_update() computes for sand_frame() to draw with, on the same
 * pass - see the app.h contract: update() must not touch gfx, so every
 * advance_*() result it needs to hand off is state, not a draw call. */
static int pending_gx, pending_gy;
static bool label_dirty_this_frame;

/* sand_ui_step()'s actions, stepped in sand_update() and drawn in
 * sand_frame(). A pass with no update() (the first frame after entering)
 * leaves ui_stepped_this_pass false, and sand_frame() steps the UI itself. */
static unsigned pending_ui_actions;
static bool ui_stepped_this_pass;
static bool pending_shine_moved, pending_local_depth_woke, pending_cullet_moved, pending_glass_moved,
    pending_wood_leaf_moved;
#if CONFIG_LAUNCHER_DEVELOPMENT
static int64_t pending_step_us;
static int pending_awake_blocks, pending_awake_cells;
#endif

static void
read_gravity_input(uint32_t dt_ms, imu_sample_t* sample, int* gx, int* gy, int* flow, int* jostle, int* rotation) {
    *gx = 0;
    *gy = IMU_COUNTS_PER_G;
    *flow = 256;
    *jostle = 0;
    *rotation = 0;

    if (!imu_ready() || !imu_read(sample)) {
        return;
    }

    *rotation = imu_rotation_level(sample);

    tilt_update(&tilt, imu_gravity_screen_x(sample), imu_gravity_screen_y(sample), sample->az, *rotation, dt_ms);

    *gx = tilt_x(&tilt);
    *gy = tilt_y(&tilt);
    *flow = tilt_strength(&tilt);

    const int shake = tilt_shake(&tilt);
    *jostle = shake > SHAKE_DEADZONE ? shake : 0;
}

static void
handle_detonate_input(const input_t* input) {
    pour_accumulator_ms = 0; /* do not let held time leak into paint/erase */
    if (!input->pressed) {
        return;
    }
    const int cx = input->x / cell;
    const int cy = input->y / cell;
    sand_explode(&sim, cx, cy, (sand_ui_radius(&ui) + cell / 2) / cell);
}

static void
handle_spawn_emitter_input(const input_t* input) {
    if (!input->pressed) {
        return;
    }
    const int cx = input->x / cell;
    const int cy = input->y / cell;
    if (!sand_add_emitter(&sim, cx, cy, brushes[ui.brush].cell)) {
        ESP_LOGW(TAG, "emitter list full (%d) - tap ignored", SAND_MAX_EMITTERS);
    }
}

static void
apply_pour_step(int cx, int cy) {
    if (ui.mode == SAND_MODE_ERASE) {
        sand_erase(&sim, cx, cy, (sand_ui_radius(&ui) + cell / 2) / cell);
        /* Wider than the sweep above on purpose - see
         * ERASE_EMITTER_RADIUS_PX's own comment for why a point target
         * needs more aiming tolerance than an area sweep does. */
        sand_remove_emitters(&sim, cx, cy, (ERASE_EMITTER_RADIUS_PX + cell / 2) / cell);
        return;
    }
    sand_spawn_cell_share(&sim, cx, cy, (sand_ui_radius(&ui) + cell / 2) / cell, brushes[ui.brush].cell,
                          brushes[ui.brush].share_pct);
}

static void
handle_pour_input(const input_t* input, uint32_t dt_ms) {
    if (ui.mode == SAND_MODE_DETONATE) {
        handle_detonate_input(input);
        return;
    }

    if (!input->down) {
        pour_accumulator_ms = 0;
        return;
    }

    if (ui.mode == SAND_MODE_PAINT && ui.modes[ui.brush] == BRUSH_SPAWN) {
        handle_spawn_emitter_input(input);
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

    const int cx = input->x / cell;
    const int cy = input->y / cell;
    for (int i = 0; i < applications; i++) {
        apply_pour_step(cx, cy);
    }
}

/* Logs the direction whenever the NEAREST of the eight changes. Quiet when
 * the board is still, and it is what the axis mapping above was verified
 * against. The simulation itself uses the dithered direction, which changes
 * every frame by design and would be useless to log. */
static void
log_direction_change(int gx, int gy, int jostle, const imu_sample_t* sample) {
    static int last_dx = 99, last_dy = 99;
    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);
    if (dx == last_dx && dy == last_dy) {
        return;
    }
    ESP_LOGI(TAG,
             "down is (%+d,%+d)  smoothed (%+6d,%+6d)  "
             "raw (%+6d,%+6d)  shake %d",
             dx, dy, gx, gy, sample->ax, sample->ay, jostle);
    last_dx = dx;
    last_dy = dy;
}

static void
run_sim_steps(int gx, int gy, int jostle, int flow, uint32_t dt_ms) {
    sim_accumulator_q8 += dt_ms * (uint32_t)flow;
    int steps = (int)(sim_accumulator_q8 / (SIM_STEP_MS * 256));
    if (steps > SIM_MAX_CATCHUP) {
        steps = SIM_MAX_CATCHUP;
        sim_accumulator_q8 = 0; /* give up on the backlog */
    } else {
        sim_accumulator_q8 -= (uint32_t)steps * SIM_STEP_MS * 256;
    }

    for (int i = 0; i < steps; i++) {
        sand_step(&sim, gx, gy, jostle);
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    steps_total += steps;
#endif
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static int
count_occupied_in_block(int bx, int by) {
    const int x0 = bx * SAND_BLOCK_W;
    const int x1 = (x0 + SAND_BLOCK_W < grid_w) ? x0 + SAND_BLOCK_W : grid_w;
    const int y0 = by * SAND_BLOCK_H;
    const int y1 = (y0 + SAND_BLOCK_H < grid_h) ? y0 + SAND_BLOCK_H : grid_h;

    int cells = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t* row = &grid[(size_t)y * grid_w];
        for (int x = x0; x < x1; x++) {
            if (row[x] != SAND_EMPTY) {
                cells++;
            }
        }
    }
    return cells;
}

static void
count_awake(int* out_blocks, int* out_cells) {
    int blocks = 0, cells = 0;
    for (int by = 0; by < block_rows; by++) {
        for (int bx = 0; bx < block_cols; bx++) {
            if (sand_block_settled(&sim, bx, by)) {
                continue;
            }
            blocks++;
            cells += count_occupied_in_block(bx, by);
        }
    }
    *out_blocks = blocks;
    *out_cells = cells;
}

static void
track_pour_split(const input_t* input, int64_t step_us, int64_t draw_us, int awake_blocks, int awake_cells,
                 int64_t now) {
    if (input->down && ui.mode == SAND_MODE_PAINT) {
        pour_step_us_total += step_us;
        pour_draw_us_total += draw_us;
        pour_awake_total += awake_blocks;
        pour_awake_cells_total += awake_cells;
        pour_frames++;
    } else {
        idle_step_us_total += step_us;
        idle_draw_us_total += draw_us;
        idle_awake_total += awake_blocks;
        idle_awake_cells_total += awake_cells;
        idle_frames++;
    }

    if (now < split_log_at_us) {
        return;
    }
    if (pour_frames > 0) {
        const int64_t blocks = pour_awake_total / pour_frames;
        const int64_t cells = pour_awake_cells_total / pour_frames;
        ESP_LOGI(TAG,
                 "POURING:     %lu frames, step %lld us, draw %lld us, "
                 "%lld of %d blocks awake, %lld cells/awake block",
                 (unsigned long)pour_frames, (long long)(pour_step_us_total / pour_frames),
                 (long long)(pour_draw_us_total / pour_frames), (long long)blocks, block_cols * block_rows,
                 (long long)(blocks > 0 ? cells / blocks : 0));
    }
    if (idle_frames > 0) {
        const int64_t blocks = idle_awake_total / idle_frames;
        const int64_t cells = idle_awake_cells_total / idle_frames;
        ESP_LOGI(TAG,
                 "NOT POURING: %lu frames, step %lld us, draw %lld us, "
                 "%lld of %d blocks awake, %lld cells/awake block",
                 (unsigned long)idle_frames, (long long)(idle_step_us_total / idle_frames),
                 (long long)(idle_draw_us_total / idle_frames), (long long)blocks, block_cols * block_rows,
                 (long long)(blocks > 0 ? cells / blocks : 0));
    }
    pour_step_us_total = pour_draw_us_total = 0;
    idle_step_us_total = idle_draw_us_total = 0;
    pour_awake_total = idle_awake_total = 0;
    pour_awake_cells_total = idle_awake_cells_total = 0;
    pour_frames = idle_frames = 0;
    split_log_at_us = now + 2000000;
}

#endif

static void
adopt_options(const sand_options_t* options) {
    quality = options->quality;
    color_mode = (sand_color_mode_t)options->color;
    dither_mode = (gfx_dither_mode_t)options->dither;
}

static void
draw_options(mu_Context* ctx) {
    if (!mode_swatches_ready) {
        sand_mode_swatches(sand_dither_none_lut, sand_palette256_lut, GFX_INDEXED_PALETTE_SIZE, SAND_PALETTE_UI_ENTRIES,
                           mode_swatches);
        mode_swatches_ready = true;
    }
    const char* quality_names[QUALITY_COUNT];
    for (int i = 0; i < QUALITY_COUNT; i++) {
        quality_names[i] = qualities[i].name;
    }
    const options_screen_labels_t labels = {
        .quality_names = quality_names,
        .quality_count = QUALITY_COUNT,
        .dither_names = dither_names,
        .dither_count = GFX_DITHER_MODE_COUNT,
        .mode_swatches = mode_swatches,
    };
    const sand_options_t committed = current_options();
    const sand_options_hits_t hits = options_screen_draw(ctx, &menu, committed, &labels);
    if (sand_menu_options_step(&menu, committed, hits)) {
        /* Not applied to a running sim - the next start_sim() reads them. */
        adopt_options(&menu.draft);
    }
}

static void
draw_title(mu_Context* ctx) {
    switch (sand_menu_title_clicked(&menu, title_screen_draw(ctx), current_options())) {
        case SAND_MENU_START:
            /* Not called here - see pending_start's own comment. */
            pending_start = true;
            break;
        case SAND_MENU_EXIT: shell_request_exit(); break;
        case SAND_MENU_STAY: break;
    }
}

static void
draw_menu(const input_t* input) {
    mu_Context* ctx = ui_context();

    ui_begin(input);
    if (menu.screen == SAND_MENU_OPTIONS) {
        draw_options(ctx);
    } else {
        draw_title(ctx);
    }
    ui_end(COL_BACKGROUND);
}

/* Everything the play screen needs each pass that does not draw: gravity and
 * pour input, the sim itself, and the state (not pixels) the advance_*()
 * family produces for sand_frame() to paint with. Returns early - untouched
 * state, no gfx - whenever the play screen is not the one showing, since the
 * menu/palette/brush screens are drawn, not simulated. */
static void
sand_update(uint32_t dt_ms, const input_t* input) {
    if (ui.screen == SAND_UI_MENU || failed) {
        return;
    }

    /* UI FIRST, as when this all ran inside sand_frame(): a tap that opens
     * or closes a panel is the UI's, so it must be consumed before
     * handle_pour_input() below could pour under the button. sand_ui_step()
     * is state only; sand_frame() does the drawing its actions ask for. */
    const unsigned actions = sand_ui_step(&ui, input);
    pending_ui_actions |= actions;
    ui_stepped_this_pass = true;

    if (actions & (SAND_UI_CLOSE_PALETTE | SAND_UI_CLOSE_BRUSH)) {
        if (actions & SAND_UI_SHOW_LABEL) {
            label_left_ms = LABEL_MS;
        }
        sim_accumulator_q8 = 0;
        pour_accumulator_ms = 0;
        return;
    }
    if (actions & (SAND_UI_OPEN_PALETTE | SAND_UI_OPEN_BRUSH)) {
        label_left_ms = 0;
    }
    if (ui.screen != SAND_UI_RUNNING) {
        return;
    }

    int gx, gy, flow, jostle, rotation;
    imu_sample_t sample = {0};
    read_gravity_input(dt_ms, &sample, &gx, &gy, &flow, &jostle, &rotation);

    label_dirty_this_frame = label_left_ms > 0;
    if (label_dirty_this_frame) {
        label_left_ms = (dt_ms >= label_left_ms) ? 0 : (label_left_ms - dt_ms);
    }

    if (input_ready) {
        handle_pour_input(input, dt_ms);
    } else if (!input->down) {
        input_ready = true;
    }
    log_direction_change(gx, gy, jostle, &sample);

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t0 = esp_timer_get_time();
#endif

    run_sim_steps(gx, gy, jostle, flow, dt_ms);

    material_set_gravity(gx, gy);

    material_shine_direction(gx, gy, &paint_frame.shine_ux_q8, &paint_frame.shine_uy_q8);

    material_wood_leaf_wind_axis(gx, gy, &paint_frame.wood_leaf_wind_ux_q8, &paint_frame.wood_leaf_wind_uy_q8);

    material_wood_leaf_top5(gx, gy, &wood_leaf_top5_down, paint_frame.wood_leaf_top5);

    advance_wood_leaf_wind_sign(dt_ms);

    sand_paint_update_local_depth_gravity(&paint_row_state, gx, gy, grid_w, grid_h);

    foam_elapsed_ms += dt_ms;
    material_set_foam_phase(foam_elapsed_ms / FOAM_PHASE_MS);

#if CONFIG_LAUNCHER_DEVELOPMENT
    pending_step_us = esp_timer_get_time() - t0;
    count_awake(&pending_awake_blocks, &pending_awake_cells);
#endif

    /* Local-depth wake, cullet cycle, shine, and the wood-leaf swing each
     * have their own clock tick and row array. Driven by dt_ms, not frame
     * count. Glass's wake uses gravity_bearing_q16(). State only - each
     * result feeds draw_sim_frame()'s draw_dirty_rows() call. */
    pending_shine_moved = advance_shine(dt_ms);
    pending_local_depth_woke = advance_local_depth_wake(dt_ms);
    pending_cullet_moved = advance_cullet(dt_ms);
    pending_glass_moved = advance_glass_phase(gx, gy);
    pending_wood_leaf_moved = advance_wood_leaf_phase(dt_ms);

    pending_gx = gx;
    pending_gy = gy;
}

/* This pass's UI actions: the ones sand_ui_step() already produced
 * earlier this pass, or a fresh step of it now. */
static unsigned
take_ui_actions(const input_t* input) {
    unsigned actions;
    if (ui_stepped_this_pass) {
        actions = pending_ui_actions;
    } else {
        actions = sand_ui_step(&ui, input);
        if (actions & (SAND_UI_CLOSE_PALETTE | SAND_UI_CLOSE_BRUSH)) {
            if (actions & SAND_UI_SHOW_LABEL) {
                label_left_ms = LABEL_MS;
            }
            sim_accumulator_q8 = 0;
            pour_accumulator_ms = 0;
        }
        if (actions & (SAND_UI_OPEN_PALETTE | SAND_UI_OPEN_BRUSH)) {
            label_left_ms = 0;
        }
    }
    pending_ui_actions = 0;
    ui_stepped_this_pass = false;
    return actions;
}

static void
close_overlay_screen(void) {
    /* Restores UI_TEXT_PLAIN so the palette's outline style doesn't leak
     * into the next UI drawn (text style stays in force until changed -
     * ui.h); the brush screen only ever used PLAIN, so this is a no-op
     * on that path. main.c owns the transform for the whole shell,
     * sampling real orientation on its own schedule - an app must not
     * touch it; resetting it here would fight the shell the moment the
     * board is actually held sideways. */
    ui_set_text_style(UI_TEXT_PLAIN);

    apply_gfx_action(sand_colour_on_close_overlay(&colour_state));

    sim_accumulator_q8 = 0;
    pour_accumulator_ms = 0;
    /* sand_invalidate() applies the full-width spans before the next
     * frame() - this pass returns without drawing the grid at all. */
    gfx_request_full_redraw();
}

static void
open_overlay_screen(void) {
    label_left_ms = 0;

    /* The palette/brush screen composites over whatever the framebuffer
     * already holds (UI_NO_BACKGROUND, so frozen sand shows through the
     * grout) - indexed mode never wrote one, so it must repaint the RGB565
     * path's own backdrop before that screen dims and draws over it. */
    if (sand_colour_on_open_overlay(&colour_state) == SAND_GFX_EXIT_TO_FULL) {
        gfx_mode_exit();
        gfx_clear(material_palette()[SAND_EMPTY]);
        mark_sand_fully_dirty();
        draw_dirty_rows(false, false, false, false, false);
    }
}

/* Dims the backdrop when the screen opens, and repaints it when the board
 * turns while the palette or brush screen is open: the palette paints
 * UI_NO_BACKGROUND (frozen sand shows through the grout) and the opaque
 * brush screen leaves uncovered the corners the grid occupied before the
 * turn, so either leaves a ghost. Full-canvas, since the sand never
 * rotates; markers are redrawn because the grid does not store them. The
 * sim is paused, so no wake ticks. */
static void
refresh_overlay_backdrop(bool just_opened) {
    const int quarter = display_shell_quarter();

    if (just_opened) {
        ui_invalidate();

        dim_backdrop();
        panel_drawn_quarter = quarter;
    } else if (quarter != panel_drawn_quarter) {
        mark_sand_fully_dirty();
        draw_dirty_rows(false, false, false, false, false);
        draw_emitter_markers();
        dim_backdrop();
        panel_drawn_quarter = quarter;
    }
}

/* Markers and the mode label draw straight onto the canvas outside the
 * indexed pipeline (gfx_target.h has no INDEXED8 case yet). Reported
 * once per run rather than skipped silently - see
 * overlays_skipped_reason_logged's own declaration. */
static void
draw_canvas_overlays(void) {
    if (!sand_colour_indexed_active(&colour_state)) {
        draw_emitter_markers();

        if (label_left_ms > 0) {
            draw_mode_label(pending_gx, pending_gy);
        }
    } else if (!overlays_skipped_reason_logged) {
        ESP_LOGW(TAG, "COLOUR %s: emitter markers and the mode label do not draw yet - FULL-only for now",
                 color_names[color_mode]);
        overlays_skipped_reason_logged = true;
    }
}

static void
draw_sim_frame(const input_t* input) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t1 = esp_timer_get_time();
#else
    (void)input;
#endif

    if (label_dirty_this_frame) {
        memset(dirty_rows, 1, (size_t)grid_h);
        /* Full width, not whatever the sim narrowed this step to: the
         * label's own erase can leave sand pixels stale under it with no
         * grid cell having changed there at all. */
        reset_dirty_cols_full_width();
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }

    draw_dirty_rows(pending_shine_moved, pending_local_depth_woke, pending_cullet_moved, pending_glass_moved,
                    pending_wood_leaf_moved);
    heal_settled_rows();
    draw_canvas_overlays();

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t t2 = esp_timer_get_time();
    step_us_total += pending_step_us;
    draw_us_total += t2 - t1;
    frames++;
    track_pour_split(input, pending_step_us, t2 - t1, pending_awake_blocks, pending_awake_cells, t2);
#endif
}

static void
sand_frame(uint32_t dt_ms, const input_t* input) {
    if (pending_start) {
        /* Outside any ui_begin()/ui_end() - the same requirement
         * open_overlay_screen()'s own SAND_GFX_EXIT_TO_FULL repaint
         * already meets. */
        pending_start = false;
        start_sim();
    }

    if (ui.screen == SAND_UI_MENU) {
        /* Last-resort guard: the menu has no indexed draw path, so it must
         * never draw while indexed mode is active, whatever put it there -
         * sand_enter() already asks for this, but this is what keeps a
         * future path back to the menu from reintroducing the crash rather
         * than a second place trusting it stays covered. */
        apply_gfx_action(sand_colour_on_enter_menu(&colour_state));
        draw_menu(input);
        return;
    }

    if (failed) {
        gfx_clear(gfx_rgb(0x1A0C0C));
        gfx_text(20, GFX_HEIGHT / 2, "no memory for the grid", gfx_rgb(0xFF5C5C));
        return;
    }

    const unsigned actions = take_ui_actions(input);

    if (actions & (SAND_UI_CLOSE_PALETTE | SAND_UI_CLOSE_BRUSH)) {
        close_overlay_screen();
        return;
    }

    if (actions & (SAND_UI_OPEN_PALETTE | SAND_UI_OPEN_BRUSH)) {
        open_overlay_screen();
    }

    if (ui.screen == SAND_UI_PALETTE) {
        refresh_overlay_backdrop((actions & SAND_UI_OPEN_PALETTE) != 0);
        draw_palette(input);
        return;
    }

    if (ui.screen == SAND_UI_BRUSH) {
        refresh_overlay_backdrop((actions & SAND_UI_OPEN_BRUSH) != 0);
        draw_brush_screen(input);
        return;
    }

    draw_sim_frame(input);
}

#if CONFIG_LAUNCHER_SELFTEST
static void
sand_app_test_tap_menu_rect(mu_Rect r) {
    const int x = r.x + r.w / 2;
    const int y = r.y + r.h / 2;
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y};
    const input_t hold = {.down = true, .x = x, .y = y};
    const input_t release = {.released = true, .x = x, .y = y};
    const input_t idle = {0};
    draw_menu(&press);
    for (int i = 0; i < 4; i++) {
        draw_menu(&hold);
    }
    draw_menu(&release);
    draw_menu(&idle);
}

bool
sand_app_test_options_reach_start(sand_test_start_action_t action) {
    const sand_options_t saved = current_options();
    ui_set_transform(ui_transform_identity());
    sand_enter();
    const sand_options_t before = current_options();
    sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS, before);
    menu.draft.quality = (before.quality + 1) % QUALITY_COUNT;

    if (action == SAND_TEST_CANCEL_THEN_START || action == SAND_TEST_APPLY_THEN_START) {
        const input_t idle = {0};
        draw_menu(&idle);
        draw_menu(&idle);
        options_screen_layout_t lay;
        options_screen_layout(ui_width(), ui_height(), &lay);
        sand_app_test_tap_menu_rect(action == SAND_TEST_APPLY_THEN_START ? lay.apply : lay.cancel);
    } else {
        sand_menu_init(&menu);
    }

    title_screen_layout_t title;
    title_screen_layout(ui_width(), ui_height(), &title);
    sand_app_test_tap_menu_rect(title.buttons[SAND_TITLE_START]);
    const bool queued = pending_start;
    const input_t idle = {0};
    sand_frame(0, &idle);
    const int expected_quality = action == SAND_TEST_APPLY_THEN_START ? menu.draft.quality : before.quality;
    const bool ok = queued && ui.screen == SAND_UI_RUNNING && !failed && current_options().quality == expected_quality
                    && cell == qualities[expected_quality].cell;
    sand_exit();
    adopt_options(&saved);
    ui_set_transform(ui_transform_quarter_turn(display_shell_quarter(), GFX_WIDTH, GFX_HEIGHT));
    return ok;
}

/* Unlike sand_app_test_survives_indexed_then_menu() above, this runs the
 * START button itself. One pressed+released frame is not enough: microui's
 * hover_root lags next_hover_root by a frame (begin_root_container(),
 * microui.c), so a brand-new window cannot grant hover the instant it
 * opens - PRESS/HOLD below replay ui_pointer_step()'s own
 * UI_POINTER_HOVER_FRAMES wait for that; `released` on the third frame
 * folds DOWN and UP into the same draw_menu() call, leaving no touch
 * state behind. */
bool
sand_app_test_start_button_survives_the_ui_build(int mode) {
    const int previous_mode = color_mode;
    color_mode = (sand_color_mode_t)mode;
    ui_set_transform(ui_transform_identity());
    sand_enter();

    title_screen_layout_t title;
    title_screen_layout(ui_width(), ui_height(), &title);
    const mu_Rect start_rect = title.buttons[SAND_TITLE_START];
    const int cx = start_rect.x + start_rect.w / 2;
    const int cy = start_rect.y + start_rect.h / 2;
    ESP_LOGI(TAG, "START tap test: tapping (%d, %d), START rect (%d, %d, %d, %d)", cx, cy, start_rect.x, start_rect.y,
             start_rect.w, start_rect.h);

    const input_t press = {.pressed = true, .x = cx, .y = cy};
    sand_frame(0, &press); /* primes next_hover_root - see the comment above */

    const input_t hold = {.x = cx, .y = cy};
    sand_frame(16, &hold); /* hover_root now Sand Title; hover granted this frame */

    const input_t release = {.released = true, .x = cx, .y = cy};
    sand_frame(16, &release); /* DOWN then UP, same draw_menu() call - the click */

    const input_t idle = {0};
    sand_frame(16, &idle); /* pending_start applies here, before any UI build */

    const bool ok = sand_colour_indexed_active(&colour_state) && ui.screen == SAND_UI_RUNNING;
    ESP_LOGI(TAG, "START tap test: indexed_active=%d screen=%d -> %s", sand_colour_indexed_active(&colour_state),
             ui.screen, ok ? "PASS" : "FAIL");

    /* This test can return with indexed mode still engaged, so close it like
     * a real player would before restoring the prior menu selection. */
    sand_exit();
    color_mode = (sand_color_mode_t)previous_mode;
    return ok;
}
#endif /* CONFIG_LAUNCHER_SELFTEST */

#if CONFIG_LAUNCHER_DEVELOPMENT
static void
sand_diagnostic_json(char* out, size_t len) {
    snprintf(out, len, "{\"tilt_x\":%d,\"tilt_y\":%d}", tilt_x(&tilt), tilt_y(&tilt));
}
#endif /* CONFIG_LAUNCHER_DEVELOPMENT */

/* "sand counts": the grid's per-material tally as text, without a
 * screenshot's base64 round trip. */
static bool
sand_console_line(const char* args) {
    if (strcmp(args, "counts") != 0) {
        return false;
    }
    int counts[MATERIAL_MAX];
    sand_material_counts(&sim, counts);
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (counts[m] == 0) {
            continue;
        }
        printf("SAND %s=%d\n", material_by_id((material_id_t)m)->name, counts[m]);
    }
    printf("SAND_END\n");
    fflush(stdout);
    return true;
}

APP_CONSOLE("sand", sand_console_line);

/* gfx_request_full_redraw()'s app half (app.h): the row-run spans and
 * dirty-column tracker gfx cannot see, plus the overlay's own UI canvas
 * when a palette or brush screen is what is actually showing. */
static void
sand_invalidate(void) {
    mark_sand_fully_dirty();
    if (ui.screen == SAND_UI_PALETTE || ui.screen == SAND_UI_BRUSH) {
        ui_invalidate();
    }
}

app_t app_sand = {
    .name = "Falling Sand",
    .summary = "Tilt to steer, touch to pour",
    .enter = sand_enter,
    .frame = sand_frame,
    .update = sand_update,
    .exit = sand_exit,
    .invalidate = sand_invalidate,
#if CONFIG_LAUNCHER_DEVELOPMENT
    .diagnostic_json = sand_diagnostic_json,
#endif
    .console = APP_CONSOLE_PTR(sand_console_line),
};

APP_REGISTER(app_sand);
