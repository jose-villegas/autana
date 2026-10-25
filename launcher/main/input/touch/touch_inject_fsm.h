/* touch_inject_fsm: a scripted contact sampled by the touch poller, and the
 * gesture it finishes as. */
#pragma once

#include "math/linear/vec2i.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    vec2i_t from, to;
    uint32_t ms;
    int64_t started_us;
    bool active;
    bool started;
    bool endpoint_emitted;
} touch_inject_t;

void touch_inject_init(touch_inject_t* inject, vec2i_t from, vec2i_t to, uint32_t ms);

bool touch_inject_step(touch_inject_t* inject, int64_t now_us, vec2i_t* point);

/* What an injected contact finishes as, reported back to the console. */
typedef enum {
    TOUCH_INJECT_NONE,
    TOUCH_INJECT_TAP,
    TOUCH_INJECT_PRESS,
    TOUCH_INJECT_DRAG,
} touch_inject_completion_t;

/* "TAP", "PRESS" or "DRAG"; NULL for none. */
const char* touch_inject_completion_name(touch_inject_completion_t completion);
