/* Firmware launcher preview smoke checks in both orientations. */
#include "editor/runtime.h"

#include "ui/ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    uint16_t* portrait = calloc(count, sizeof(*portrait));
    if (!pixels || !portrait) {
        free(pixels);
        free(portrait);
        return 1;
    }

    int failures = 0;
    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, portrait, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, pixels, LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT);
    bool lit = false;
    bool differs = false;
    for (int y = 0; y < LANDSCAPE_HEIGHT; y++) {
        for (int x = 0; x < LANDSCAPE_WIDTH; x++) {
            const size_t target = (size_t)y * LANDSCAPE_WIDTH + x;
            const size_t source = (size_t)x * PORTRAIT_WIDTH + (PORTRAIT_WIDTH - 1 - y);
            lit |= pixels[target] != 0;
            differs |= pixels[target] != portrait[source];
        }
    }
    if (!lit || !differs || ui_width() != LANDSCAPE_WIDTH || ui_height() != LANDSCAPE_HEIGHT) {
        fprintf(stderr, "landscape must be lit and differ from portrait read as landscape\n");
        failures++;
    }
    failures += !editor_runtime_render(EDITOR_SCREEN_LAUNCHER, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    if (memcmp(portrait, pixels, count * sizeof(*pixels)) != 0) {
        fprintf(stderr, "portrait after landscape must equal fresh portrait\n");
        failures++;
    }

    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, pixels, 100, 100);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_COUNT, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);

    free(portrait);
    free(pixels);
    return failures;
}
