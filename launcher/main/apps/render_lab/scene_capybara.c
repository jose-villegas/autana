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
#include "apps/render_lab/render_lab_clips.h"
#include "console/console_words.h"
#include "core/tune.h"
#include "display/display.h"
#include "draw/draw.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "input/mapping/orbit_motion_touch.h"
#include "kinematics/orbit/orbit_motion.h"
#include "math/scalar/math_const.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_dynres.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "ui/ui.h"
#if CONFIG_LAUNCHER_SELFTEST
#include "apps/render_lab/tests/render_lab_test.h"
#include "image/pixel/rgb565.h"
#endif

static const char* TAG = "capybara";

/* The scene's id and pack, its camera, and the entity the camera circles. */
#define CAPYBARA_CAMERA         "camera"
#define CAPYBARA_DEFAULT_CLIP   "idle"

/* Where the view starts: three-quarters from the front, a little above. */
#define CAPYBARA_HOME_YAW       (135.0F * MATH_RADIANS_PER_DEGREE)
#define CAPYBARA_HOME_PITCH     (15.0F * MATH_RADIANS_PER_DEGREE)

/* The yaw that looks the capybara in the face: the model faces -z. */
#define CAPYBARA_FRONT_YAW      (180.0F * MATH_RADIANS_PER_DEGREE)

/* The lowest the eye goes: a little above level, so the meadow stays below it. */
#define CAPYBARA_PITCH_MIN      (5.0F * MATH_RADIANS_PER_DEGREE)

/* How far a zoom reaches, as multiples of the distance that fits the
 * sphere: in to half of it (the model about twice as large), out to three
 * times. Never closer than this many near planes outside the sphere, so the
 * near plane cannot cut into the model. */
#define CAPYBARA_ZOOM_IN        0.5F
#define CAPYBARA_ZOOM_OUT       3.0F
#define CAPYBARA_NEAR_CLEARANCE 2.0F

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

static scene_t* capybara;
static scene_entity_t camera_entity;
static scene_entity_t subject_entity;
static render_lab_clips_t choice;
static orbit_motion_t orbit;
static orbit_motion_touch_t touch;

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[48];

static void
fail(const char* what) {
    render_lab_scene_fail(TAG, failure, sizeof failure, what);
}

static void
place_camera(void) {
    const transformf_t pose = orbit_motion_pose(&orbit);
    scene_entity_set_local(capybara, camera_entity, pose);
}

/* Home and limits from the subject's sphere and the scene camera's lens. */
static bool
frame_subject(void) {
    const name_hash_t camera = name_hash(CAPYBARA_CAMERA);
    const scene_entity_t subject = scene_find(capybara, name_hash(CAPYBARA_SUBJECT));
    camera_entity = scene_find(capybara, camera);
    const camera_t* lens = scene_camera_lens(capybara, camera);
    if (subject == SCENE_ENTITY_NONE || camera_entity == SCENE_ENTITY_NONE || lens == NULL
        || scene_entity_mesh(capybara, subject) == NULL) {
        fail("no capybara or camera");
        return false;
    }
    const spheref_t sphere = r3d_lit_mesh_bounding_sphere(scene_entity_mesh(capybara, subject));
    const float fit = orbit_motion_fit_distance(sphere.radius, lens->half_fov_short_tan, ORBIT_MOTION_FRAME_PADDING);
    const float closest = fmaxf(fit * CAPYBARA_ZOOM_IN, sphere.radius + (CAPYBARA_NEAR_CLEARANCE * lens->near_z));
    orbit_motion_init(&orbit, sphere.centre, (orbit_motion_state_t){CAPYBARA_HOME_YAW, CAPYBARA_HOME_PITCH, logf(fit)},
                      (orbit_motion_limits_t){CAPYBARA_PITCH_MIN, ORBIT_MOTION_PITCH_LIMIT, logf(closest),
                                              logf(fit * CAPYBARA_ZOOM_OUT)});
    orbit_motion_touch_init(&touch);
    subject_entity = subject;
    return true;
}

