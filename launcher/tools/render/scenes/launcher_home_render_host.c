/*
 * launcher_home_render_host, the home screen, drawn on a host through the
 * real ui layer: ui_begin(), microui, ui_end() and gfx.c, unmodified.
 *
 * A render_host.h scene, and the one that exercises the general path
 * rather than a bespoke one: several frames, a synthetic touch declared
 * frame by frame, and a picture taken of the last of them.
 *
 * Two frames at a minimum. microui clips a window to the rect it had on the
 * previous frame, and a touchscreen never produces the "point, then click"
 * sequence microui expects, so ui_pointer.c synthesizes hover frames before
 * a press can land; a settled screen is never the first frame.
 *
 * The default list is a FIXTURE: three invented entries with no callbacks;
 * nothing drawn here came from a real app. `--row <label>`, repeatable,
 * registers the rows a caller states instead; what a comparison against a
 * real image's home screen needs, since only that image knows what it
 * registered.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "ui/ui_ridge.h"
#include "ui/ui_transform.h"

static app_t fixture_alpha = {.name = "Alpha", .summary = "The first fixture row"};
static app_t fixture_beta = {.name = "Beta", .summary = "The second fixture row"};
static app_t fixture_gamma = {.name = "Gamma", .summary = "The third fixture row"};

#define ROWS_MAX 16

static app_t stated[ROWS_MAX];
static int stated_count;
static bool tilt_sweep;
static int scene_quarter;

static void
register_fixture(void) {
    app_register(&fixture_alpha);
    app_register(&fixture_beta);
    app_register(&fixture_gamma);
}

/* A press at the centre of the panel, held on the last frame of the default
 * render so the row shows pressed; the release comes only in longer renders,
 * such as the doc sweep. Panel coordinates, as a finger reports them. Nothing
 * is pressed before frame 2, so `--frames 2` is the settled screen. */
static const render_input_step_t touch[] = {
    {2, true, GFX_WIDTH / 2, GFX_HEIGHT / 2},
    {5, false, GFX_WIDTH / 2, GFX_HEIGHT / 2},
};

/* The screen's own init seeds the transform, so the turn is stated after
 * it: stated first, it would be the one that got overwritten. */
static bool
options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--tilt-sweep") == 0) {
            tilt_sweep = true;
            continue;
        }
        if (strcmp(argv[i], "--row") != 0 || i + 1 >= argc) {
            return false;
        }
        if (stated_count >= ROWS_MAX) {
            fprintf(stderr, "at most %d rows\n", ROWS_MAX);
            return false;
        }
        stated[stated_count].name = argv[++i];
        stated_count++;
    }
    if (stated_count > 0) {
        for (int i = 0; i < stated_count; i++) {
            app_register(&stated[i]);
        }
    } else {
        register_fixture();
    }
    return true;
}

#define TWO_PI 6.283185307179586

static const int DOWN[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};

/* A render stands for a device held the way it is drawn and already at
 * rest: down is where that quarter's own down points on the panel, the
 * backdrop is level with it, and nothing about it depends on the clock. */
static bool
setup(int quarter) {
    ui_launcher_init();
    scene_quarter = quarter;
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    ui_ridge_set_gravity(DOWN[quarter & 3][0], DOWN[quarter & 3][1], 256, 0);
    ui_ridge_set_ambient(false);
    ui_ridge_settle();
    return true;
}

/* --tilt-sweep rocks the board 30 degrees either way over 4 s, the one
 * motion the launcher's backdrop answers on the device. */
static void
sweep_gravity(uint32_t elapsed_ms) {
    const double turn = 30.0 / 360.0 * TWO_PI * sin(TWO_PI * (double)elapsed_ms / 4000.0);
    const int dx = DOWN[scene_quarter & 3][0], dy = DOWN[scene_quarter & 3][1];
    const int gx = (int)lround(1000.0 * (dx * cos(turn) - dy * sin(turn)));
    const int gy = (int)lround(1000.0 * (dx * sin(turn) + dy * cos(turn)));
    ui_ridge_set_gravity(gx, gy, 256, 0);
}

static void
draw(const render_frame_t* frame) {
    if (tilt_sweep) {
        sweep_gravity(frame->elapsed_ms);
    }
    ui_invalidate();
    ui_launcher_frame(&frame->input, frame->dt_ms);
}

const render_scene_t render_scene = {
    .name = "launcher_home",
    .quarter = 1,
    .frames = 5,
    .dt_ms = 16,
    .input = touch,
    .input_count = (int)(sizeof(touch) / sizeof(touch[0])),
    .options = options,
    .setup = setup,
    .draw = draw,
};
