/*
 * ui_launcher - the home screen's frame, ui_begin()/ui_end() included.
 *
 * The row layout itself lives in ui_launcher_draw.c, split out the same way
 * every app's own screen splits drawing from ui_begin()/ui_end() (see
 * docs/Building-a-Screen.md) - not for reuse, but because ui_end() needs the
 * real framebuffer and ui_launcher_draw() alone does not, which is what
 * lets a host suite drive the real row layout without linking gfx.c.
 */

#include "ui/ui_launcher.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_ridge.h"
#include "util/frame_cost.h"

#define LAUNCHER_HEAL_ROWS 32

const app_t*
ui_launcher_frame(const input_t* input, uint32_t dt_ms) {
    mu_Context* ctx = ui_context();

    gfx_heal_set_budget(GFX_WIDTH * LAUNCHER_HEAL_ROWS);
    gfx_heal_set_rolling(LAUNCHER_HEAL_ROWS);

    FRAME_COST_BEGIN(built_from);
    ui_begin(input);
    const app_t* chosen = ui_launcher_draw(ctx, dt_ms);
    FRAME_COST_END(built_from, "ui.build");
    const input_t idle = {0};
    ui_ridge_step(input != NULL ? input : &idle, dt_ms);

    /* Repaints only what looks different from what is already on screen, so
     * a home screen nobody is touching, its ridge at rest, costs no bus time
     * at all. */
    FRAME_COST_BEGIN(painted_from);
    ui_end_over(ui_ridge_paint);
    FRAME_COST_END(painted_from, "ui.paint");

    return chosen;
}
