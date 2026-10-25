#include "apps/input_lab/orientation_probe.h"

#include <stdio.h>

#include "apps/input_lab/motion_probe.h"
#include "display/display_judge.h"
#include "math/scalar/mathi.h"

const char*
orientation_probe_action(orientation_probe_step_t step) {
    static const char* const actions[] = {"PICK UP", "WALK", "SHAKE", "OVERHEAD"};
    if (step >= ORIENTATION_PROBE_PICKUP && step <= ORIENTATION_PROBE_OVERHEAD) {
        return actions[step - ORIENTATION_PROBE_PICKUP];
    }
    if (step >= ORIENTATION_PROBE_READ_0 && step <= ORIENTATION_PROBE_READ_4) {
        return step == ORIENTATION_PROBE_READ_0 ? "HOLD" : "TURN TO";
    }
    return step == ORIENTATION_PROBE_FLAT ? "PUT DOWN FLAT" : "";
}

int
orientation_probe_bearing(int quarter) {
    return mathi_wrap((quarter + 1) * (int)DISPLAY_QUARTER_DEG, 0, MOTION_PROBE_FULL_TURN_DEG);
}

int
orientation_probe_target(orientation_probe_step_t step) {
    if (step >= ORIENTATION_PROBE_READ_0 && step <= ORIENTATION_PROBE_READ_4) {
        return (DISPLAY_DEFAULT_QUARTER + (int)(step - ORIENTATION_PROBE_READ_0)) % DISPLAY_QUARTER_COUNT;
    }
    return step >= ORIENTATION_PROBE_PICKUP && step <= ORIENTATION_PROBE_OVERHEAD ? DISPLAY_DEFAULT_QUARTER
                                                                                  : DISPLAY_QUARTER_NONE;
}

void
orientation_probe_prompt(orientation_probe_step_t step, char* text, size_t size) {
    const int target = orientation_probe_target(step);
    if (step >= ORIENTATION_PROBE_READ_0 && step <= ORIENTATION_PROBE_READ_4) {
        (void)snprintf(text, size,
                       step == ORIENTATION_PROBE_READ_0 ? "HOLD %s AT %d DEG" : "TURN TO %s, HOLD AT %d DEG",
                       display_quarter_name(target), ORIENTATION_PROBE_READING_TILT_DEG);
    } else if (step >= ORIENTATION_PROBE_PICKUP && step <= ORIENTATION_PROBE_OVERHEAD) {
        (void)snprintf(text, size, "%s, KEEP %s", orientation_probe_action(step), display_quarter_name(target));
    } else {
        (void)snprintf(text, size, "%s",
                       step == ORIENTATION_PROBE_FLAT   ? "PUT DOWN FLAT, FACE UP"
                       : step == ORIENTATION_PROBE_DONE ? "DONE: LABDUMP TO COLLECT"
                                                        : "");
    }
}
