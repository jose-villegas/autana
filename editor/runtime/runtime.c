/*
 * runtime - the firmware's UI and gfx code, compiled for the host and
 * rendered into a caller's buffer. The editor previews through this and
 * nothing else, so a preview is the device's own pixels.
 */

#include "editor/runtime.h"

#include <stddef.h>

#include "app/app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "ui/ui_ridge.h"
#include "ui/ui_transform.h"

#define LANDSCAPE_QUARTER 1

static bool initialized;

static app_t preview_render_lab = {.name = "Render Lab", .summary = "Software rendering experiments"};
static app_t preview_diagnostics = {.name = "Diagnostics", .summary = "Device status"};
static app_t preview_sand = {.name = "Falling Sand", .summary = "Particle simulation"};

bool
editor_runtime_init(void) {
    if (initialized) {
        return true;
    }
    if (!gfx_init()) {
        return false;
    }

    app_register(&preview_render_lab);
    app_register(&preview_diagnostics);
    app_register(&preview_sand);

    ui_launcher_init();
    initialized = true;
    return true;
}

int
editor_runtime_panel_width(void) {
    return GFX_WIDTH;
}

int
editor_runtime_panel_height(void) {
    return GFX_HEIGHT;
}

int
editor_runtime_tap_min(void) {
    return UI_TAP_MIN;
}

static void
copy_framebuffer(uint16_t* pixels, int width, int height, bool portrait) {
    const gfx_color_t* framebuffer = gfx_framebuffer();
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t source =
                portrait ? (size_t)y * GFX_WIDTH + (size_t)x : (size_t)x * GFX_WIDTH + (size_t)(GFX_WIDTH - 1 - y);
            pixels[(size_t)y * (size_t)width + (size_t)x] = gfx_color_swap(framebuffer[source]);
        }
    }
}

static void
render_launcher(const input_t* input) {
    ui_invalidate();
    ui_launcher_frame(input, 0);
}

bool
editor_runtime_render(editor_screen_t screen, uint16_t* pixels, int width, int height) {
    const bool portrait = width == GFX_WIDTH && height == GFX_HEIGHT;
    const bool landscape = width == GFX_HEIGHT && height == GFX_WIDTH;
    if (pixels == NULL || (!portrait && !landscape) || screen < 0 || screen >= EDITOR_SCREEN_COUNT
        || !editor_runtime_init()) {
        return false;
    }

    /* A preview stands for a device held the way it is drawn, already
     * settled: down is the panel's -x in landscape and its +y in portrait. */
    ui_ridge_set_gravity(landscape ? -1 : 0, landscape ? 0 : 1, 256, 0);
    ui_ridge_set_ambient(false);
    ui_ridge_settle();

    const input_t no_input = {0};
    ui_set_transform(landscape ? ui_transform_quarter_turn(LANDSCAPE_QUARTER, GFX_WIDTH, GFX_HEIGHT)
                               : ui_transform_identity());
    render_launcher(&no_input);
    copy_framebuffer(pixels, width, height, portrait);
    return true;
}
