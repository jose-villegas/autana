/* Firmware preview smoke checks with authored geometry overrides. */
#include "editor/runtime.h"

#include <stdint.h>
#include <stdlib.h>

#define PORTRAIT_WIDTH   editor_runtime_panel_width()
#define PORTRAIT_HEIGHT  editor_runtime_panel_height()
#define LANDSCAPE_WIDTH  PORTRAIT_HEIGHT
#define LANDSCAPE_HEIGHT PORTRAIT_WIDTH

static bool
differs(const uint16_t* first, const uint16_t* second, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (first[i] != second[i]) {
            return true;
        }
    }
    return false;
}

int
main(void) {
    if (!editor_runtime_init()) {
        return 1;
    }
    const size_t count = (size_t)PORTRAIT_WIDTH * PORTRAIT_HEIGHT;
    uint16_t* pixels = calloc(count, sizeof(*pixels));
    uint16_t* moved = calloc(count, sizeof(*moved));
    if (!pixels || !moved) {
        return 1;
    }

    control_center_layout_t layout = control_center_layout_portrait;

    int failures = 0;
    failures += editor_runtime_element_count(EDITOR_SCREEN_CONTROL_CENTER) != CONTROL_CENTER_ELEMENT_COUNT;
    failures += editor_runtime_element_count(EDITOR_SCREEN_LAUNCHER) != 0;
    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, pixels, LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT);
    failures += !editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, NULL, pixels, LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT);
    failures += !editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);

    /* This portrait frame follows a landscape one, and must not be clipped to
     * the landscape canvas: the last element reaches below row 368. */
    bool below_landscape_height = false;
    for (size_t i = (size_t)LANDSCAPE_HEIGHT * PORTRAIT_WIDTH; i < count; i++) {
        below_landscape_height |= pixels[i] != 0;
    }
    failures += !below_landscape_height;

    /* An authored rect has to reach the pixels, not just be accepted. */
    layout.rects[2].y += 4;
    failures += !editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, moved, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += !differs(pixels, moved, count);
    layout.rects[2].y -= 4;

    layout.rects[1].width = PORTRAIT_WIDTH;
    failures += editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    layout.rects[1].width = control_center_layout_portrait.rects[1].width;

    failures += editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, pixels, LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, &layout, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, pixels, 100, 100);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, NULL, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_COUNT, NULL, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);

    free(moved);
    free(pixels);
    return failures;
}
