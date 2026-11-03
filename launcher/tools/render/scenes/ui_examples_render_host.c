/*
 * ui_examples_render_host, one face example per render, drawn by the real
 * code inside ui_screen(): at rest, or in one of the STATES the face gives
 * unchanged code (a finger on it, a disabled scope). Each
 * render also taps the example once and prints when it fired, which the docs'
 * state tables report. ui_examples_render_host.sh lists the examples.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "display/display.h"
#include "gfx/gfx.h"
#include "profile/frame_watch.h"
#include "render_host.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

/* ui_pointer holds a press back until microui's hover catches up. */
#define TAP_HELD_FRAMES 4
/* The image's tap lands once the screen has settled. */
#define IMAGE_TAP_FRAME 2
#define IMAGE_FRAMES    8
/* Frames after a release that still count as firing on it. */
#define RELEASE_FRAMES  2

/* The measuring tap follows the image and is reported within the frame
 * watch's warmup, so the window it judges is the example at rest. */
_Static_assert(IMAGE_FRAMES + TAP_HELD_FRAMES + RELEASE_FRAMES + 1 < FRAME_WATCH_WARMUP,
               "the measuring tap must end before the frame watch judges");

#define X(name) bool ui_example_##name(void);
UI_EXAMPLES
#undef X

static const struct {
    const char* name;
    bool (*draw)(void);
} EXAMPLES[] = {

#define X(name) {#name, ui_example_##name},
    UI_EXAMPLES
#undef X
};

typedef enum { STATE_REST, STATE_PRESSED, STATE_TAPPED, STATE_DISABLED } state_t;

/* What each state does to the unchanged example: rest draws it alone,
 * pressed holds a finger on it, tapped lifts the finger before the image,
 * disabled wraps it in ui_disabled_begin(true). */
static const char* const STATES[] = {"rest", "pressed", "tapped", "disabled"};

static bool (*draw_example)(void);
static state_t state;
static affine2x_t transform;

/* The one tap a render makes: pressed in the image for pressed and tapped,
 * after it otherwise, and where the example returned true along it. */
static int press_frame, release_frame;
static vec2i_t tap_at;
static bool fired_held, fired_released, reported;

int
display_quarter_now(void) {
    return DISPLAY_PORTRAIT;
}

/* --example NAME and --state STATE, each required and known. */
static bool
options(int argc, char** argv) {
    bool state_known = false;
    for (int i = 0; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--example") == 0) {
            for (size_t e = 0; e < sizeof EXAMPLES / sizeof EXAMPLES[0]; e++) {
                if (strcmp(argv[i + 1], EXAMPLES[e].name) == 0) {
                    draw_example = EXAMPLES[e].draw;
                }
            }
        } else if (strcmp(argv[i], "--state") == 0) {
            for (size_t s = 0; s < sizeof STATES / sizeof STATES[0]; s++) {
                if (strcmp(argv[i + 1], STATES[s]) == 0) {
                    state = (state_t)s;
                    state_known = true;
                }
            }
        } else {
            return false;
        }
    }
    return argc % 2 == 0 && draw_example != NULL && state_known;
}

static bool
setup(int quarter) {
    ui_init();
    transform = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
    ui_set_transform(transform);
    press_frame = state == STATE_PRESSED || state == STATE_TAPPED ? IMAGE_TAP_FRAME : IMAGE_FRAMES;
    release_frame = state == STATE_TAPPED ? press_frame + TAP_HELD_FRAMES : IMAGE_FRAMES + TAP_HELD_FRAMES;
    return true;
}

/* The finger for `frame`, at the centre of what the frame before the press
 * drew, in panel coordinates. */
static input_t
finger(const provider_frame_t* frame) {
    input_t in = {0};
    if (frame->index == press_frame) {
        tap_at = affine2x_map_pixel(
            &transform, (vec2i_t){frame->drawn.x + frame->drawn.w / 2, frame->drawn.y + frame->drawn.h / 2});
    }
    if (frame->index < press_frame || frame->index > release_frame) {
        return in;
    }
    in.point = in.press = (vec2i_t){tap_at.x, tap_at.y};
    in.down = frame->index < release_frame;
    in.pressed = frame->index == press_frame;
    in.released = frame->index == release_frame;
    return in;
}

static void
draw(const provider_frame_t* frame) {
    const input_t in = finger(frame);
    ui_begin(&in);
    ui_screen();
    ui_disabled_begin(state == STATE_DISABLED);
    const bool fired = draw_example();
    ui_disabled_end();
    ui_end(UI_BACKGROUND);

    if (frame->index >= press_frame && frame->index < release_frame) {
        fired_held |= fired;
    } else if (frame->index >= release_frame && frame->index <= release_frame + RELEASE_FRAMES) {
        fired_released |= fired;
    } else if (frame->index > release_frame + RELEASE_FRAMES && !reported) {
        reported = true;
        fprintf(stderr, "fires %s\n", fired_held ? "while held" : fired_released ? "on release" : "no");
    }
}

const frame_provider_t ui_examples_provider = {
    .name = "ui_examples",
    .quarter = 0,
    .frames = IMAGE_FRAMES,
    .crop_margin = UI_MARGIN,
    .dt_ms = 33,
    .options = options,
    .setup = setup,
    .draw = draw,
};
