/* orientation_replay_probe: replay timestamped screen-axis counts through real judges. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/input_lab/orientation_probe.h"
#include "display/display_judge.h"
#include "input/motion/gravity.h"
#include "math/linear/vec_convert.h"

#define REPLAY_ARGUMENTS       4
#define REPLAY_FIELDS          7
#define REPLAY_VECTOR_FIELDS   3
#define REPLAY_TRACE_ARGUMENTS (REPLAY_ARGUMENTS + 1)

int
main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--script") == 0) {
        char prompt[ORIENTATION_PROBE_PROMPT_SIZE];
        for (orientation_probe_step_t step = ORIENTATION_PROBE_READ_0; step <= ORIENTATION_PROBE_DONE; ++step) {
            orientation_probe_prompt(step, prompt, sizeof prompt);
            printf("%d %d %s\n", (int)step, orientation_probe_target(step), prompt);
        }
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--candidates") == 0) {
        vec3f_t down;
        while (scanf("%f %f %f", &down.x, &down.y, &down.z) == REPLAY_VECTOR_FIELDS) {
            for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
                printf("%d %d%c", display_candidate(down, q, false), display_candidate(down, q, true),
                       q == DISPLAY_QUARTER_COUNT - 1 ? '\n' : ' ');
            }
        }
        return EXIT_SUCCESS;
    }
    const bool trace = argc == REPLAY_TRACE_ARGUMENTS && strcmp(argv[4], "--gates") == 0;
    if ((argc != REPLAY_ARGUMENTS && !trace) || (strcmp(argv[1], "accel") != 0 && strcmp(argv[1], "fused") != 0)) {
        return EXIT_FAILURE;
    }
    const int sample_ms = atoi(argv[2]);
    const int quarter = atoi(argv[3]);
    if (sample_ms <= 0 || quarter < 0 || quarter >= DISPLAY_QUARTER_COUNT) {
        return EXIT_FAILURE;
    }
    const bool fused = strcmp(argv[1], "fused") == 0;
    gravity_t gravity;
    gravity_reset(&gravity);
    display_t display;
    display_init(&display);
    display.quarter = quarter;
    int64_t now_us, previous_us = 0, next_us = 0;
    bool first = true;
    input_motion_t motion = {0};
    int fields;
    while ((fields = scanf("%" SCNd64 " %d %d %d %d %d %d", &now_us, &motion.acceleration.x, &motion.acceleration.y,
                           &motion.acceleration.z, &motion.rotation.x, &motion.rotation.y, &motion.rotation.z))
           == REPLAY_FIELDS) {
        if (now_us < 0 || (!first && now_us <= previous_us)) {
            return EXIT_FAILURE;
        }
        const int64_t elapsed_us = first ? 0 : now_us - previous_us;
        const uint32_t dt_us = elapsed_us > GRAVITY_MAX_DT_US ? GRAVITY_MAX_DT_US : (uint32_t)elapsed_us;
        gravity_update(&gravity, &motion, dt_us);
        previous_us = now_us;
        first = false;
        if (now_us < next_us) {
            continue;
        }
        next_us = now_us + (int64_t)sample_ms * MATH_US_PER_MS;
        /* Unit fused input removes measured magnitude; judge filtering still changes vector length. */
        const vec3f_t acceleration = fused
                                         ? gravity_down(&gravity)
                                         : vec3f_scale(vec3f_from_vec3i(motion.acceleration), 1.0F / INPUT_ACCEL_ONE_G);
        const bool changed = display_update(&display, acceleration, now_us);
        if (trace) {
            printf("G %" PRId64 " %d %d %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
                   " %.6f %.6f %.6f %d %d\n",
                   now_us, display.quarter, display.predicted, display.predicted_us, display.flat_ended_us,
                   display.accel_ended_us, display.swing_ended_us, display.touch_ended_us, display.filtered.x,
                   display.filtered.y, display.filtered.z, display.flat, display.overhead);
        }
        if (changed) {
            printf(trace ? "C %" PRId64 " %d\n" : "%" PRId64 " %d\n", now_us, display_quarter(&display));
        }
    }
    return fields == EOF ? EXIT_SUCCESS : EXIT_FAILURE;
}
