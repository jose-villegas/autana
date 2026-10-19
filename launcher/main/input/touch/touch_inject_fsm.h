/* touch_inject_fsm: a scripted contact sampled by the touch poller, and the
 * gesture it finishes as. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x0, y0;
    int x1, y1;
    uint32_t ms;
    int64_t started_us;
    bool active;
    bool started;
    bool endpoint_emitted;
} touch_inject_t;

void touch_inject_init(touch_inject_t* inject, int x0, int y0, int x1, int y1, uint32_t ms);

bool touch_inject_step(touch_inject_t* inject, int64_t now_us, int* x, int* y);

/* What an injected contact finishes as, reported back to the console. */
typedef enum {
    TOUCH_INJECT_NONE,
    TOUCH_INJECT_TAP,
    TOUCH_INJECT_PRESS,
    TOUCH_INJECT_DRAG,
} touch_inject_completion_t;

/* "TAP", "PRESS" or "DRAG"; NULL for none. */
const char* touch_inject_completion_name(touch_inject_completion_t completion);
