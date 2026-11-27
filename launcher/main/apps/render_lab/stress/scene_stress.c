/* scene_stress: a pool of static or generated copies, or a pack's own baked mesh, before a moving or held camera. */

#include "apps/render_lab/stress/scene_stress.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/scene_baked.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "esp_log.h"
#include "image/pixel/rgb565.h"
#include "kinematics/hinge/hinge.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathi.h"
#include "math/scalar/units.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/probe/sh_probes.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"
#include "ui/ui.h"

static const char* TAG = "stress";

/* Copies are named consecutively from zero: the generated presets' slots, the carved presets' props. */
#define STRESS_SLOT_FORMAT "slot_%02d"
#define STRESS_WING_FORMAT STRESS_SLOT_FORMAT "_wing_%c"
#define STRESS_PROP_FORMAT "prop_%02d"

static const stress_params_t* shown;
static entity_t loaded;
static entity_t camera; /* the swing the copies are placed for */
static entity_t viewer; /* the camera shown: the one the start named, else the swing */
static entity_t* slots; /* the pool's copies, in name order */
static int pool;
#define STRESS_WING_SIDES 2
static const char wing_suffix[STRESS_WING_SIDES] = {'l', 'r'};
static entity_t* wings;
static hinge_t* wing_hinges;
static stress_buffers_t buffers[STRESS_MIX_LEVELS];
static r3d_dynamic_mesh_t mesh[STRESS_MIX_LEVELS];
static uint32_t wave_ms;
static bool held;
#define STRESS_PROBE_SIDE              2
#define STRESS_PROBE_COUNT             (STRESS_PROBE_SIDE * STRESS_PROBE_SIDE * STRESS_PROBE_SIDE)
#define STRESS_PROBE_SCALE             (1.0F / 1024.0F)
#define STRESS_PROBE_EXTENT            10.0F
#define STRESS_PROBE_BASE              0.5F
#define STRESS_PROBE_STEP              0.1F
#define STRESS_PROBE_UP                0.25F
#define STRESS_CONTACT_LIGHT_ELEVATION (35.0F * MATH_PI / 180.0F)
#define STRESS_GROUND_WIDTH_SCALE      1.5F
#define STRESS_GROUND_ROTATION         (-0.25F * MATH_TAU)
static int16_t probe_coefficients[STRESS_PROBE_COUNT][SH_PROBES_COMPONENTS];
static sh_probes_t probes;
static int skinned_count;
static entity_t* grounds;

/* What the preset's settings replaced, in its order, and how many it set. */
static int32_t settings_before[STRESS_SETTINGS_MAX];
static int settings_set;

/* A textured preset's texture and its texturing of the one mesh. */
static r3d_texture_t* texture;
static r3d_mesh_texturing_t texturing;

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[LAB_STATUS_LEN];

static void
fail(const char* what) {
    lab_scene_fail(TAG, failure, sizeof failure, what);
}

static bool
find_wings(int copy) {
    for (int side = 0; side < STRESS_WING_SIDES; side++) {
        char name[ASSET_NAME_MAX];
        if (snprintf(name, sizeof name, STRESS_WING_FORMAT, copy, wing_suffix[side]) < 0) {
            return false;
        }
        const int at = (copy * STRESS_WING_SIDES) + side;
        wings[at] = entity_find(loaded, name_hash(name));
        if (handle_is_none(wings[at]) || !handle_equal(entity_parent(wings[at]), slots[copy])) {
            return false;
        }
        wing_hinges[at] = (hinge_t){.rest = transform_local(wings[at]),
                                    .axis = side == 0 ? vec3f_up() : vec3f_down(),
                                    .min = -STRESS_FLAP_LIMIT,
                                    .max = STRESS_FLAP_LIMIT};
    }
    return true;
}

/* The pool is every copy of the preset's kind the pack names, from 0 up to
 * the first it lacks; every copy of the other kind is hidden. */
