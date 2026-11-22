#include "apps/render_lab/capybara_sponza/capybara_run.h"

#include <string.h>

#include "apps/render_lab/capybara_content.h"
#include "scene/animator/animator.h"

#define RUN_START_X (-616.0F)
#define RUN_END_X   487.0F
#define RUN_REST_S  3.0F
#define RUN_RAMP_S  2.5F

float
capybara_run_gallop_mps(entity_t runner) {
    const int clips = animator_clip_count(runner);
    for (int i = 0; i < clips; i++) {
        if (strcmp(animator_clip_name(runner, i), CAPYBARA_GALLOP) == 0) {
            return animator_clip_ground_mps(runner, i);
        }
    }
    return 0.0F;
}

motion_profile_t
capybara_run_profile(float gallop_mps, float scale) {
    return (motion_profile_t){.start = RUN_START_X,
                              .end = RUN_END_X,
                              .peak_speed = gallop_mps * scale * CAPYBARA_RUN_CM_PER_M,
                              .rest_s = RUN_REST_S,
                              .ramp_s = RUN_RAMP_S};
}
