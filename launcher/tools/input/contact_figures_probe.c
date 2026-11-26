/* contact_figures_probe: replay a touch tap and swipe and a BOOT tap and hold. */
#include <stdio.h>
#include <stdlib.h>

#include "gfx/gfx.h"
#include "input/buttons/button_fsm.h"
#include "input/touch/gesture.h"
#include "input/touch/touch.h"
#include "input/touch/touch_fsm.h"
#include "math/scalar/units.h"

#define TOUCH_POLL_US           (MATH_US_PER_S / TOUCH_POLL_HZ)
#define TOUCH_IDLE_MARGIN_POLLS 3
#define TOUCH_IDLE_POLLS        (TOUCH_RELEASE_QUIET_US / TOUCH_POLL_US + TOUCH_IDLE_MARGIN_POLLS)
#define TOUCH_LEAD_POLLS        3
#define TAP_POLLS               7
#define TAP_STEP_PX             3
#define SWIPE_POLLS             12
#define DROPOUT_POLL            2

_Static_assert(TOUCH_POLL_US <= TOUCH_RELEASE_QUIET_US, "The scripted dropout occupies one poll.");

static void
touch_trace(vec2i_t tap) {
    const int swipe_start = TOUCH_LEAD_POLLS + TAP_POLLS + TOUCH_IDLE_POLLS;
    const int end = swipe_start + SWIPE_POLLS + TOUCH_IDLE_POLLS;
    touch_fsm_t fsm;
    touch_fsm_init(&fsm);
    bool swiped = false;
    printf("{\"poll_hz\":%d,\"quiet_us\":%d,\"zone\":%d,\"width\":%d,\"height\":%d,\"rows\":[", TOUCH_POLL_HZ,
           TOUCH_RELEASE_QUIET_US, GESTURE_HOME_ZONE_DEPTH, GFX_WIDTH, GFX_HEIGHT);
    for (int poll = 0; poll < end; ++poll) {
        const int tap_poll = poll - TOUCH_LEAD_POLLS;
        const int swipe_poll = poll - swipe_start;
        const bool tapping = tap_poll >= 0 && tap_poll < TAP_POLLS && tap_poll != DROPOUT_POLL;
        const bool dragging = swipe_poll >= 0 && swipe_poll < SWIPE_POLLS && swipe_poll != DROPOUT_POLL;
        vec2i_t point = vec2i_zero();
        if (tapping) {
            point = (vec2i_t){tap.x + tap_poll * TAP_STEP_PX, tap.y};
        } else if (dragging) {
            const int travel = GESTURE_HOME_SWIPE_DIST + GESTURE_HOME_ZONE_DEPTH / 2;
            point = (vec2i_t){GFX_WIDTH / 3,
                              GFX_HEIGHT - GESTURE_HOME_ZONE_DEPTH / 2 - swipe_poll * travel / (SWIPE_POLLS - 1)};
        }
        const unsigned us = poll * TOUCH_POLL_US;
        touch_fsm_update(&fsm, tapping || dragging, point, us);
        input_t sample = {0};
        touch_fsm_take(&fsm, &sample);
        const bool swipe = !swiped && gesture_is_home_swipe(&sample, GESTURE_EDGE_BOTTOM, GFX_WIDTH, GFX_HEIGHT);
        swiped |= swipe;
        printf("%s[%u,%d,%d,%d,%d,%d,%d,%d]", poll == 0 ? "" : ",", us, tapping || dragging, sample.down,
               sample.point.x, sample.point.y, sample.pressed, sample.released, swipe);
    }
    printf("]}");
}

#define CHATTER_US      (BUTTON_DEBOUNCE_US / 4)
#define BUTTON_LEAD_US  (2 * BUTTON_DEBOUNCE_US)
#define TAP_LENGTH_US   (2 * BUTTON_DEBOUNCE_US + BUTTON_HOLD_US / 8)
#define BETWEEN_TAPS_US (4 * BUTTON_DEBOUNCE_US)
#define HOLD_MARGIN_US  (4 * BUTTON_DEBOUNCE_US)

_Static_assert(TAP_LENGTH_US < BUTTON_HOLD_US, "The scripted tap ends before a hold.");
_Static_assert(CHATTER_US >= MATH_US_PER_MS, "The scripted chatter spans at least one sample.");

static void
button_trace(void) {
    const int tap_up = BUTTON_LEAD_US + TAP_LENGTH_US;
    const int hold_down = tap_up + BETWEEN_TAPS_US;
    const int hold_up = hold_down + BUTTON_HOLD_US + HOLD_MARGIN_US;
    const int edges[] = {BUTTON_LEAD_US, tap_up, hold_down, hold_up};
    const int end = hold_up + HOLD_MARGIN_US;
    button_fsm_t fsm;
    button_fsm_reset(&fsm);
    printf("{\"debounce_us\":%d,\"hold_us\":%d,\"rows\":[", BUTTON_DEBOUNCE_US, BUTTON_HOLD_US);
    bool stable = false;
    for (int us = 0; us <= end; us += MATH_US_PER_MS) {
        bool raw_down = false;
        for (size_t i = 0; i < sizeof edges / sizeof edges[0]; ++i) {
            const int since_edge = us - edges[i];
            raw_down ^= since_edge >= 0 && (since_edge < CHATTER_US || since_edge >= 2 * CHATTER_US);
        }
        button_fsm_update(&fsm, raw_down, us);
        const bool down = button_fsm_is_down(&fsm);
        const int debounce_start = down != stable ? (int)fsm.candidate_us : -1;
        stable = down;
        printf("%s[%d,%d,%d,%d,%d,%d,%d]", us == 0 ? "" : ",", us, raw_down, down, button_fsm_take_pressed(&fsm),
               button_fsm_take_released(&fsm), button_fsm_take_held(&fsm), debounce_start);
    }
    printf("]}");
}

int
main(int argc, char** argv) {
    if (argc != 3) {
        fputs("usage: contact_figures_probe X Y\n", stderr);
        return EXIT_FAILURE;
    }
    char* x_end;
    char* y_end;
    const long x = strtol(argv[1], &x_end, 10);
    const long y = strtol(argv[2], &y_end, 10);
    if (x_end == argv[1] || *x_end || y_end == argv[2] || *y_end || x < 0 || x >= GFX_WIDTH || y < 0
        || y >= GFX_HEIGHT) {
        fputs("tap coordinates must be decimal pixels within the screen\n", stderr);
        return EXIT_FAILURE;
    }
    printf("{\"us_per_ms\":%u,\"touch\":", MATH_US_PER_MS);
    touch_trace((vec2i_t){(int)x, (int)y});
    printf(",\"button\":");
    button_trace();
    puts("}");
    return 0;
}
