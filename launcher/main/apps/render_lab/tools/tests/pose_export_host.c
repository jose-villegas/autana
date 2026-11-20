/* pose_export_host: export a seeked triangle fixture through the host frame writer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/render_lab/render_lab.h"
#include "asset/asset_file.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/renderer/renderer.h"
#include "scene/transform/transform.h"

#define POSE_SCENE        "scene"
#define POSE_ENTITY       "subject"
#define POSE_CLIP         "move"
#define POSE_SKIN         "skin"
#define POSE_SEEK         "seek"

#define POSE_TIME_MS      375U
#define POSE_SCALE        2.0F
#define POSE_X            1.0F
#define POSE_Y            2.0F
#define POSE_Z            3.0F

#define POSE_PACK_ENV     "AUTANA_POSE_TEST_PACK"
#define POSE_SKELETON_ENV "AUTANA_POSE_TEST_SKELETON"

lab_t render_lab;
static entity_t subject;

static void
enter(void) {
    asset_pack_t* pack = malloc(sizeof *pack);
    void* bytes;
    if (pack == NULL || asset_file_open(getenv(POSE_PACK_ENV), pack, &bytes) != ASSET_OK) {
        exit(EXIT_FAILURE);
    }
    const entity_t root = scene_load_from(pack, POSE_SCENE);
    subject = entity_find(root, name_hash(POSE_ENTITY));
    const char* clips[] = {POSE_CLIP};
    scene_failure_t why;
    if (scene_state(root) != PENDING_READY || !animator_set_clips(subject, getenv(POSE_SKELETON_ENV), clips, 1, &why)
        || !renderer_set_skin(subject, POSE_SKIN, &why) || !animator_play(subject, 0, 0)
        || !camera_activate(camera_first(root))) {
        exit(EXIT_FAILURE);
    }
}

bool
render_lab_console(const char* words) {
    if (strcmp(words, POSE_SEEK) != 0) {
        return false;
    }
    transformf_t world = transformf_identity();
    transformf_set_position(&world, (vec3f_t){POSE_X, POSE_Y, POSE_Z});
    transformf_set_scale(&world, (vec3f_t){POSE_SCALE, POSE_SCALE, POSE_SCALE});
    transform_set_world(subject, world);
    return animator_seek(subject, ANIMATOR_CURRENT, POSE_TIME_MS)
           && animator_set_paused(subject, ANIMATOR_CURRENT, true);
}

static void
frame(uint32_t dt_ms, const input_t* input) {}

const app_t*
app_list(void) {
    static const app_t app = {.name = "pose fixture", .enter = enter, .frame = frame, .update = frame};
    return &app;
}

const char* const*
render_lab_small_fill_names(int* count) {
    *count = 0;
    return NULL;
}

bool
render_lab_small_fill_select(int index) {
    return false;
}
