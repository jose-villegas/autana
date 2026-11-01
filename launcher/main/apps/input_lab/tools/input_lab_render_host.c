/* Deterministic page fixtures use the real app callbacks and drawing. */
#include <string.h>
#include <time.h>
#include "app/app.h"
#include "apps/input_lab/imu_probe.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "input/motion/imu.h"
#include "input/motion/motion_reader.h"
#include "math/scalar/units.h"
#include "render_host.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#define TAP_FRAME 4
#define FRAME_MS  16
#define POSE_FRAMES                                                                                                    \
    ((IMU_REST_SETTLE_MS + FRAME_MS - 1) / FRAME_MS + (IMU_REST_WINDOW_MS + FRAME_MS - 1) / FRAME_MS + 1)
#define TAP_X 180
#define TAP_Y 220
static int page;
static bool tapped, results, orientation;
static int shell_quarter;
static int clock_frame;

int
timespec_get(struct timespec* now, int base) {
    const int64_t elapsed_ms = (int64_t)clock_frame * FRAME_MS;
    now->tv_sec = 1 + (elapsed_ms / 1000);
    now->tv_nsec = (long)(elapsed_ms % 1000) * 1000000L;
    return base;
}

int
display_quarter_now(void) {
    return shell_quarter;
}

bool
imu_ready(void) {
    return true;
}

bool
input_read_motion(input_motion_t* out) {
    *out = (input_motion_t){.acceleration = {INPUT_ACCEL_ONE_G / 4, INPUT_ACCEL_ONE_G / 8, INPUT_ACCEL_ONE_G}};
    if (orientation) {
        out->acceleration = (vec3i_t){-INPUT_ACCEL_ONE_G, 0, -INPUT_ACCEL_ONE_G};
    }
    if (results && !orientation) {
        const int pose_frame = clock_frame - TAP_FRAME;
        const int pose = mathi_clamp(pose_frame / POSE_FRAMES, 0, IMU_PROBE_POSE_COUNT - 1);
        const int turn_frame = (IMU_PROBE_POSE_COUNT * POSE_FRAMES) + ((IMU_REST_SETTLE_MS + FRAME_MS - 1) / FRAME_MS);
        if (pose_frame >= turn_frame && (pose_frame - turn_frame) * FRAME_MS <= IMU_PROBE_TURN_WINDOW_MS) {
            out->rotation.x = IMU_PROBE_TURN_THRESHOLD + 1;
        }
        const vec3i_t direction = imu_probe_pose_direction(pose, DISPLAY_DEFAULT_QUARTER);
        out->acceleration = (vec3i_t){direction.x * INPUT_ACCEL_ONE_G, direction.y * INPUT_ACCEL_ONE_G,
                                      direction.z * INPUT_ACCEL_ONE_G};
    }
    return true;
}

bool
input_read_motion_sequence(input_motion_t* out, uint32_t* sequence) {
    *sequence = (uint32_t)clock_frame;
    return input_read_motion(out);
}

bool
input_read_motion_batch(input_motion_batch_t* out) {
    const int64_t now_us = (int64_t)clock_frame * FRAME_MS * MATH_US_PER_MS;
    static int64_t last_us;
    if (now_us < last_us) {
        last_us = 0;
    }
    const int count = (int)((now_us - last_us) / INPUT_MOTION_SAMPLE_US);
    last_us += (int64_t)count * INPUT_MOTION_SAMPLE_US;
    out->count = mathi_min(count, INPUT_MOTION_SAMPLES);
    out->gap = count > INPUT_MOTION_SAMPLES;
    memset(out->lost_before, 0, sizeof out->lost_before);
    for (int i = 0; i < out->count; i++) {
        input_read_motion(&out->samples[i]);
    }
    return true;
}

vec3i_t
input_accel_offset(void) {
    return (vec3i_t){0};
}

vec3i_t
input_gyro_offset(void) {
    return (vec3i_t){0};
}

bool
input_learn_gyro_offset(vec3i_t residual) {
    (void)residual;
    return false;
}

bool
input_set_accel_offset(vec3i_t offset) {
    (void)offset;
    return true;
}

static bool
options(int argc, char** argv) {
    if (argc != 2 || strcmp(argv[0], "--page") != 0) {
        return false;
    }
    const char* name = argv[1];
    if (strcmp(name, "random") == 0) {
        page = 0;
    } else if (strcmp(name, "random-tap") == 0) {
        page = 0;
        tapped = true;
    } else if (strcmp(name, "grid") == 0) {
        page = 1;
    } else if (strcmp(name, "bezel") == 0) {
        page = 2;
    } else if (strcmp(name, "motion") == 0) {
        page = 3;
    } else if (strcmp(name, "motion-results") == 0) {
        page = 3;
        results = true;
    } else if (strcmp(name, "orientation-ready") == 0 || strcmp(name, "orientation-running") == 0
               || strcmp(name, "orientation-turn-ready") == 0) {
        page = 5;
        orientation = true;
        results = strcmp(name, "orientation-ready") != 0;
    } else {
        return false;
    }
    return true;
}

static bool
setup(int quarter) {
    shell_quarter = orientation ? DISPLAY_DEFAULT_QUARTER : quarter;
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    app_list()->enter();
    return true;
}

static const render_input_step_t touch[] = {
    {TAP_FRAME, true, {TAP_X, TAP_Y}},
    {TAP_FRAME + 1, false, {TAP_X, TAP_Y}},
};

static void
draw(const provider_frame_t* frame) {
    clock_frame = frame->index;
    input_t in = tapped || results ? frame->input : (input_t){0};
    if (orientation) {
        in = (input_t){0};
        in.pressed = frame->index == 6 || (results && frame->index == 10);
    }
    in.boot.pressed = frame->index < page;
    app_list()->frame(frame->dt_ms, &in);
}

const frame_provider_t input_lab_provider = {
    .name = "input_lab",
    .frames = 8,
    .dt_ms = FRAME_MS,
    .input = touch,
    .input_count = sizeof touch / sizeof touch[0],
    .options = options,
    .setup = setup,
    .draw = draw,
};
