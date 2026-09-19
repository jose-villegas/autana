#include "editor/runtime.h"

#include <stdint.h>
#include <stdlib.h>

#define PORTRAIT_WIDTH   368
#define PORTRAIT_HEIGHT  448
#define LANDSCAPE_WIDTH  PORTRAIT_HEIGHT
#define LANDSCAPE_HEIGHT PORTRAIT_WIDTH

static const editor_rect_t portrait_rects[] = {
    {16, 16, 160, 78},   {192, 16, 160, 78}, {16, 106, 336, 58}, {16, 176, 160, 52},
    {192, 176, 160, 52}, {16, 242, 336, 18}, {16, 270, 336, 72}, {16, 354, 336, 72},
};

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

    editor_rect_t rects[sizeof(portrait_rects) / sizeof(portrait_rects[0])];
    for (size_t i = 0; i < sizeof(rects) / sizeof(rects[0]); i++) {
        rects[i] = portrait_rects[i];
    }
    editor_layout_t layout = {PORTRAIT_WIDTH, PORTRAIT_HEIGHT, (int)(sizeof(rects) / sizeof(rects[0])), rects};

    int failures = 0;
    failures += editor_runtime_element_count(EDITOR_SCREEN_CONTROL_CENTER) != layout.rect_count;
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
    rects[2].y += 4;
    failures += !editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, moved, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += !differs(pixels, moved, count);
    rects[2].y -= 4;

    rects[1].width = PORTRAIT_WIDTH;
    failures += editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    rects[1].width = portrait_rects[1].width;

    layout.rect_count--;
    failures += editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    layout.rect_count++;

    failures += editor_runtime_render(EDITOR_SCREEN_CONTROL_CENTER, &layout, pixels, LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, &layout, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, pixels, 100, 100);
    failures += editor_runtime_render(EDITOR_SCREEN_LAUNCHER, NULL, NULL, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    failures += editor_runtime_render(EDITOR_SCREEN_COUNT, NULL, pixels, PORTRAIT_WIDTH, PORTRAIT_HEIGHT);

    free(moved);
    free(pixels);
    return failures;
}