static bool
find_pool(void) {
    const bool carved = shown->surface == STRESS_SURFACE_CARVED;
    int others = 0;
    entity_t* hidden = lab_scene_copies(loaded, carved ? STRESS_SLOT_FORMAT : STRESS_PROP_FORMAT, &others);
    for (int i = 0; hidden != NULL && i < others; i++) {
        entity_set_enabled(hidden[i], false);
    }
    slots = lab_scene_copies(loaded, carved ? STRESS_PROP_FORMAT : STRESS_SLOT_FORMAT, &pool);
    if (slots == NULL || shown->surface != STRESS_SURFACE_HINGED) {
        const bool found = slots != NULL && hidden != NULL;
        if (!found) {
            fail("no stress copies");
        }
        return found;
    }
    wings = lab_scene_take(sizeof *wings * (size_t)pool * STRESS_WING_SIDES);
    wing_hinges = lab_scene_take(sizeof *wing_hinges * (size_t)pool * STRESS_WING_SIDES);
    if (wings == NULL || wing_hinges == NULL) {
        fail("no stress copies");
        return false;
    }
    for (int i = 0; i < pool; i++) {
        if (!find_wings(i)) {
            fail("missing or misparented stress wing");
            return false;
        }
    }
    return true;
}

static void
set_contact_light(entity_t root) {
    if (shown->contact_shape == SCENE_CONTACT_BLOB && !shown->contact_warp) {
        return;
    }
    const entity_t light = entity_find(root, name_hash(CAPYBARA_LIGHT));
    transformf_t pose = transform_local(light);
    transformf_set_rotation(&pose,
                            quatf_from_axis_angle(vec3f_unit_x(), (MATH_PI / 2) - STRESS_CONTACT_LIGHT_ELEVATION));
    transform_set_local(light, pose);
}

static bool
load_skins(void) {
    for (int i = 0; i < pool; i++) {
        entity_set_enabled(slots[i], false);
    }
    grounds = slots;
    slots = lab_scene_take(STRESS_SKINNED_COPIES * sizeof *slots);
    if (slots == NULL) {
        return false;
    }
    pool = STRESS_SKINNED_COPIES;
    probes = (sh_probes_t){.origin = {-STRESS_PROBE_EXTENT, -STRESS_PROBE_EXTENT, -STRESS_PROBE_EXTENT},
                           .spacing = {2 * STRESS_PROBE_EXTENT, 2 * STRESS_PROBE_EXTENT, 2 * STRESS_PROBE_EXTENT},
                           .dims = {STRESS_PROBE_SIDE, STRESS_PROBE_SIDE, STRESS_PROBE_SIDE},
                           .scale = STRESS_PROBE_SCALE,
                           .coefficients = (const int16_t (*)[SH_PROBES_COMPONENTS])probe_coefficients};
    memset(probe_coefficients, 0, sizeof probe_coefficients);
    for (int p = 0; p < STRESS_PROBE_COUNT; p++) {
        for (int channel = 0; channel < SH1_CHANNELS; channel++) {
            probe_coefficients[p][channel] =
                (int16_t)((STRESS_PROBE_BASE + STRESS_PROBE_STEP * (float)p) / STRESS_PROBE_SCALE);
            probe_coefficients[p][SH1_CHANNELS + channel] = (int16_t)(STRESS_PROBE_UP / STRESS_PROBE_SCALE);
        }
    }
    for (int i = 0; i < pool; i++) {
        slots[i] = lab_scene_load(TAG, failure, sizeof failure, CAPYBARA_SCENE, CAPYBARA_SCENE);
        if (handle_is_none(slots[i])) {
            return false;
        }
        skinned_count++;
        entity_set_enabled(entity_find(slots[i], name_hash(CAPYBARA_MEADOW)), false);
        const entity_t skin = entity_find(slots[i], name_hash(CAPYBARA_SUBJECT));
        for (int c = 0; c < animator_clip_count(skin); c++) {
            if (strcmp(animator_clip_name(skin, c), CAPYBARA_GALLOP) == 0) {
                (void)animator_play(skin, c, CAPYBARA_CLIP_CUT_MS);
            }
        }
        scene_set_probes(slots[i], &probes);
        set_contact_light(slots[i]);
    }
    return true;
}

