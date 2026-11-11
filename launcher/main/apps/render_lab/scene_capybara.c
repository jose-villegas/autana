/*
 * scene_capybara: the capybara on its meadow, orbited by one finger.
 *
 * The camera circles its bounding sphere at the distance that fits it from
 * every angle (orbit_motion.h); a drag turns it, a double tap brings it home
 * (orbit_motion_touch.h). The capybara is skinned and lit at run time; the
 * meadow's light is baked.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "apps/render_lab/capybara_content.h"

#include "app/app.h"
#include "console/console_words.h"
#include "core/tune.h"
#include "display/display.h"
#include "draw/draw.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "lab/lab_clips.h"
#include "lab/lab_orbit.h"
#include "lab/lab_scene.h"
#include "math/scalar/math_const.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_dynres.h"
#include "render_lab_view.h"
#include "scene/camera/camera.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"
#include "ui/ui.h"
#if CONFIG_LAUNCHER_SELFTEST
#include "image/pixel/rgb565.h"
#include "lab/test/lab_test.h"
#endif

static const char* TAG = "capybara";

/* The scene's id and pack, its camera, and the entity the camera circles. */
#define CAPYBARA_CAMERA       "camera"
#define CAPYBARA_DEFAULT_CLIP "idle"

/* Where the view starts: three-quarters from the front, a little above. */
#define CAPYBARA_HOME_YAW     (135.0F * MATH_RADIANS_PER_DEGREE)
#define CAPYBARA_HOME_PITCH   (15.0F * MATH_RADIANS_PER_DEGREE)

/* The yaw that looks the capybara in the face: the model faces -z. */
#define CAPYBARA_FRONT_YAW    (180.0F * MATH_RADIANS_PER_DEGREE)

/* The lowest the eye goes: a little above level, so the meadow stays below
 * it. A zoom reaches in to half the fitting distance (the model about twice
 * as large) and out to three times it, never closer than two near planes
 * outside the sphere. */
static const lab_orbit_frame_t CAPYBARA_FRAME = {
    .home_yaw = CAPYBARA_HOME_YAW,
    .home_pitch = CAPYBARA_HOME_PITCH,
    .pitch_min = 5.0F * MATH_RADIANS_PER_DEGREE,
    .zoom_in = 0.5F,
    .zoom_out = 3.0F,
    .near_clearance = 2.0F,
};

/* The console's named views: side and back are quarter turns on from the
 * front; the side is the flank the home view does not show. */
typedef struct {
    const char* name;
    float yaw;
    bool closest; /* at the zoom-in limit rather than the home distance */
} capybara_view_t;

static const capybara_view_t views[] = {
    {CAPYBARA_VIEW_HOME, CAPYBARA_HOME_YAW, false},
    {CAPYBARA_VIEW_FRONT, CAPYBARA_FRONT_YAW, false},
    {CAPYBARA_VIEW_SIDE, CAPYBARA_FRONT_YAW + (MATH_TAU / 4.0F), false},
    {CAPYBARA_VIEW_BACK, CAPYBARA_FRONT_YAW + (MATH_TAU / 2.0F), false},
    {CAPYBARA_VIEW_CLOSE, CAPYBARA_HOME_YAW, true},
};

static entity_t capybara;
static entity_t camera_entity;
static entity_t subject_entity;
static lab_clips_t choice;
static lab_orbit_t orbit;

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[48];

static void
fail(const char* what) {
    lab_scene_fail(TAG, failure, sizeof failure, what);
}

/* Home and limits from the subject's sphere and the scene camera's lens. */
static bool
frame_subject(void) {
    const name_hash_t camera = name_hash(CAPYBARA_CAMERA);
    const entity_t subject = entity_find(capybara, name_hash(CAPYBARA_SUBJECT));
    camera_entity = entity_find(capybara, camera);
    if (handle_is_none(subject) || handle_is_none(camera_entity) || renderer_mesh(subject) == NULL
        || !lab_orbit_fit(&orbit, camera_entity, r3d_lit_mesh_bounding_sphere(renderer_mesh(subject)),
                          &CAPYBARA_FRAME)) {
        fail("no capybara or camera");
        return false;
    }
    subject_entity = subject;
    return true;
}

