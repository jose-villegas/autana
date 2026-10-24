#include "apps/input_lab/orientation_probe.h"

const char*
orientation_probe_prompt(orientation_probe_step_t step) {
    static const char* const prompts[] = {"READ AT 45 DEG: QUARTER 0", "READ AT 45 DEG: QUARTER 1",
                                          "READ AT 45 DEG: QUARTER 2", "READ AT 45 DEG: QUARTER 3",
                                          "PUT DOWN FLAT, FACE UP",    "PICK UP, KEEP QUARTER 3",
                                          "WALK, KEEP QUARTER 3",      "SHAKE, KEEP QUARTER 3",
                                          "OVERHEAD, KEEP QUARTER 3",  "DONE: LABDUMP TO COLLECT"};
    return step >= ORIENTATION_PROBE_READ_0 && step <= ORIENTATION_PROBE_DONE ? prompts[step] : "";
}
