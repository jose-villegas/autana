/* Sample an analytic banking flight and a timed hand-off for the rig reference. */
#include <math.h>
#include <stdio.h>

#include "kinematics/rig/rig.h"
#include "math/scalar/ease.h"
#include "math/scalar/math_const.h"
#include "math/text/math_text.h"

#define PATH_STEPS      64
#define TRAVEL          12.0F
#define RISE            3.0F
#define CLIMB           2.0F
#define TURN            3.0F
#define BANK            (MATH_PI / 3.0F)
#define BLEND_MS        1000U
#define HANDOFF_STEP_MS 25U
#define HANDOFF_END_MS  (BLEND_MS + BLEND_MS / 4U)

static const vec3f_t offset = {1.0F, 1.5F, -2.5F};
static const rig_bind_t binds[] = {RIG_BIND_WORLD, RIG_BIND_TARGET, RIG_BIND_TARGET_YAW};

static transformf_t
flight_pose(float progress) {
    const float phase = MATH_PI * progress;
    const vec3f_t position = {TRAVEL * progress, RISE * progress + CLIMB * sinf(phase), TURN * (1.0F - cosf(phase))};
    const vec3f_t tangent = {TRAVEL, RISE + CLIMB * MATH_PI * cosf(phase), TURN * MATH_PI * sinf(phase)};
    transformf_t pose = transformf_looking(position, tangent, vec3f_up());
    transformf_rotate(&pose, quatf_from_axis_angle(vec3f_forward(), BANK * sinf(phase)));
    return pose;
}

int
main(void) {
    printf("{\"paths\":[");
    for (int i = 0; i <= PATH_STEPS; i++) {
        const transformf_t target = flight_pose((float)i / PATH_STEPS);
        printf("%s[[%s]", i ? "," : "", vec3f_text(target.position).chars);
        for (size_t j = 0; j < sizeof binds / sizeof binds[0]; j++) {
            const vec3f_t follower = rig_follow_position(target, offset, binds[j]);
            printf(",[%s]", vec3f_text(follower).chars);
        }
        printf("]");
    }
    printf("],\"over_ms\":%u,\"handoff\":[", BLEND_MS);
    for (uint32_t ms = 0; ms <= HANDOFF_END_MS; ms += HANDOFF_STEP_MS) {
        printf("%s[%u,%.6f]", ms ? "," : "", (unsigned)ms, ease_smooth(ms, BLEND_MS));
    }
    puts("]}");
    return 0;
}
