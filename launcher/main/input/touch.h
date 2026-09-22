/*
 * touch - reads the touch panel on its own schedule.
 *
 * Sampling is deliberately decoupled from rendering. A frame takes ~40 ms
 * (the panel blit alone is 25 ms), and a quick tap can be shorter than that,
 * so polling once per frame drops taps entirely. This runs at TOUCH_POLL_HZ
 * and latches press/release edges, so an event that happens between two frames
 * is still delivered to the next one.
 */
#pragma once

#include <stdint.h>

#include "build_variant.h"
#include "input/input.h"

/* Fast enough that a brief tap is sampled several times, cheap enough to be
 * irrelevant next to rendering (one small I2C read per poll, and only when
 * the controller says it has data). */
#define TOUCH_POLL_HZ 100

/* Starts the polling task, once; later calls do nothing. Safe to call if the
 * panel is missing: reads simply report nothing rather than failing. */
void touch_start(void);

#if CONFIG_LAUNCHER_DEVELOPMENT
typedef enum {
    TOUCH_GESTURE_NONE,
    TOUCH_GESTURE_TAP,
    TOUCH_GESTURE_PRESS,
    TOUCH_GESTURE_DRAG,
} touch_gesture_completion_t;

/* What is injected outranks the controller: a level until an up reaches the
 * polling task, a gesture until its `ms` has elapsed. */
void touch_inject(bool down, int x, int y);
void touch_gesture_start(int x0, int y0, int x1, int y1, uint32_t ms, touch_gesture_completion_t completion);
bool touch_gesture_take_completion(touch_gesture_completion_t* completion);
#endif

/* Copies the accumulated state into `out` and clears the latched edges, so
 * each press and release is reported exactly once. */
void touch_read(input_t* out);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Samples since the last call that carried a point, and how many of those
 * moved from the previous one - the controller's real report rate, which a
 * resting finger or a slow controller holds below TOUCH_POLL_HZ. */
void touch_take_sample_counts(uint32_t* points, uint32_t* moved);
#endif
