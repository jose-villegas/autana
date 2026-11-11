/*
 * scene_stress: loads the stress pack, makes the preset's mesh into one
 * dynamic mesh every copy draws, places the copies the preset asks for
 * before the camera's swing and hides the rest of the pool; see
 * scene_stress.h. A per-frame preset makes its mesh again each update.
 */

#include "apps/render_lab/stress/scene_stress.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/scene_baked.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "esp_log.h"
#include "image/pixel/rgb565.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"
#include "ui/ui.h"

static const char* TAG = "stress";

/* The scene's id and pack, its camera, and its copies' names, numbered from 0. */
#define STRESS_SCENE          "stress"
#define STRESS_CAMERA         "camera"
#define STRESS_SLOT_FORMAT    "slot_%02d"

/* The longest copy name STRESS_SLOT_FORMAT makes, with its terminator. */
#define STRESS_SLOT_NAME_SIZE 16

static const stress_params_t* shown;
static entity_t loaded;
static entity_t camera; /* the swing the copies are placed for */
static entity_t viewer; /* the camera shown: the one the start named, else the swing */
static entity_t* slots; /* the pool's copies, in name order */
static int pool;
static stress_buffers_t buffers;
static r3d_dynamic_mesh_t mesh;
static uint32_t wave_ms;

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[LAB_STATUS_LEN];

static void
fail(const char* what) {
    lab_scene_fail(TAG, failure, sizeof failure, what);
}

static entity_t
find_slot(int index) {
    char name[STRESS_SLOT_NAME_SIZE];
    const int length = snprintf(name, sizeof name, STRESS_SLOT_FORMAT, index);
    return length > 0 && (size_t)length < sizeof name ? entity_find(loaded, name_hash(name)) : ENTITY_NONE;
}

/* The pool is every copy the pack names, from 0 up to the first it lacks. */
static bool
find_pool(void) {
    pool = 0;
    while (!handle_is_none(find_slot(pool))) {
        pool++;
    }
    slots = lab_scene_take(sizeof *slots * (size_t)(pool > 0 ? pool : 1));
    if (slots == NULL) {
        return false;
    }
    for (int i = 0; i < pool; i++) {
        slots[i] = find_slot(i);
    }
    return true;
}

static bool
take_mesh(void) {
    const int vertices = stress_vertex_count(shown);
    const int triangles = stress_triangle_count(shown);
    buffers = (stress_buffers_t){lab_scene_take(sizeof(vec3f_t) * (size_t)vertices),
                                 lab_scene_take(sizeof(uint8_t[3]) * (size_t)vertices),
                                 lab_scene_take(sizeof(uint16_t[3]) * (size_t)triangles)};
    const r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(vertices, triangles);
    const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
    void* block = lab_scene_take(bytes);
    return buffers.positions != NULL && buffers.colors != NULL && buffers.triangles != NULL && block != NULL
           && r3d_dynamic_mesh_init(&mesh, block, bytes, capacity, STRESS_TICKS_PER_UNIT);
}

/* Makes the mesh at `wave_ms` and hands it to every copy shown, which takes its bounds from it. */
static bool
build(void) {
    r3d_dynamic_source_t source;
    if (!stress_source(shown, wave_ms, &buffers, &source) || !r3d_dynamic_mesh_build(&mesh, &source)) {
        return false;
    }
    for (int i = 0; i < shown->copies; i++) {
        (void)renderer_set_mesh(slots[i], &mesh.mesh);
    }
    return true;
}

/* The arrangement is sized to where the camera's swing starts, and turned
 * about the origin's up so its +z faces it there. */
static void
place_copies(void) {
    (void)animator_seek(camera, ANIMATOR_CURRENT, 0);
    const vec3f_t eye = transform_world(camera).position;
    const lens_t* lens = camera_lens(entity_find(loaded, name_hash(STRESS_CAMERA)));
    const stress_framing_t seen = {sqrtf(vec3f_dot(eye, eye)), lens->half_fov_short_tan};
    transformf_t facing = transformf_identity();
    transformf_set_rotation(&facing, quatf_from_axis_angle(vec3f_unit_y(), atan2f(eye.x, eye.z)));
    for (int i = 0; i < pool; i++) {
        const bool used = i < shown->copies;
        entity_set_enabled(slots[i], used);
        if (used) {
            const transformf_t place = stress_copy_place(shown, i, seen);
            transform_set_local(slots[i], transformf_compose(&facing, &place));
        }
    }
}

