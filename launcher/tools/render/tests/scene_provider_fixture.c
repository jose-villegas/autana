/* Scene provider hooks checked against a freshly advanced camera path. */
#include <math.h>

#include "../scene_viewer.c"
#include "scene/scene_shell.h"

#define FIXTURE_STEP_MS 100
#define FIXTURE_STEPS   3
#define POSE_TOLERANCE  0.00001f

static bool
check_path(void) {
    if (scene_provider.path_ms == NULL || scene_provider.seek == NULL) {
        return false;
    }
    const uint32_t duration = scene_animator_clip_duration_ms(shown, camera, 0);
    if (duration == 0 || scene_provider.path_ms() != duration) {
        return false;
    }
    const input_t input = {0};
    for (int i = 0; i < FIXTURE_STEPS; i++) {
        render_host_step(FIXTURE_STEP_MS, &input);
    }
    const transformf_t expected = scene_entity_world(shown, camera);
    scene_provider.seek(0);
    render_context_t* context = render_context_main();
    context->kept = true;
    context->paying = true;
    context->drawn_in_row = FRAMEGEN_RETRY_DRAWN;
    scene_provider.seek(FIXTURE_STEP_MS * FIXTURE_STEPS);
    const transformf_t actual = scene_entity_world(shown, camera);
    if (fabsf(actual.position.x - expected.position.x) > POSE_TOLERANCE
        || fabsf(actual.position.y - expected.position.y) > POSE_TOLERANCE
        || fabsf(actual.position.z - expected.position.z) > POSE_TOLERANCE
        || fabsf(actual.rotation.x - expected.rotation.x) > POSE_TOLERANCE
        || fabsf(actual.rotation.y - expected.rotation.y) > POSE_TOLERANCE
        || fabsf(actual.rotation.z - expected.rotation.z) > POSE_TOLERANCE
        || fabsf(actual.rotation.w - expected.rotation.w) > POSE_TOLERANCE) {
        return false;
    }
    return !context->kept && !context->paying && context->drawn_in_row == 0;
}

int
main(int argc, char** argv) {
    int exit_code;
    if (!render_host_open(&scene_provider, argc, argv, &exit_code)) {
        return exit_code;
    }
    const bool ok = check_path();
    if (!ok) {
        fprintf(stderr, "scene provider path, pose or history check failed\n");
    }
    return render_host_close() && ok ? 0 : 1;
}
