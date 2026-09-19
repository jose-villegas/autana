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
 * The list is a FIXTURE: three invented entries with no callbacks, because
 * the shell is what owns the registry and this is not the shell. Nothing
 * drawn here came from a registered app.
 */

#include <stddef.h>

#include "app.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "ui/ui_transform.h"

static const app_t fixture_alpha = {.name = "Alpha", .summary = "The first fixture row"};
static const app_t fixture_beta = {.name = "Beta", .summary = "The second fixture row"};
static const app_t fixture_gamma = {.name = "Gamma", .summary = "The third fixture row"};

static const app_t* const fixture[] = {&fixture_alpha, &fixture_beta, &fixture_gamma};

const app_t* const*
app_list(void) {
    return fixture;
}

int
app_list_count(void) {
    return (int)(sizeof(fixture) / sizeof(fixture[0]));
}

void
app_register(const app_t* app) {
    (void)app;
}

/* A press at the centre of the panel, held across two frames so
 * ui_pointer.c's synthesized hover has landed by the time the picture is
 * taken, then released. Panel coordinates, as a finger reports them: which
 * row that lands on is whatever the layout puts under the middle. */
static const render_input_step_t touch[] = {
    {1, true, GFX_WIDTH / 2, GFX_HEIGHT / 2},
    {4, false, GFX_WIDTH / 2, GFX_HEIGHT / 2},
};

/* The screen's own init seeds the transform, so the turn is stated after
 * it: stated first, it would be the one that got overwritten. */
static bool
setup(int quarter) {
    ui_launcher_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
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
    .setup = setup,
    .draw = draw,
};
