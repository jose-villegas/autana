/* Firmware launcher preview smoke checks in both orientations. */
#include "editor/runtime.h"

#include <stdint.h>
#include <stdlib.h>

#define PORTRAIT_WIDTH   editor_runtime_panel_width()
#define PORTRAIT_HEIGHT  editor_runtime_panel_height()
#define LANDSCAPE_WIDTH  PORTRAIT_HEIGHT
#define LANDSCAPE_HEIGHT PORTRAIT_WIDTH

int
main(void) {
    if (!editor_runtime_init()) {
        return 1;
    }
    const size_t count = (size_t)PORTRAIT_WIDTH * PORTRAIT_HEIGHT;
    uint16_t* pixels = calloc(count, sizeof(*pixels));
    if (!pixels) {
        return 1;
    }

    int failures = 0;
    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, pixels, LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT);

    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);

    /* This portrait frame follows a landscape one, and must not be clipped to
     * the landscape canvas. */
    bool below_landscape_height = false;
    for (size_t i = (size_t)LANDSCAPE_HEIGHT * PORTRAIT_WIDTH; i < count; i++) {
        below_landscape_height |= pixels[i] != 0;
    }
    failures += !below_landscape_height;

    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, pixels, 100, 100);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_COUNT, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);

    free(pixels);
    return failures;
}
