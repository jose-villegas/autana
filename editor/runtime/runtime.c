/*
 * runtime - the firmware's UI and gfx code, compiled for the host and
 * rendered into a caller's buffer. The editor previews through this and
 * nothing else, so a preview is the device's own pixels.
 */

#include "editor/runtime.h"

#include <stddef.h>

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_control_center.h"
#include "ui/ui_launcher.h"
#include "ui/ui_ridge.h"
#include "ui/ui_transform.h"

#define LANDSCAPE_QUARTER 1

/* microui clips a window to the rect it had on the previous frame, so the
 * first frame after the canvas turns is cut to the old shape. The device
 * shows the next frame 16 ms later; a preview is one frame, so it takes the
 * second. */
#define SETTLE_FRAMES     2

static bool initialized;

static const app_t preview_cube = {.name = "3D Cube", .summary = "Real-time 3D rendering"};
static const app_t preview_diagnostics = {.name = "Diagnostics", .summary = "Device status"};
static const app_t preview_sand = {.name = "Falling Sand", .summary = "Particle simulation"};

static const app_t* const preview_apps[] = {
    &preview_cube,
    &preview_diagnostics,
    &preview_sand,
};

const app_t* const*
app_list(void) {
    return preview_apps;
}

int
app_list_count(void) {
    return (int)(sizeof(preview_apps) / sizeof(preview_apps[0]));
}

bool
editor_runtime_init(void) {
    if (initialized) {
        return true;
    }
    if (!gfx_init()) {
        return false;
    }

    ui_launcher_init();
    initialized = true;
    return true;
}

int
editor_runtime_element_count(editor_screen_t screen) {
    return screen == EDITOR_SCREEN_CONTROL_CENTER ? CONTROL_CENTER_ELEMENT_COUNT : 0;
}

static uint16_t
native_rgb565(gfx_color_t color) {
    return (uint16_t)((color >> 8) | (color << 8));
}

static bool
layout_fits(editor_screen_t screen, const editor_layout_t* layout, int width, int height) {
    if (layout->rect_count != editor_runtime_element_count(screen) || layout->rects == NULL
        || layout->canvas_width != width || layout->canvas_height != height) {
        return false;
    }
    for (int i = 0; i < layout->rect_count; i++) {
        const editor_rect_t* rect = &layout->rects[i];
        if (rect->x < 0 || rect->y < 0 || rect->width <= 0 || rect->height <= 0 || rect->width > width - rect->x
            || rect->height > height - rect->y) {
            return false;
        }
    }
    return true;
}

static void
copy_framebuffer(uint16_t* pixels, int width, int height, bool portrait) {
    const gfx_color_t* framebuffer = gfx_framebuffer();
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t source =
                portrait ? (size_t)y * GFX_WIDTH + (size_t)x : (size_t)x * GFX_WIDTH + (size_t)(GFX_WIDTH - 1 - y);
            pixels[(size_t)y * (size_t)width + (size_t)x] = native_rgb565(framebuffer[source]);
        }
    }
}

static void
render_launcher(const input_t* input) {
    ui_invalidate();
    ui_launcher_frame(input, 0);
}

static void
render_control_center(const input_t* input, const editor_layout_t* authored) {
    render_launcher(input);
    ui_control_center_dim_backdrop();
    ui_invalidate();

    if (authored == NULL) {
        ui_control_center_frame(input);
        return;
    }
    control_center_layout_t layout = {
        .canvas_width = (int16_t)authored->canvas_width,
        .canvas_height = (int16_t)authored->canvas_height,
    };
    for (int i = 0; i < CONTROL_CENTER_ELEMENT_COUNT; i++) {
        layout.rects[i] = (control_center_layout_rect_t){
            .x = (int16_t)authored->rects[i].x,
            .y = (int16_t)authored->rects[i].y,
            .width = (int16_t)authored->rects[i].width,
            .height = (int16_t)authored->rects[i].height,
        };
    }
    ui_control_center_frame_layout(input, &layout);
}

bool
editor_runtime_render(editor_screen_t screen, const editor_layout_t* layout, uint16_t* pixels, int width, int height) {
    const bool portrait = width == GFX_WIDTH && height == GFX_HEIGHT;
    const bool landscape = width == GFX_HEIGHT && height == GFX_WIDTH;
    if (pixels == NULL || (!portrait && !landscape) || screen < 0 || screen >= EDITOR_SCREEN_COUNT
        || !editor_runtime_init()) {
        return false;
    }
    if (layout != NULL && !layout_fits(screen, layout, width, height)) {
        return false;
    }

    /* A preview stands for a device held the way it is drawn, already
     * settled: down is the panel's -x in landscape and its +y in portrait. */
    ui_ridge_set_gravity(landscape ? -1 : 0, landscape ? 0 : 1, 256, 0);
    ui_ridge_settle();

    const input_t no_input = {0};
    ui_set_transform(landscape ? ui_transform_quarter_turn(LANDSCAPE_QUARTER, GFX_WIDTH, GFX_HEIGHT)
                               : ui_transform_identity());
    for (int frame = 0; frame < SETTLE_FRAMES; frame++) {
        if (screen == EDITOR_SCREEN_CONTROL_CENTER) {
            render_control_center(&no_input, layout);
        } else {
            render_launcher(&no_input);
        }
    }
    copy_framebuffer(pixels, width, height, portrait);
    return true;
}
