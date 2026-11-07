/*
 * scene_herd: loads the herd pack, shows the size's first copies and hides
 * the rest of the pool, starts each shown copy at its own phase of its clip
 * and flies the camera's lap; see scene_herd.h.
 */

#include "apps/render_lab/herd/scene_herd.h"

#include <stdbool.h>
#include <stddef.h>

#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/entity/entity.h"
#include "scene/scene.h"
#include "ui/ui.h"

static const char* TAG = "herd";

/* The scene's id in the capybara's pack, the scene keys' prefix, its camera,
 * and its copies' names, numbered from 0. */
#define HERD_SCENE       "capybara/herd"
#define HERD_KEY         "herd"
#define HERD_CAMERA      "camera"
#define HERD_COPY_FORMAT "capybara_%02d"

static const int* shown; /* the size's copy count */
static entity_t loaded;
static entity_t camera;
static entity_t* copies; /* the pool, in name order */
static int pool;

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[RENDER_LAB_STATUS_LEN];

static void
fail(const char* what) {
    render_lab_scene_fail(TAG, failure, sizeof failure, what);
}

/* The pool is every copy the pack names, from 0 up to the first it lacks. */
static bool
find_pool(void) {
    copies = render_lab_scene_copies(loaded, HERD_COPY_FORMAT, &pool);
    return copies != NULL;
}

/* The first N copies shown, each `i / pool` of the way through its clip, so
 * neighbours playing one clip are never in step; the rest hidden, so they
 * are neither posed nor skinned. */
static void
show_copies(void) {
    for (int i = 0; i < pool; i++) {
        const bool used = i < *shown;
        entity_set_enabled(copies[i], used);
        const uint32_t duration_ms = animator_clip_duration_ms(copies[i], animator_current_clip(copies[i]));
        if (used && duration_ms > 0) {
            (void)animator_seek(copies[i], ANIMATOR_CURRENT,
                                (uint32_t)((uint64_t)duration_ms * (uint32_t)i / (uint32_t)pool));
        }
    }
}

static void
apply_settings(void) {
    render_lab_apply_raster_settings();
    if (!handle_is_none(loaded)) {
        (void)render_lab_apply_camera_settings(camera);
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static bool
load(void) {
    loaded = render_lab_scene_load(TAG, failure, sizeof failure, HERD_SCENE, HERD_KEY);
    if (handle_is_none(loaded)) {
        return false;
    }
    camera = entity_find(loaded, name_hash(HERD_CAMERA));
    if (handle_is_none(camera)) {
        fail("no herd camera");
        return false;
    }
    if (!find_pool()) {
        fail("no memory for the herd");
        return false;
    }
    if (*shown > pool) {
        fail("too few capybaras in the herd");
        return false;
    }
    return true;
}

static void
scene_herd_enter(const render_lab_scene_t* self) {
    shown = self->data;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    if (!load()) {
        return;
    }
    show_copies();
    (void)camera_activate(camera);
    apply_settings();
}

static void
scene_herd_exit(void) {
    scene_release(loaded);
    loaded = ENTITY_NONE;
    copies = NULL;
    pool = 0;
}

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_herd_frame(uint32_t dt_ms) {
    (void)dt_ms;
    apply_settings();
}

/* The size drawn at, the triangle count and the copies, or why the scene did not load. */
static const char*
scene_herd_status(void) {
    return render_lab_scene_copies_status(failure, *shown);
}

/* The camera's lap. */
static render_lab_path_t
scene_herd_path(void) {
    return (render_lab_path_t){loaded, handle_is_none(loaded) ? ENTITY_NONE : camera, NULL};
}

#define HERD_COPIES(id_, key_, copies_) static const int herd_copies_##id_ = copies_;
HERD_SIZES(HERD_COPIES)

/* The copies are animated, so no frame is generated from the camera's pose alone. */
#define HERD_SCENE_ROW(id_, key_, copies_)                                                                             \
    [HERD_##id_] = {.name = "Herd " key_,                                                                              \
                    .key = HERD_KEY "-" key_,                                                                          \
                    .family = "Herd",                                                                                  \
                    .variant = key_,                                                                                   \
                    .data = &herd_copies_##id_,                                                                        \
                    .enter = scene_herd_enter,                                                                         \
                    .frame = scene_herd_frame,                                                                         \
                    .exit = scene_herd_exit,                                                                           \
                    .status = RENDER_LAB_SCENE_DEV(scene_herd_status),                                                 \
                    .path = scene_herd_path},

const render_lab_scene_t scene_herd[HERD_SIZE_COUNT] = {HERD_SIZES(HERD_SCENE_ROW)};
