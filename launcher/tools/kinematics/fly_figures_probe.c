/* fly_figures_probe: one scripted flight through fly_motion_update() for the fly reference. */
#include <stdio.h>

#include "kinematics/fly/fly_motion.h"
#include "math/text/math_text.h"

static const float step_s = 0.05F;
static const int phase_steps = 20;
static const float turn_drag_px = 20.0F;  /* a step's drag: 400 px over a phase, about a quarter turn */
static const float wheel_notches = 0.25F; /* a step's notches: 5 over a phase */
static const vec3f_t frame_axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

typedef struct {
    const char* name;
    fly_motion_steer_t steer;
} phase_t;

static const phase_t phases[] = {
    {"W: forward", {.move = {0, 0, 1}}},
    {"W + drag right: turn", {.move = {0, 0, 1}, .look = {turn_drag_px, 0}}},
    {"W + E: climb", {.move = {0, 1, 1}}},
    {"wheel up: faster", {.wheel = wheel_notches}},
    {"W + drag down: dive", {.move = {0, 0, 1}, .look = {0, turn_drag_px}}},
};

int
main(void) {
    fly_motion_t fly = fly_motion_start(transformf_looking(vec3f_zero(), vec3f_forward(), vec3f_up()),
                                        FLY_MOTION_START_SPEED, FLY_MOTION_TUNING_DEFAULT);
    printf("{\"step\":%.9g,\"pitch_limit\":%.9g,\"phases\":[", step_s, (double)fly.tuning.pitch_limit);
    for (size_t p = 0; p < sizeof phases / sizeof phases[0]; p++) {
        printf("%s\"%s\"", p ? "," : "", phases[p].name);
    }
    printf("],\"samples\":[");
    int sample = 0;
    for (size_t p = 0; p < sizeof phases / sizeof phases[0]; p++) {
        for (int i = 0; i < phase_steps; i++) {
            (void)fly_motion_update(&fly, &phases[p].steer, step_s);
            printf("%s{\"time\":%.9g,\"phase\":%d,\"speed\":%.9g,\"position\":[%s],\"axes\":[", sample ? "," : "",
                   (double)((float)(sample + 1) * step_s), (int)p, (double)fly.speed,
                   vec3f_text(fly.pose.position).chars);
            sample++;
            for (size_t j = 0; j < sizeof frame_axes / sizeof frame_axes[0]; j++) {
                printf("%s[%s]", j ? "," : "", vec3f_text(quatf_rotate(fly.pose.rotation, frame_axes[j])).chars);
            }
            printf("]}");
        }
    }
    puts("]}");
    return 0;
}
