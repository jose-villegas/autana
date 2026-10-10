/*
 * scene_capybara: the capybara on its meadow, orbited by one finger.
 *
 * The camera circles the capybara's bounding sphere, measured from its mesh
 * when the scene loads, at the distance that fits the sphere from every
 * angle (orbit_motion.h). A drag turns it, a double tap brings it home, and
 * touches in the shell's edge strips are left alone (orbit_motion_touch.h). The
 * shell draws the scene through the camera; light is baked per vertex.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app/app.h"
#include "display/display.h"
#include "esp_log.h"
#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "input/orbit_motion_touch.h"
#include "math/motion/orbit_motion.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "services/tune.h"

static const char* TAG = "capybara";

/* The scene's id and pack, its camera, and the entity the camera circles. */
#define CAPYBARA_SCENE          "capybara"
#define CAPYBARA_CAMERA         "camera"
#define CAPYBARA_SUBJECT        "capybara"

/* Where the view starts: three-quarters from the front, a little above. */
#define CAPYBARA_HOME_YAW       (MATH_PI * 135.0F / 180.0F)
#define CAPYBARA_HOME_PITCH     (MATH_PI * 15.0F / 180.0F)

/* The lowest the eye goes: a little above level, so the meadow stays below it. */
#define CAPYBARA_PITCH_MIN      (MATH_PI * 5.0F / 180.0F)

/* How far a zoom reaches, as multiples of the distance that fits the
 * sphere: in to half of it (the model about twice as large), out to three
 * times. Never closer than this many near planes outside the sphere, so the
 * near plane cannot cut into the model. */
#define CAPYBARA_ZOOM_IN        0.5F
#define CAPYBARA_ZOOM_OUT       3.0F
#define CAPYBARA_NEAR_CLEARANCE 2.0F

#define MS_PER_SECOND           1000.0F
#define DEGREES_PER_RADIAN      (180.0F / MATH_PI)

static scene_t* capybara;
static scene_entity_t camera_entity;
static orbit_motion_t orbit;
static orbit_motion_touch_t touch;

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[48];

static void
fail(const char* what) {
    ESP_LOGE(TAG, "%s; the scene stays blank", what);
    if (snprintf(failure, sizeof failure, "%s", what) < 0) {
        failure[0] = '\0';
    }
}

static void
place_camera(void) {
    const transformf_t pose = orbit_motion_pose(&orbit);
    const scene_transform_t placement = r3d_scene_camera_placement(&pose);
    scene_entity_set_transform(capybara, camera_entity, &placement);
}

/* Home and limits from the subject's sphere and the scene camera's lens. */
static bool
frame_subject(void) {
    const scene_entity_t subject = scene_find(capybara, CAPYBARA_SUBJECT);
    camera_entity = scene_find(capybara, CAPYBARA_CAMERA);
    const r3d_scene_camera_t* lens = scene_camera_lens(capybara, CAPYBARA_CAMERA);
    if (subject == SCENE_ENTITY_NONE || camera_entity == SCENE_ENTITY_NONE || lens == NULL
        || scene_entity_mesh(capybara, subject) == NULL) {
        fail("no capybara or camera");
        return false;
    }
    vec3f_t centre;
    float radius;
    r3d_lit_mesh_bounding_sphere(scene_entity_mesh(capybara, subject), &centre, &radius);
    const float fit = orbit_motion_fit_distance(radius, lens->half_fov_short_tan, ORBIT_MOTION_FRAME_PADDING);
    const float closest = fmaxf(fit * CAPYBARA_ZOOM_IN, radius + (CAPYBARA_NEAR_CLEARANCE * lens->near_z));
    orbit_motion_init(&orbit, centre, (orbit_motion_state_t){CAPYBARA_HOME_YAW, CAPYBARA_HOME_PITCH, logf(fit)},
                      (orbit_motion_limits_t){CAPYBARA_PITCH_MIN, ORBIT_MOTION_PITCH_LIMIT, logf(closest),
                                              logf(fit * CAPYBARA_ZOOM_OUT)});
    orbit_motion_touch_init(&touch);
    return true;
}

static void
scene_capybara_enter(void) {
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));
    failure[0] = '\0';
    scene_failure_t why;
    capybara = scene_load(CAPYBARA_SCENE, &why);
    if (capybara == NULL) {
        fail(why.asset == ASSET_ERR_NOT_FOUND || why.asset == ASSET_ERR_NO_PACK ? "no capybara pack: flash it"
                                                                                : "capybara did not load");
        return;
    }
    if (!frame_subject()) {
        return;
    }
    place_camera();
    (void)scene_activate(capybara, CAPYBARA_CAMERA);
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    render_context_set_scale(render_context_main(), 10000 / render_lab_scale());
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static void
scene_capybara_exit(void) {
    scene_unload(capybara);
    capybara = NULL;
}

/* The finger turns the camera before the shell draws this frame. */
static void
scene_capybara_steer(uint32_t dt_ms, const input_t* input) {
    if (capybara == NULL || failure[0] != '\0') {
        return;
    }
    const viewport_t panel = {GFX_WIDTH, GFX_HEIGHT, display_quarter_now()};
    const orbit_motion_input_t steering = orbit_motion_touch_step(&touch, input, dt_ms, panel, shell_home_edge());
    orbit_motion_update(&orbit, &steering, (float)dt_ms / MS_PER_SECOND);
    place_camera();
}

/* Every frame already redraws the whole screen. */

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_capybara_frame(uint32_t dt_ms) {
    (void)dt_ms;
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static const char*
capybara_status(void) {
    static char buf[48];
    if (failure[0] != '\0') {
        return failure;
    }
    /* Yaw/pitch too, so a screenshot says where it was taken from. */
    if (snprintf(buf, sizeof buf, "%d tris %d/%d deg", render_context_frame(render_context_main()).stats.triangles,
                 (int)lroundf(orbit.at.yaw * DEGREES_PER_RADIAN), (int)lroundf(orbit.at.pitch * DEGREES_PER_RADIAN))
        < 0) {
        buf[0] = '\0';
    }
    return buf;
}

const render_lab_scene_t scene_capybara = {.name = "Capybara",
                                           .key = "capybara",
                                           .enter = scene_capybara_enter,
                                           .frame = scene_capybara_frame,
                                           .exit = scene_capybara_exit,
                                           .status = capybara_status,
                                           .steer = scene_capybara_steer};