static int
mesh_count(void) {
    return shown->shape == STRESS_SHAPE_GRID ? STRESS_MIX_LEVELS : 1;
}

static bool
take_mesh(void) {
    for (int i = 0; i < mesh_count(); i++) {
        const stress_params_t p = stress_copy_params(shown, i);
        const int vertices = stress_vertex_count(&p);
        const int triangles = stress_triangle_count(&p);
        const bool textured = stress_textured(shown);
        buffers[i] = (stress_buffers_t){lab_scene_take(sizeof(vec3f_t) * (size_t)vertices),
                                        lab_scene_take(sizeof(uint8_t[3]) * (size_t)vertices),
                                        lab_scene_take(sizeof(uint16_t[3]) * (size_t)triangles),
                                        textured ? lab_scene_take(sizeof(int16_t[2]) * (size_t)vertices) : NULL};
        r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(vertices, triangles);
        capacity.textured = textured;
        const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
        void* block = lab_scene_take(bytes);
        if ((textured && buffers[i].uvs == NULL) || buffers[i].positions == NULL || buffers[i].colors == NULL
            || buffers[i].triangles == NULL || block == NULL
            || !r3d_dynamic_mesh_init(&mesh[i], block, bytes, capacity, STRESS_TICKS_PER_UNIT)) {
            return false;
        }
    }
    return true;
}

/* The surface's entry, in the scene's arena, and a texturing of every
 * triangle of the one mesh with it, on the UVs the build copies. */
static bool
take_texture(void) {
    const size_t bytes = stress_texture_bytes(shown->surface);
    uint8_t* ltex = lab_scene_take(bytes);
    texture = lab_scene_take(sizeof *texture);
    const r3d_lit_mesh_t* m = &mesh[0].mesh;
    uint8_t (*material)[R3D_TEXTURE_CHARTS] = lab_scene_take(sizeof *material * (size_t)m->cluster_count);
    int32_t* shift = lab_scene_take(sizeof *shift * (size_t)m->cluster_count);
    /* The mesh is built later: its codes cover every triangle it can hold. */
    const size_t codes = ((size_t)mesh[0].capacity.triangles + R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE - 1U)
                         / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE;
    uint8_t* code = lab_scene_take(codes);
    if (ltex == NULL || texture == NULL || material == NULL || shift == NULL || code == NULL) {
        return false;
    }
    stress_texture_write(shown->surface, ltex);
    const asset_view_t view = {ltex, (uint32_t)bytes};
    if (r3d_texture_from_asset(&view, texture) != ASSET_OK) {
        return false;
    }
    /* One chart, the texture, under every triangle. */
    memset(material, R3D_TEXTURE_NONE, sizeof *material * (size_t)m->cluster_count);
    for (int c = 0; c < m->cluster_count; c++) {
        material[c][0] = 0;
    }
    memset(shift, 0, sizeof *shift * (size_t)m->cluster_count);
    uint8_t chart_0 = 0;
    for (unsigned k = 0; k < R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE; k++) {
        chart_0 |= (uint8_t)(1U << (k * R3D_TEXTURE_UVS_CODE_BITS));
    }
    memset(code, chart_0, codes);
    texturing = (r3d_mesh_texturing_t){.uvs = (const int16_t (*)[2])mesh[0].uvs,
                                       .cluster_material = (const uint8_t (*)[R3D_TEXTURE_CHARTS])material,
                                       .cluster_shift = shift,
                                       .triangle_code = code,
                                       .materials = texture,
                                       .material_count = 1};
    return true;
}