static void
scene_capybara_enter(const lab_scene_t* self) {
    (void)self;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    capybara = lab_scene_load(TAG, failure, sizeof failure, CAPYBARA_SCENE, "capybara");
    if (handle_is_none(capybara)) {
        return;
    }
    if (!frame_subject()) {
        return;
    }
    if (!lab_clips_init(&choice, lab_reply_tag(&render_lab), capybara, subject_entity, CAPYBARA_DEFAULT_CLIP)) {
        ESP_LOGW(TAG, "clip picker allocation failed");
    }
    (void)camera_activate(entity_find(capybara, name_hash(CAPYBARA_CAMERA)));
    render_lab_apply_raster_settings();
    (void)render_lab_apply_camera_settings(camera_entity);
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static void
scene_capybara_exit(void) {
    lab_clips_destroy(&choice);
    scene_release(capybara);
    capybara = ENTITY_NONE;
    subject_entity = ENTITY_NONE;
}

/* The finger turns the camera before the shell draws this frame. */
static void
scene_capybara_steer(uint32_t dt_ms, const input_t* input) {
    if (handle_is_none(capybara) || failure[0] != '\0') {
        return;
    }
    lab_orbit_steer(&orbit, dt_ms, input, shell_home_edge(), lab_view_orbit_ms(&render_lab_view_tunables));
}

/* Every frame already redraws the whole screen. */

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_capybara_frame(uint32_t dt_ms) {
    (void)dt_ms;
    render_lab_apply_raster_settings();
    if (!handle_is_none(capybara)) {
        (void)render_lab_apply_camera_settings(camera_entity);
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static inline const char*
capybara_status(void) {
    static char buf[LAB_STATUS_LEN];
    if (failure[0] != '\0') {
        return failure;
    }
    /* Yaw/pitch too, so a screenshot says where it was taken from. */
    if (snprintf(buf, sizeof buf, "%s%d tris %d/%d deg", render_lab_dynres_size(),
                 render_context_frame(render_context_main()).stats.triangles,
                 (int)lroundf(orbit.motion.at.yaw * MATH_DEGREES_PER_RADIAN),
                 (int)lroundf(orbit.motion.at.pitch * MATH_DEGREES_PER_RADIAN))
        < 0) {
        buf[0] = '\0';
    }
    return buf;
}

/* "view <name>" holds the orbit still on a named view. */
static inline bool
view_reply(const char* args, console_reply_fn reply) {
    const char* name;
    if (!console_word_match(args, "view", &name)) {
        return false;
    }
    if (handle_is_none(capybara) || failure[0] != '\0') {
        reply("RENDER_ERR no capybara");
        return true;
    }
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        if (console_name_cmp(name, views[i].name) == 0) {
            lab_orbit_hold(&orbit, views[i].yaw, views[i].closest);
            reply("RENDER view ok");
            reply("RENDER_END");
            return true;
        }
    }
    reply("RENDER_ERR unknown view");
    return true;
}

static inline bool
scene_capybara_console(const char* args, console_reply_fn reply) {
    return view_reply(args, reply) || lab_clips_reply(&choice, args, reply);
}

static void
scene_capybara_controls(int width) {
    lab_clips_draw(width, &choice);
}

/* The subject, galloping. */
static lab_path_t
capybara_path(void) {
    return (lab_path_t){capybara, subject_entity, CAPYBARA_GALLOP};
}

const lab_scene_t scene_capybara = {.name = "Capybara",
                                    .key = CAPYBARA_SCENE,
                                    .family = "Capybara",
                                    .enter = scene_capybara_enter,
                                    .frame = scene_capybara_frame,
                                    .exit = scene_capybara_exit,
                                    .status = LAB_SCENE_DEV(capybara_status),
                                    .steer = scene_capybara_steer,
                                    .controls = scene_capybara_controls,
                                    .console = LAB_SCENE_DEV(scene_capybara_console),
                                    .path = capybara_path};