static void
scene_capybara_enter(const render_lab_scene_t* self) {
    (void)self;
    draw_clear(RGB565BE(UI_BACKGROUND));
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
    if (!render_lab_clips_init(&choice, capybara, subject_entity, CAPYBARA_DEFAULT_CLIP)) {
        ESP_LOGW(TAG, "clip picker allocation failed");
    }
    place_camera();
    (void)scene_activate(capybara, name_hash(CAPYBARA_CAMERA));
    render_lab_apply_raster_settings();
    (void)render_lab_apply_camera_settings(capybara, camera_entity);
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static void
scene_capybara_exit(void) {
    render_lab_clips_destroy(&choice);
    scene_unload(capybara);
    capybara = NULL;
    subject_entity = SCENE_ENTITY_NONE;
}

/* The finger turns the camera before the shell draws this frame. */
static void
scene_capybara_steer(uint32_t dt_ms, const input_t* input) {
    if (capybara == NULL || failure[0] != '\0') {
        return;
    }
    const viewport_t panel = {GFX_WIDTH, GFX_HEIGHT, display_quarter_now()};
    const orbit_motion_input_t steering = orbit_motion_touch_step(&touch, input, dt_ms, panel, shell_home_edge());
    orbit_motion_update(&orbit, &steering, (float)dt_ms / (float)MATH_MS_PER_S);
    if (render_lab_orbit_ms() > 0) {
        orbit.at.yaw = fmodf(orbit.at.yaw + (MATH_TAU * (float)dt_ms / (float)render_lab_orbit_ms()), MATH_TAU);
    }
    place_camera();
}

/* Every frame already redraws the whole screen. */

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_capybara_frame(uint32_t dt_ms) {
    (void)dt_ms;
    render_lab_apply_raster_settings();
    if (capybara != NULL) {
        (void)render_lab_apply_camera_settings(capybara, camera_entity);
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static inline const char*
capybara_status(void) {
    static char buf[RENDER_LAB_STATUS_LEN];
    if (failure[0] != '\0') {
        return failure;
    }
    /* Yaw/pitch too, so a screenshot says where it was taken from. */
    if (snprintf(buf, sizeof buf, "%s%d tris %d/%d deg", render_lab_dynres_size(),
                 render_context_frame(render_context_main()).stats.triangles,
                 (int)lroundf(orbit.at.yaw * MATH_DEGREES_PER_RADIAN),
                 (int)lroundf(orbit.at.pitch * MATH_DEGREES_PER_RADIAN))
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
    if (capybara == NULL || failure[0] != '\0') {
        reply("RENDER_ERR no capybara");
        return true;
    }
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        if (console_name_cmp(name, views[i].name) == 0) {
            orbit.at = (orbit_motion_state_t){views[i].yaw, orbit.home.pitch,
                                              views[i].closest ? orbit.limits.log_dist_min : orbit.home.log_dist};
            orbit.velocity = (orbit_motion_state_t){0};
            orbit.resetting = false;
            place_camera();
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
    return view_reply(args, reply) || render_lab_clips_reply(&choice, args, reply);
}

static void
scene_capybara_controls(int width) {
    render_lab_clips_draw(width, &choice);
}

/* The subject, galloping. */
static render_lab_path_t
capybara_path(void) {
    return (render_lab_path_t){capybara, subject_entity, CAPYBARA_GALLOP};
}

const render_lab_scene_t scene_capybara = {.name = "Capybara",
                                           .key = CAPYBARA_SCENE,
                                           .family = "Capybara",
                                           .enter = scene_capybara_enter,
                                           .frame = scene_capybara_frame,
                                           .exit = scene_capybara_exit,
                                           .status = RENDER_LAB_SCENE_DEV(capybara_status),
                                           .steer = scene_capybara_steer,
                                           .controls = scene_capybara_controls,
                                           .console = RENDER_LAB_SCENE_DEV(scene_capybara_console),
                                           .path = capybara_path};
