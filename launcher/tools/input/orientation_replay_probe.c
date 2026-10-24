/* orientation_replay_probe: replay timestamped screen-axis counts through real judges. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/input_lab/orientation_probe.h"
#include "display/display.h"
#include "input/motion/gravity.h"
#include "math/linear/vec_convert.h"

#define REPLAY_ARGUMENTS 4
#define REPLAY_FIELDS    7

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
    if (argc != REPLAY_ARGUMENTS || (strcmp(argv[1], "accel") != 0 && strcmp(argv[1], "fused") != 0)) {
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
        if (display_update(&display, acceleration, now_us)) {
            printf("%" PRId64 " %d\n", now_us, display_quarter(&display));
        }
    }
    return fields == EOF ? EXIT_SUCCESS : EXIT_FAILURE;
}
