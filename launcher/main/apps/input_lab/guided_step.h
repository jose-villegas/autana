/* guided_step: tap-armed timing and drawing for a measured pose. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { GUIDED_STEP_WAIT, GUIDED_STEP_ARMED, GUIDED_STEP_NEXT } guided_step_event_t;

typedef struct {
    int step;
    int64_t phase_us;
    bool ready;
} guided_step_t;

void guided_step_begin(guided_step_t* state, int step, int64_t now_us);
guided_step_event_t guided_step_update(guided_step_t* state, bool tap, int64_t now_us, int64_t duration_us);
int64_t guided_step_remaining_us(const guided_step_t* state, int64_t now_us, int64_t duration_us);
void guided_step_draw(const guided_step_t* state, const char* action, const char* target, int current_bearing_deg,
                      int committed_bearing_deg, int target_bearing_deg, int tilt_deg, int target_tilt_deg, int width,
                      int height);
