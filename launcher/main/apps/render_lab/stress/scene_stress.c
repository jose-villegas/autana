/* scene_stress: static assets or shared generated meshes in a copy pool before a camera swing. */

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
#include "math/scalar/math_const.h"
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
static stress_buffers_t buffers[STRESS_MIX_LEVELS];
static r3d_dynamic_mesh_t mesh[STRESS_MIX_LEVELS];
static uint32_t wave_ms;

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[RENDER_LAB_STATUS_LEN];

static void
fail(const char* what) {
    render_lab_scene_fail(TAG, failure, sizeof failure, what);
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
    slots = render_lab_scene_take(sizeof *slots * (size_t)(pool > 0 ? pool : 1));
    if (slots == NULL) {
        return false;
    }
    for (int i = 0; i < pool; i++) {
        slots[i] = find_slot(i);
    }
    return true;
}

static int
mesh_count(void) {
    return shown->surface == STRESS_SURFACE_MIXED ? STRESS_MIX_LEVELS : 1;
}

static bool
take_mesh(void) {
    for (int i = 0; i < mesh_count(); i++) {
        const stress_params_t p = stress_copy_params(shown, i);
        const int vertices = stress_vertex_count(&p);
        const int triangles = stress_triangle_count(&p);
        buffers[i] = (stress_buffers_t){render_lab_scene_take(sizeof(vec3f_t) * (size_t)vertices),
                                        render_lab_scene_take(sizeof(uint8_t[3]) * (size_t)vertices),
                                        render_lab_scene_take(sizeof(uint16_t[3]) * (size_t)triangles)};
        const r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(vertices, triangles);
        const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
        void* block = render_lab_scene_take(bytes);
        if (buffers[i].positions == NULL || buffers[i].colors == NULL || buffers[i].triangles == NULL || block == NULL
            || !r3d_dynamic_mesh_init(&mesh[i], block, bytes, capacity, STRESS_TICKS_PER_UNIT)) {
            return false;
        }
    }
    return true;
}

static bool
build(void) {
    for (int i = 0; i < mesh_count(); i++) {
        const stress_params_t p = stress_copy_params(shown, i);
        r3d_dynamic_source_t source;
        if (!stress_source(&p, wave_ms, &buffers[i], &source) || !r3d_dynamic_mesh_build(&mesh[i], &source)) {
            return false;
        }
    }
    for (int i = 0; i < shown->copies; i++) {
        (void)renderer_set_mesh(slots[i], &mesh[i % mesh_count()].mesh);
    }
    return true;
}

/* The asset's longest side becomes one unit, centred like a generated primitive. */
static transformf_t
static_shape(void) {
    const r3d_lit_mesh_t* asset = renderer_mesh(slots[0]);
    const box3f_t box = r3d_lit_mesh_world_bounds(asset, NULL);
    const vec3f_t span = vec3f_sub(box.hi, box.lo);
    const float width = fmaxf(span.x, fmaxf(span.y, span.z));
    transformf_t shape = transformf_identity();
    transformf_set_scale(&shape, (vec3f_t){1.0F / width, 1.0F / width, 1.0F / width});
    transformf_set_rotation(&shape, quatf_from_axis_angle(vec3f_unit_y(), MATH_PI));
    const vec3f_t centre = quatf_rotate(shape.rotation, vec3f_add(box.lo, box.hi));
    transformf_set_position(&shape, vec3f_scale(centre, -0.5F / width));
    return shape;
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
            transformf_t place = stress_copy_place(shown, i, seen);
            if (shown->surface == STRESS_SURFACE_STATIC) {
                const transformf_t shape = static_shape();
                place = transformf_compose(&place, &shape);
            }
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
    loaded = render_lab_scene_load(TAG, failure, sizeof failure, STRESS_SCENE, "stress");
    if (handle_is_none(loaded)) {
        return false;
    }
    camera = entity_find(loaded, name_hash(STRESS_CAMERA));
    if (handle_is_none(camera) || camera_lens(entity_find(loaded, name_hash(STRESS_CAMERA))) == NULL) {
        fail("no stress camera");
        return false;
    }
    viewer = render_lab_start_camera == NULL ? camera : entity_find(loaded, name_hash(render_lab_start_camera));
    if (!find_pool() || (shown->surface != STRESS_SURFACE_STATIC && !take_mesh())) {
        fail("no memory for stress");
        return false;
    }
    if (shown->copies > pool) {
        fail("too few stress copies");
        return false;
    }
    if (shown->surface != STRESS_SURFACE_STATIC && !build()) {
        fail("stress mesh did not fit");
        return false;
    }
    return true;
}

static void
scene_stress_enter(const render_lab_scene_t* self) {
    shown = self->data;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    wave_ms = 0;
    if (!load()) {
        return;
    }
    (void)camera_activate(
        entity_find(loaded, name_hash(render_lab_start_camera == NULL ? STRESS_CAMERA : render_lab_start_camera)));
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
    static char buf[RENDER_LAB_STATUS_LEN];
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
static render_lab_path_t
scene_stress_path(void) {
    return (render_lab_path_t){loaded, handle_is_none(loaded) ? ENTITY_NONE : viewer, NULL};
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
                .status = RENDER_LAB_SCENE_DEV(scene_stress_status),                                                   \
                .controls = STRESS_SURFACE_IS_PER_FRAME(STRESS_SURFACE_##surface_) ? NULL : scene_baked_controls,      \
                .generates_frames = !STRESS_SURFACE_IS_PER_FRAME(STRESS_SURFACE_##surface_),                           \
                .path = scene_stress_path},

const render_lab_scene_t scene_stress[STRESS_PRESET_COUNT] = {STRESS_PRESETS(STRESS_SCENE_ROW)};
