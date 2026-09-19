/*
 * ui_ridge - the launcher's backdrop: Cerro Autana's ridge as a line of
 * light on black, which a touch sets waving.
 *
 * The ridge is the one the boot animation's photograph ends on, in the same
 * frame, so the cut from boot to launcher leaves the outline where the
 * mountain was. That frame is 448 columns wide, the landscape canvas; a
 * portrait canvas shows the middle 368 of them.
 *
 * Black is the point and not a default: an AMOLED pixel at 0 is off, so the
 * line is the only thing lit.
 */

#include "ui/ui_ridge.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "ui/ridge_curve_generated.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_transform.h"
#include "util/spring_line.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#define RIDGE_ALLOC(bytes) heap_caps_malloc((bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define RIDGE_ALLOC(bytes) malloc(bytes)
#endif

#define GLOW_RADIUS_PX   13
#define GLOW_CORE_PX     3
#define GLOW_CORE_RGB    0xFFFFFF
#define GLOW_HALO_RGB    0x38D6E8

/* A tap flicks the line up; a finger drawn along it plucks it again every
 * STRUM_STEP_PX. Velocities are pixels per spring tick. */
#define TOUCH_HALF_WIDTH 24
#define TAP_VELOCITY     (-(SPRING_LINE_ONE * 3 / 2))
#define STRUM_VELOCITY   (-(SPRING_LINE_ONE / 2))
#define STRUM_STEP_PX    12

typedef struct {
    spring_line_t line;
    gfx_glow_style_t style;
    int32_t offset[RIDGE_CURVE_POINTS];
    int32_t velocity[RIDGE_CURVE_POINTS];
    int16_t drawn[RIDGE_CURVE_POINTS];
    int last_pluck_x;
} ridge_t;

static ridge_t* ridge;
static bool allocation_tried;

/* On first use rather than at boot, so the memory is not taken from a boot
 * that never reaches the launcher. Without it the backdrop is plain black. */
static void
allocate_once(void) {
    if (allocation_tried) {
        return;
    }
    allocation_tried = true;
    ridge = RIDGE_ALLOC(sizeof *ridge);
    if (ridge == NULL) {
        return;
    }
    spring_line_init(&ridge->line, ridge->offset, ridge->velocity, RIDGE_CURVE_POINTS);
    gfx_glow_style_set(&ridge->style, GLOW_RADIUS_PX, GLOW_CORE_PX, GLOW_CORE_RGB, GLOW_HALO_RGB);
    memcpy(ridge->drawn, ridge_curve_y, sizeof ridge->drawn);
}

/* The first ridge column the canvas shows: none are cut in landscape. */
static int
first_shown_column(void) {
    const int hidden = RIDGE_CURVE_POINTS - ui_width();
    return hidden > 0 ? hidden / 2 : 0;
}

static void
draw_columns(int lo, int hi, int erase_px) {
    const int first = first_shown_column();
    const int shown = RIDGE_CURVE_POINTS - 2 * first;
    gfx_glow_curve(ridge->drawn + first, shown, lo - first, hi - first,
                   ui_transform_quarter_turns(ui_effective_transform()), erase_px, &ridge->style);
}

void
ui_ridge_paint(void) {
    allocate_once();
    gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x000000));
    if (ridge != NULL) {
        draw_columns(0, RIDGE_CURVE_POINTS, 0);
    }
}

/* From the raw touch, not microui's pointer: ui_pointer holds a press back
 * until it knows a tap from a scroll, and feedback that waits for that is
 * feedback a finger has already stopped expecting. */
static void
pluck_from_touch(const input_t* input) {
    if (!input->down) {
        return;
    }
    int x, y;
    ui_to_logical(input->x, input->y, &x, &y);
    x += first_shown_column();
    if (input->pressed) {
        spring_line_poke(&ridge->line, x, TOUCH_HALF_WIDTH, TAP_VELOCITY);
        ridge->last_pluck_x = x;
    } else if (abs(x - ridge->last_pluck_x) >= STRUM_STEP_PX) {
        spring_line_poke(&ridge->line, x, TOUCH_HALF_WIDTH, STRUM_VELOCITY);
        ridge->last_pluck_x = x;
    }
}

void
ui_ridge_step(const input_t* input, uint32_t dt_ms) {
    allocate_once();
    if (ridge == NULL) {
        return;
    }
    pluck_from_touch(input);
    if (spring_line_advance(&ridge->line, dt_ms) == 0) {
        return;
    }
    int lo, hi;
    const int moved_px = spring_line_apply(&ridge->line, ridge_curve_y, ridge->drawn, &lo, &hi);
    if (hi > lo) {
        draw_columns(lo - GLOW_RADIUS_PX - 1, hi + GLOW_RADIUS_PX + 1, moved_px + 1);
    }
}
