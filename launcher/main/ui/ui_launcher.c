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

#include "ui/ui.h"
#include "ui/ui_ridge.h"

int
ui_launcher_frame(const input_t* input, uint32_t dt_ms) {
    mu_Context* ctx = ui_context();

    ui_begin(input);
    const int chosen = ui_launcher_draw(ctx, dt_ms);
    ui_ridge_step(input, dt_ms);

    /* Repaints only what looks different from what is already on screen, so
     * a home screen nobody is touching, its ridge at rest, costs no bus time
     * at all. */
    ui_end_over(ui_ridge_paint);

    return chosen;
}
