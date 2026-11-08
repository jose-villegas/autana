/* Scene provider hooks checked against a freshly advanced camera path. */
#include <math.h>

#include "../scene_viewer.c"
#include "scene/scene_shell.h"

#define FIXTURE_STEP_MS 100
#define FIXTURE_STEPS   3
#define POSE_TOLERANCE  0.00001f

static bool
pose_near(transformf_t actual, transformf_t expected) {
    const vec3f_t position = vec3f_sub(actual.position, expected.position);
    const float differences[] = {position.x,
                                 position.y,
                                 position.z,
                                 actual.rotation.x - expected.rotation.x,
                                 actual.rotation.y - expected.rotation.y,
                                 actual.rotation.z - expected.rotation.z,
                                 actual.rotation.w - expected.rotation.w};
    for (size_t i = 0; i < sizeof differences / sizeof differences[0]; i++) {
        if (fabsf(differences[i]) > POSE_TOLERANCE) {
            return false;
        }
    }
    return true;
}

static bool
check_path(void) {
    if (scene_provider.path_ms == NULL || scene_provider.seek == NULL) {
        return false;
    }
    const uint32_t duration = animator_clip_duration_ms(camera, 0);
    if (duration == 0 || scene_provider.path_ms() != duration) {
        return false;
    }
    const transformf_t initial = transform_world(camera);
    const input_t input = {0};
    for (int i = 0; i < FIXTURE_STEPS; i++) {
        render_host_step(FIXTURE_STEP_MS, &input);
    }
    const transformf_t expected = transform_world(camera);
    scene_provider.seek(0);
    const transformf_t reset = transform_world(camera);
    if (!pose_near(reset, initial) || pose_near(reset, expected)) {
        return false;
    }
    render_context_t* context = render_context_main();
    context->kept = true;
    context->paying = true;
    context->drawn_in_row = FRAMEGEN_RETRY_DRAWN;
    scene_provider.seek(FIXTURE_STEP_MS * FIXTURE_STEPS);
    const transformf_t actual = transform_world(camera);
    if (!pose_near(actual, expected)) {
        return false;
    }
    return !context->kept && !context->paying && context->drawn_in_row == 0;
}

/* One --video of the room holds the fit the run before reported: that many
 * frames open, one more is refused. The runs write beside `out`. */
static bool
check_video_fit(int64_t fit, const char* out) {
    char frames[24];
    char video[1024];
    char image[1024];
    (void)snprintf(video, sizeof video, "%s.avi", out);
    (void)snprintf(image, sizeof image, "%s.fit.bmp", out);
    char* args[] = {"fit", "--scene", "room", "--frames", frames, "--video", video, "-o", image};
    const int count = (int)(sizeof args / sizeof args[0]);
    int exit_code;
    (void)snprintf(frames, sizeof frames, "%lld", (long long)fit + 1);
    if (render_host_open(&scene_provider, count, args, &exit_code)) {
        (void)render_host_close();
        fprintf(stderr, "%s frames opened a video past the fit\n", frames);
        return false;
    }
    (void)snprintf(frames, sizeof frames, "%lld", (long long)fit);
    if (!render_host_open(&scene_provider, count, args, &exit_code)) {
        fprintf(stderr, "%s frames, the fit, did not open a video\n", frames);
        return false;
    }
    return render_host_close();
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
    const int64_t fit = render_host_video_frames_fit();
    const char* out = "hooks.bmp";
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            out = argv[i + 1];
        }
    }
    return render_host_close() && ok && check_video_fit(fit, out) ? 0 : 1;
}