static void
apply_settings(void) {
    render_lab_apply_raster_settings();
    if (!handle_is_none(loaded)) {
        (void)render_lab_apply_camera_settings(viewer);
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static bool
load(void) {
    loaded = lab_scene_load(TAG, failure, sizeof failure, STRESS_SCENE, "stress");
    if (handle_is_none(loaded)) {
        return false;
    }
    camera = entity_find(loaded, name_hash(STRESS_CAMERA));
    if (handle_is_none(camera) || camera_lens(entity_find(loaded, name_hash(STRESS_CAMERA))) == NULL) {
        fail("no stress camera");
        return false;
    }
    viewer = render_lab.start_camera == NULL ? camera : entity_find(loaded, name_hash(render_lab.start_camera));
    if (!find_pool() || !take_mesh()) {
        fail("no memory for stress");
        return false;
    }
    if (shown->copies > pool) {
        fail("too few stress copies");
        return false;
    }
    if (!build()) {
        fail("stress mesh did not fit");
        return false;
    }
    return true;
}

static void
scene_stress_enter(const lab_scene_t* self) {
    shown = self->data;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    wave_ms = 0;
    if (!load()) {
        return;
    }
    (void)camera_activate(
        entity_find(loaded, name_hash(render_lab.start_camera == NULL ? STRESS_CAMERA : render_lab.start_camera)));
    place_copies();
    apply_settings();
}

static void
scene_stress_exit(void) {
    scene_release(loaded);
    loaded = ENTITY_NONE;
    slots = NULL;
    pool = 0;
}

/* A per-frame preset's wave moves on by the frame's time, so a replay at the same steps draws the same frames. */
static void
scene_stress_update(uint32_t dt_ms) {
    if (handle_is_none(loaded) || failure[0] != '\0' || !stress_per_frame(shown)) {
        return;
    }
    wave_ms += dt_ms;
    if (!build()) {
        ESP_LOGW(TAG, "the mesh did not fit");
    }
}

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_stress_frame(uint32_t dt_ms) {
    (void)dt_ms;
    apply_settings();
}

/* The size drawn at, the triangle count and the copies, or why the scene did not load. */
static const char*
scene_stress_status(void) {
    static char buf[LAB_STATUS_LEN];
    if (failure[0] != '\0') {
        return failure;
    }
    if (snprintf(buf, sizeof buf, "%s%5d tris x%d", render_lab_dynres_size(),
                 render_context_frame(render_context_main()).stats.triangles, shown->copies)
        < 0) {
        buf[0] = '\0';
    }
    return buf;
}

/* The camera shown: swinging, unless the start named a still one. */
static lab_path_t
scene_stress_path(void) {
    return (lab_path_t){loaded, handle_is_none(loaded) ? ENTITY_NONE : viewer, NULL};
}

/* A still preset can have frames generated: only the camera moves. */
#define STRESS_SCENE_ROW(id_, key_, surface_, ...)                                                                     \
    [STRESS_##                                                                                                         \
        id_] = {.name = "Stress " key_,                                                                                \
                .key = STRESS_SCENE "-" key_,                                                                          \
                .family = "Stress",                                                                                    \
                .variant = key_,                                                                                       \
                .data = &stress_presets[STRESS_##id_],                                                                 \
                .enter = scene_stress_enter,                                                                           \
                .frame = scene_stress_frame,                                                                           \
                .exit = scene_stress_exit,                                                                             \
                .update = scene_stress_update,                                                                         \
                .status = LAB_SCENE_DEV(scene_stress_status),                                                          \
                .controls = STRESS_SURFACE_IS_PER_FRAME(STRESS_SURFACE_##surface_) ? NULL : scene_baked_controls,      \
                .generates_frames = !STRESS_SURFACE_IS_PER_FRAME(STRESS_SURFACE_##surface_),                           \
                .path = scene_stress_path},

const lab_scene_t scene_stress[STRESS_PRESET_COUNT] = {STRESS_PRESETS(STRESS_SCENE_ROW)};
