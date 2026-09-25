/*
 * launcher_home_render_host - the home screen, drawn on a host through the
 * real ui layer: ui_begin(), microui, ui_end() and gfx.c, unmodified.
 *
 * A render_host.h scene, and the one that exercises the general path
 * rather than a bespoke one: several frames, a synthetic touch declared
 * frame by frame, and a picture taken of the last of them.
 *
 * Two frames at a minimum. microui clips a window to the rect it had on the
 * previous frame, and a touchscreen never produces the "point, then click"
 * sequence microui expects, so ui_pointer.c synthesizes hover frames before
 * a press can land - a settled screen is never the first frame.
 *
 * The default list is a FIXTURE: three invented entries with no callbacks -
 * nothing drawn here came from a real app. `--row <label>`, repeatable,
 * registers the rows a caller states instead - what a comparison against a
 * real image's home screen needs, since only that image knows what it
 * registered.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
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

static void
register_fixture(void) {
    app_register(&fixture_alpha);
    app_register(&fixture_beta);
    app_register(&fixture_gamma);
}

/* A press at the centre of the panel, held across two frames so
 * ui_pointer.c's synthesized hover has landed by the time the picture is
 * taken, then released. Panel coordinates, as a finger reports them: which
 * row that lands on is whatever the layout puts under the middle. Nothing
 * is pressed before frame 2, so `--frames 2` is the settled screen with no
 * finger on it - the state a capture of an idle device is in. */
static const render_input_step_t touch[] = {
    {2, true, GFX_WIDTH / 2, GFX_HEIGHT / 2},
    {4, false, GFX_WIDTH / 2, GFX_HEIGHT / 2},
};

/* The screen's own init seeds the transform, so the turn is stated after
 * it: stated first, it would be the one that got overwritten. */
static bool
options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
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

/* A render stands for a device held the way it is drawn and already at
 * rest: down is where that quarter's own down points on the panel, the
 * backdrop is level with it, and nothing about it depends on the clock. */
static bool
setup(int quarter) {
    static const int down[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
    ui_launcher_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    ui_ridge_set_gravity(down[quarter & 3][0], down[quarter & 3][1], 256, 0);
    ui_ridge_set_ambient(false);
    ui_ridge_settle();
    return true;
}

static void
draw(const render_frame_t* frame) {
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
