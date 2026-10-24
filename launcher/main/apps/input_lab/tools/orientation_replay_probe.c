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

typedef struct {
    gravity_t gravity;
    display_t display;
    int64_t previous_us, next_us;
    int sample_ms;
    bool first, fused, trace;
} replay_t;

static void
print_script(void) {
    char prompt[ORIENTATION_PROBE_PROMPT_SIZE];
    for (orientation_probe_step_t step = ORIENTATION_PROBE_READ_0; step <= ORIENTATION_PROBE_DONE; ++step) {
        orientation_probe_prompt(step, prompt, sizeof prompt);
        printf("%d %d %s\n", (int)step, orientation_probe_target(step), prompt);
    }
}

static void
print_candidates(void) {
    vec3f_t down;
    while (scanf("%f %f %f", &down.x, &down.y, &down.z) == REPLAY_VECTOR_FIELDS) {
        for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
            printf("%d %d%c", display_candidate(down, q, false), display_candidate(down, q, true),
                   q == DISPLAY_QUARTER_COUNT - 1 ? '\n' : ' ');
        }
    }
}

static void
emit_sample(const replay_t* replay, int64_t now_us, bool changed) {
    const display_t* d = &replay->display;
    if (replay->trace) {
        printf("G %" PRId64 " %d %d %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %.6f %.6f %.6f %d %d\n",
               now_us, d->quarter, d->predicted, d->predicted_us, d->flat_ended_us, d->accel_ended_us,
               d->swing_ended_us, d->touch_ended_us, d->filtered.x, d->filtered.y, d->filtered.z, d->flat, d->overhead);
    }
    if (changed) {
        printf(replay->trace ? "C %" PRId64 " %d\n" : "%" PRId64 " %d\n", now_us, display_quarter(d));
    }
}

static bool
replay_sample(replay_t* replay, const input_motion_t* motion, int64_t now_us) {
    if (now_us < 0 || (!replay->first && now_us <= replay->previous_us)) {
        return false;
    }
    const int64_t elapsed_us = replay->first ? 0 : now_us - replay->previous_us;
    const uint32_t dt_us = elapsed_us > GRAVITY_MAX_DT_US ? GRAVITY_MAX_DT_US : (uint32_t)elapsed_us;
    gravity_update(&replay->gravity, motion, dt_us);
    replay->previous_us = now_us;
    replay->first = false;
    if (now_us < replay->next_us) {
        return true;
    }
    replay->next_us = now_us + ((int64_t)replay->sample_ms * MATH_US_PER_MS);
    /* Unit fused input removes measured magnitude; judge filtering still changes vector length. */
    const vec3f_t acceleration = replay->fused
                                     ? gravity_down(&replay->gravity)
                                     : vec3f_scale(vec3f_from_vec3i(motion->acceleration), 1.0F / INPUT_ACCEL_ONE_G);
    const bool changed = display_update(&replay->display, acceleration, now_us);
    emit_sample(replay, now_us, changed);
    return true;
}

static int
replay_samples(bool fused, int sample_ms, int quarter, bool trace) {
    replay_t replay = {.first = true, .fused = fused, .trace = trace, .sample_ms = sample_ms};
    gravity_reset(&replay.gravity);
    display_init(&replay.display);
    replay.display.quarter = quarter;
    int64_t now_us;
    input_motion_t motion = {0};
    int fields;
    while ((fields = scanf("%" SCNd64 " %d %d %d %d %d %d", &now_us, &motion.acceleration.x, &motion.acceleration.y,
                           &motion.acceleration.z, &motion.rotation.x, &motion.rotation.y, &motion.rotation.z))
           == REPLAY_FIELDS) {
        if (!replay_sample(&replay, &motion, now_us)) {
            return EXIT_FAILURE;
        }
    }
    return fields == EOF ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--script") == 0) {
        print_script();
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--candidates") == 0) {
        print_candidates();
        return EXIT_SUCCESS;
    }
    const bool trace = argc == REPLAY_TRACE_ARGUMENTS && strcmp(argv[REPLAY_ARGUMENTS], "--gates") == 0;
    if ((argc != REPLAY_ARGUMENTS && !trace) || (strcmp(argv[1], "accel") != 0 && strcmp(argv[1], "fused") != 0)) {
        return EXIT_FAILURE;
    }
    const int sample_ms = atoi(argv[2]);
    const int quarter = atoi(argv[3]);
    if (sample_ms <= 0 || quarter < 0 || quarter >= DISPLAY_QUARTER_COUNT) {
        return EXIT_FAILURE;
    }
    return replay_samples(strcmp(argv[1], "fused") == 0, sample_ms, quarter, trace);
}
