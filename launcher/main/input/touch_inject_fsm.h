/* touch_inject_fsm - a scripted contact sampled by the touch poller. */
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