static bool
build(void) {
    for (int i = 0; i < mesh_count(); i++) {
        const stress_params_t p = stress_copy_params(shown, i);
        r3d_mesh_source_t source;
        if (!stress_source(&p, wave_ms, &buffers[i], &source) || !r3d_dynamic_mesh_build(&mesh[i], &source)) {
            return false;
        }
        if (stress_textured(shown)) {
            mesh[i].mesh.texturing = &texturing;
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
    const entity_t subject = stress_skinned(shown) ? entity_find(slots[0], name_hash(CAPYBARA_SUBJECT)) : slots[0];
    const r3d_lit_mesh_t* asset = renderer_mesh(subject);
    box3f_t box = r3d_lit_mesh_world_bounds(asset, NULL);
    for (entity_t e = entity_next(loaded, ENTITY_NONE); !handle_is_none(e); e = entity_next(loaded, e)) {
        const r3d_lit_mesh_t* part = renderer_mesh(e);
        if (shown->surface == STRESS_SURFACE_HINGED && part != NULL && handle_equal(entity_parent(e), slots[0])) {
            transformf_t local = transform_local(e);
            const mat4f_t model = transformf_matrix(&local);
            box = box3f_union(box, r3d_lit_mesh_world_bounds(part, &model));
        }
    }
    const vec3f_t span = vec3f_sub(box.hi, box.lo);
    const float width = fmaxf(span.x, fmaxf(span.y, span.z));
    transformf_t shape = transformf_identity();
    transformf_set_scale(&shape, (vec3f_t){1.0F / width, 1.0F / width, 1.0F / width});
    transformf_set_rotation(&shape, quatf_from_axis_angle(vec3f_unit_y(), MATH_PI));
    const vec3f_t centre = quatf_rotate(shape.rotation, vec3f_add(box.lo, box.hi));
    transformf_set_position(&shape, vec3f_scale(centre, -0.5F / width));
    return shape;
}

static transformf_t
copy_local(int i, stress_framing_t seen, const transformf_t* shape) {
    transformf_t place = stress_copy_place(shown, i, seen);
    if (stress_baked(shown) || stress_skinned(shown)) {
        place = transformf_compose(&place, shape);
    }
    if (shown->surface == STRESS_SURFACE_CONTACT_BLOBS) {
        place.rotation = quatf_from_axis_angle(vec3f_unit_x(), STRESS_GROUND_ROTATION);
    }
    return place;
}

static void __attribute__((noinline))
copy_extras(int i) {
    if (shown->surface == STRESS_SURFACE_CONTACT_BLOBS) {
        const scene_contact_t contact = {transform_world(slots[i]).position.y, SCENE_CONTACT_BLOB_STRENGTH_DEFAULT,
                                         SCENE_CONTACT_BLOB, false};
        scene_set_contact(slots[i], &contact);
    }
    if (stress_skinned(shown)) {
        const box3f_t bounds = scene_bounds(slots[i]);
        const vec3f_t span = vec3f_sub(bounds.hi, bounds.lo);
        const float width = fmaxf(span.x, span.z) * STRESS_GROUND_WIDTH_SCALE;
        transformf_t floor = transformf_identity();
        floor.position = box3f_centre(bounds);
        floor.position.y = bounds.lo.y;
        floor.rotation = quatf_from_axis_angle(vec3f_unit_x(), STRESS_GROUND_ROTATION);
        floor.scale = (vec3f_t){width, width, width};
        transform_set_local(grounds[i], floor);
        const entity_t subject = entity_find(slots[i], name_hash(CAPYBARA_SUBJECT));
        const scene_contact_t contact = {floor.position.y, SCENE_CONTACT_BLOB_STRENGTH_DEFAULT, shown->contact_shape,
                                         shown->contact_warp};
        scene_set_contact(subject, &contact);
        entity_set_enabled(grounds[i], true);
    }
}

/* The arrangement is sized to where the camera's swing starts, and turned
 * about the origin's up so its +z faces it there. */
static void
place_copies(void) {
    if (shown->copies == 0) {
        return; /* a pack without a pool shows its meshes where it placed them */
    }
    (void)animator_seek(camera, ANIMATOR_CURRENT, 0);
    const vec3f_t eye = transform_world(camera).position;
    const lens_t* lens = camera_lens(camera);
    const stress_framing_t seen = {sqrtf(vec3f_dot(eye, eye)), lens->half_fov_short_tan};
    transformf_t facing = transformf_identity();
    transformf_set_rotation(&facing, quatf_from_axis_angle(vec3f_unit_y(), atan2f(eye.x, eye.z)));
    const transformf_t shape = (stress_baked(shown) || stress_skinned(shown)) ? static_shape() : transformf_identity();
    for (int i = 0; i < pool; i++) {
        const bool used = i < shown->copies;
        entity_set_enabled(slots[i], used);
        if (used) {
            const transformf_t place = copy_local(i, seen, &shape);
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

/* The preset's settings, each through the registry SET writes, keeping the
 * value it replaces. False, naming the setting, when one is unknown or out of
 * its range, or when the image has no registry to set it through. */
static bool
set_preset_settings(void) {
    for (int i = 0; i < STRESS_SETTINGS_MAX && shown->settings[i].name != NULL; i++) {
#if TUNE_ENABLED
        const tune_entry_t* entry = tune_find(tune_shared(), shown->settings[i].name);
        if (entry != NULL) {
            settings_before[i] = *entry->value;
        }
        if (entry != NULL && tune_set(tune_shared(), shown->settings[i].name, shown->settings[i].value)) {
            settings_set = i + 1;
            continue;
        }
#endif
        fail(shown->settings[i].name);
        return false;
    }
    return true;
}

static void
put_back_settings(void) {
#if TUNE_ENABLED
    for (int i = settings_set - 1; i >= 0; i--) {
        (void)tune_set(tune_shared(), shown->settings[i].name, settings_before[i]);
    }
#endif
    settings_set = 0;
}

static bool
load(void) {
    loaded = lab_scene_load(TAG, failure, sizeof failure, shown->scene != NULL ? shown->scene : STRESS_SCENE, "stress");
    if (handle_is_none(loaded)) {
        return false;
    }
    camera = entity_find(loaded, name_hash(shown->camera));
    if (handle_is_none(camera) || camera_lens(camera) == NULL) {
        fail("no stress camera");
        return false;
    }
    viewer = render_lab.start_camera == NULL ? camera : entity_find(loaded, name_hash(render_lab.start_camera));
    if (!find_pool()) {
        return false;
    }
    if (stress_skinned(shown)) {
        return take_mesh() && build() && load_skins();
    }
    if (!stress_baked(shown) && !take_mesh()) {
        fail("no memory for stress");
        return false;
    }
    if (shown->copies > pool) {
        fail("too few stress copies");
        return false;
    }
    if (stress_textured(shown) && !take_texture()) {
        fail("no memory for the stress texture");
        return false;
    }
    if (!stress_baked(shown) && !build()) {
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
    held = false;
    skinned_count = 0;
    if (!set_preset_settings() || !load()) {
        return;
    }
    (void)camera_set_skip_hidden(viewer, shown->skip_hidden);
    (void)camera_activate(
        entity_find(loaded, name_hash(render_lab.start_camera == NULL ? shown->camera : render_lab.start_camera)));
    place_copies();
    for (int i = 0; i < mathi_min(shown->copies, pool); i++) {
        copy_extras(i);
    }
    if (shown->hold_camera) {
        (void)animator_set_paused(viewer, ANIMATOR_CURRENT, true);
    }
    apply_settings();
}

static void
scene_stress_exit(void) {
    put_back_settings();
    for (int i = 0; i < skinned_count; i++) {
        scene_release(slots[i]);
    }
    skinned_count = 0;
    scene_release(loaded);
    loaded = ENTITY_NONE;
    slots = NULL;
    pool = 0;
    wings = NULL;
    wing_hinges = NULL;
}

static void
flap(void) {
    FRAME_COST_BEGIN(flap_from);
    const float seconds = (float)wave_ms / (float)MATH_MS_PER_S;
    /* A fixed sine, phase-stepped per copy, so the wings move out of step
     * and a replay at the same time gives the same frame. */
    for (int i = 0; i < shown->copies; i++) {
        const float phase = STRESS_FLAP_PHASE_STEP * (float)i;
        const float angle =
            (STRESS_FLAP_AMPLITUDE * sinf((MATH_TAU * STRESS_FLAP_HZ * seconds) + phase)) + STRESS_FLAP_BIAS;
        for (int side = 0; side < STRESS_WING_SIDES; side++) {
            const int at = (i * STRESS_WING_SIDES) + side;
            transform_set_local(wings[at], hinge_pose(&wing_hinges[at], angle));
        }
    }
    FRAME_COST_END(flap_from, STRESS_FLAP_STAGE_NAME);
}

/* A per-frame preset's wave and a hinged preset's flap move on by the frame's time,
 * so a replay at the same steps draws the same frames. */
static void
scene_stress_update(uint32_t dt_ms) {
    if (handle_is_none(loaded) || failure[0] != '\0') {
        return;
    }
    if (!held) {
        wave_ms += dt_ms;
    }
    if (shown->surface == STRESS_SURFACE_HINGED) {
        flap();
    } else if (stress_per_frame(shown) && !build()) {
        ESP_LOGW(TAG, "the mesh did not fit");
    }
}

static bool
seek_flap(uint32_t time_ms) {
    if (handle_is_none(loaded) || failure[0] != '\0' || !animator_seek(camera, ANIMATOR_CURRENT, time_ms)) {
        return false;
    }
    (void)animator_set_paused(camera, ANIMATOR_CURRENT, true);
    wave_ms = time_ms;
    held = true;
    flap();
    return true;
}

static void
play_flap(void) {
    held = false;
    (void)animator_set_paused(camera, ANIMATOR_CURRENT, false);
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
    return render_lab_copies_status(failure, shown->copies);
}

/* The camera shown: swinging, unless the start named a still one. */
static lab_path_t
scene_stress_path(void) {
    lab_path_t path = {.scene = loaded, .entity = handle_is_none(loaded) ? ENTITY_NONE : viewer};
    if (!handle_is_none(loaded) && shown->surface == STRESS_SURFACE_HINGED) {
        path.seek = seek_flap;
        path.play = play_flap;
        path.duration_ms = animator_clip_duration_ms(camera, animator_current_clip(camera));
    }
    return path;
}

/* A still preset can have frames generated: only the camera moves. */
#define STRESS_SCENE_ROW(id_, key_, surface_, ...)                                                                     \
    [STRESS_##id_] = {.name = "Stress " key_,                                                                          \
                      .key = STRESS_SCENE "-" key_,                                                                    \
                      .family = "Stress",                                                                              \
                      .variant = key_,                                                                                 \
                      .data = &stress_presets[STRESS_##id_],                                                           \
                      .enter = scene_stress_enter,                                                                     \
                      .frame = scene_stress_frame,                                                                     \
                      .exit = scene_stress_exit,                                                                       \
                      .update = scene_stress_update,                                                                   \
                      .status = LAB_SCENE_DEV(scene_stress_status),                                                    \
                                                                                                                       \
                      .generates_frames = STRESS_SURFACE_GENERATES_FRAMES(STRESS_SURFACE_##surface_),                  \
                      .path = scene_stress_path},

const lab_scene_t scene_stress[STRESS_PRESET_COUNT] = {STRESS_PRESETS(STRESS_SCENE_ROW)};
