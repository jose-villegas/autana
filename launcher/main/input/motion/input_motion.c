/* input_motion: shared on-demand motion readers. */
#include "input/motion/input_motion.h"

#include "build/build_variant.h"
#include "input/input.h"
#include "input/motion/gravity.h"
#include "input/motion/shake.h"
#if CONFIG_LAUNCHER_DEVELOPMENT
#include <math.h>
#include "core/tune.h"
#include "esp_log.h"
#define MOTION_LOG_MAX_MS (60 * MATH_MS_PER_S)
#define MOTION_MG_PER_G   1000
TUNE_OWNER(motion);
TUNE(motion, log_ms, 0, 0, MOTION_LOG_MAX_MS);
static int64_t last_log_us;
#endif

static gravity_t gravity = {.down = {0, 0, -1}};
static shake_t shake;
static vec3f_t down = {0, 1, 0};
static int64_t frame_us, updated_us;
static bool pending = true, have_update;

void
input_motion_reset(void) {
    gravity_reset(&gravity);
    shake_reset(&shake);
    down = (vec3f_t){0, 1, 0};
    frame_us = updated_us = 0;
    pending = true;
    have_update = false;
}

void
input_motion_frame(int64_t now_us) {
    frame_us = now_us;
    pending = true;
}

static void
update(void) {
    if (!pending) {
        return;
    }
    pending = false;
    input_motion_t sample;
    if (!input_read_motion(&sample)) {
        down = (vec3f_t){0, 1, 0};
        shake_reset(&shake);
        return;
    }
    const int64_t elapsed = have_update && frame_us > updated_us ? frame_us - updated_us : 0;
    const uint32_t dt_us = elapsed > GRAVITY_MAX_DT_US ? GRAVITY_MAX_DT_US : (uint32_t)elapsed;
    updated_us = frame_us;
    have_update = true;
    gravity_update(&gravity, &sample, dt_us);
    shake_update(&shake, gravity_linear(&gravity), dt_us);
    down = gravity_in_free_fall(&gravity) ? (vec3f_t){0} : gravity_down(&gravity);
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (log_ms > 0 && frame_us - last_log_us >= (int64_t)log_ms * MATH_US_PER_MS) {
        ESP_LOGI("motion", "shake_smooth_mg %d", (int)lroundf(shake.smooth_g * MOTION_MG_PER_G));
        last_log_us = frame_us;
    }
#endif
}

vec3f_t
input_gravity(void) {
    update();
    return down;
}

float
input_shake(void) {
    update();
    return shake_level(&shake);
}
