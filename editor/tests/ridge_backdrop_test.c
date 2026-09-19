/*
 * The launcher over its ridge backdrop, through the firmware's own ui.c and
 * gfx.c: what an idle home screen costs, what a touch wakes, and what is
 * left when the line comes back to rest.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor/runtime.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "ui/ui_transform.h"

#define FRAME_MS       16
#define FRAMES_TO_REST 600
#define PIXELS         ((size_t)GFX_WIDTH * GFX_HEIGHT)

/* Below the app rows and beside the ridge's left slope, in portrait. */
#define TOUCH_X        60
#define TOUCH_Y        400

static int failures;

static void
expect(bool ok, const char* what) {
    if (!ok) {
        failures++;
        fprintf(stderr, "FAILED: %s\n", what);
    }
}

static bool
frame(bool touching, bool pressed) {
    const input_t input = {
        .down = touching,
        .pressed = pressed,
        .x = TOUCH_X,
        .y = TOUCH_Y,
        .press_x = TOUCH_X,
        .press_y = TOUCH_Y,
    };
    ui_launcher_frame(&input, FRAME_MS);
    const bool sends = gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    gfx_present();
    return sends;
}

static gfx_color_t*
snapshot(void) {
    gfx_color_t* copy = malloc(PIXELS * sizeof *copy);
    if (copy != NULL) {
        memcpy(copy, gfx_framebuffer(), PIXELS * sizeof *copy);
    }
    return copy;
}

static size_t
lit_pixels(const gfx_color_t* pixels) {
    size_t lit = 0;
    for (size_t i = 0; i < PIXELS; i++) {
        lit += pixels[i] != 0;
    }
    return lit;
}

int
main(void) {
    if (!editor_runtime_init()) {
        return 1;
    }
    ui_set_transform(ui_transform_identity());
    frame(false, false);
    frame(false, false);

    gfx_color_t* settled = snapshot();
    if (settled == NULL) {
        return 1;
    }
    expect(settled[0] == 0, "the backdrop is black, so an AMOLED pixel is off");
    expect(lit_pixels(settled) > 8000, "the ridge and the app rows are drawn");
    expect(!frame(false, false), "an idle launcher sends nothing");

    expect(frame(true, true), "a touch sets the ridge moving on the frame it lands");
    frame(true, false);
    frame(false, false);
    int moving_frames = 0;
    for (int i = 0; i < 20; i++) {
        moving_frames += frame(false, false);
    }
    expect(moving_frames == 20, "the wave keeps sending while it travels");
    expect(memcmp(settled, gfx_framebuffer(), PIXELS * sizeof *settled) != 0, "the line is displaced");

    int frames_until_quiet = 0;
    while (frames_until_quiet < FRAMES_TO_REST && frame(false, false)) {
        frames_until_quiet++;
    }
    expect(frames_until_quiet < FRAMES_TO_REST, "the line comes to rest");
    expect(!frame(false, false) && !frame(false, false), "and then sends nothing again");
    expect(memcmp(settled, gfx_framebuffer(), PIXELS * sizeof *settled) == 0,
           "at rest the screen is the settled one exactly: no trail, and the app rows intact");

    free(settled);
    return failures;
}
