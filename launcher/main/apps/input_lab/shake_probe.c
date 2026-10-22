#include "apps/input_lab/shake_probe.h"

void
shake_probe_start(shake_probe_round_t* round) {
    *round = (shake_probe_round_t){.step = SHAKE_PROBE_STILL, .flash_ms = SHAKE_PROBE_FLASH_MS};
}

bool
shake_probe_advance(shake_probe_round_t* round, uint32_t dt_ms) {
    bool changed = false;
    round->flash_ms = dt_ms < round->flash_ms ? round->flash_ms - dt_ms : 0;
    while (round->step > SHAKE_PROBE_IDLE && round->step < SHAKE_PROBE_DONE) {
        const uint32_t remaining = SHAKE_PROBE_STEP_MS - round->ms;
        if (dt_ms < remaining) {
            round->ms += dt_ms;
            break;
        }
        dt_ms -= remaining;
        round->ms = 0;
        round->step++;
        const uint32_t flash = round->step == SHAKE_PROBE_DONE ? SHAKE_PROBE_DONE_FLASH_MS : SHAKE_PROBE_FLASH_MS;
        round->flash_ms = dt_ms < flash ? flash - dt_ms : 0;
        changed = true;
    }
    return changed;
}

const char*
shake_probe_prompt(shake_probe_step_t step) {
    static const char* const prompts[] = {"TAP TO START",
                                          "LEAVE IT ON THE TABLE",
                                          "HOLD IT AS IN PLAY",
                                          "SHAKE IT GENTLY",
                                          "SHAKE IT HARD",
                                          "WALK WHILE HOLDING IT",
                                          "TURN IN PLACE",
                                          "TILT AND FLIP IT QUICKLY",
                                          "TAP AND DRAG THE SCREEN",
                                          "DONE: LABDUMP TO COLLECT"};
    return step >= SHAKE_PROBE_IDLE && step <= SHAKE_PROBE_DONE ? prompts[step] : "";
}

void
shake_probe_feed(shake_probe_levels_t* levels, vec3f_t linear_g) {
    const float dt_ms = (float)INPUT_MOTION_SAMPLE_US / MATH_US_PER_MS;
    const float smooth = dt_ms / (SHAKE_PROBE_SMOOTH_MS + dt_ms);
    const float slow = dt_ms / (SHAKE_PROBE_SLOW_MS + dt_ms);
    levels->size = sqrtf(vec3f_dot(linear_g, linear_g)) * SHAKE_PROBE_MILLI_G;
    levels->smooth += smooth * (levels->size - levels->smooth);
    levels->average = vec3f_add(levels->average, vec3f_scale(vec3f_sub(linear_g, levels->average), slow));
    const vec3f_t high = vec3f_sub(linear_g, levels->average);
    const float size = sqrtf(vec3f_dot(high, high)) * SHAKE_PROBE_MILLI_G;
    levels->jerky += smooth * (size - levels->jerky);
}
