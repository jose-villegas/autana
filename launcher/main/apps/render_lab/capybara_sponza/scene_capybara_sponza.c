/*
 * scene_capybara_sponza: two capybaras' run down Sponza's atrium. Sponza (its
 * fitted full bake) and the capybaras load as scenes, all drawn through
 * Sponza's camera. Each runner idles between the first pair of columns at the
 * lion end, gallops down its own lane near the sunlit +z columns and comes to
 * rest between the last pair (a motion profile scaled by its size), its clip
 * and rate from its ground speed (gait). A third-person camera on the hero's
 * -z side, looking toward the sunlit columns, sees it from a front
 * three-quarter while it idles, turns slowly round toward its back as it
 * runs, never all the way, and back as it comes to rest. Everything is a
 * function of the path's time, so a frame profile seeks it.
 */
#include "apps/render_lab/capybara_sponza/scene_capybara_sponza.h"

#include <math.h>
#include <stdbool.h>
#include "esp_log.h"

#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/capybara_sponza/capybara_run.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/scene_baked.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "lab/lab_scene.h"
#include "math/linear/quatf.h"
#include "math/scalar/math_const.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/animator/gait.h"
#include "scene/camera/camera.h"
#include "scene/entity/entity.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"
#include "ui/ui.h"

static const char* TAG = "capybara_sponza";

#define RUN_KEY             "capybara-sponza"
/* Sponza is in centimetres, the capybara in metres. The lanes run inside the
 * sunlit strip of floor along the +z columns, on the atrium floor (y). */
#define RUN_HERO_LANE_Z     155.0F
#define RUN_SMALL_LANE_Z    80.0F
#define RUN_FLOOR_Y         0.0F
/* The capybara faces -z; a quarter turn back about y faces it +x, down the run. */
#define RUN_HEADING         (-0.25F * MATH_TAU)

/* The third-person camera: this far from the capybara and this high, looking
 * at a point this high on it; on its -z side, so it looks toward the sunlit
 * +z columns, and well inside the -z ones (z -259) when it is at the
 * capybara's side. It stands 15 degrees round from the face at both ends
 * and 135 mid-run, short of the back. */
#define RUN_CAMERA_DISTANCE 220.0F
#define RUN_CAMERA_HEIGHT   100.0F
#define RUN_CAMERA_FROM     (MATH_PI / 12.0F)
#define RUN_CAMERA_REACH    (0.75F * MATH_PI)
#define RUN_CAMERA_LOOK_Y   35.0F

/* A clip plays at 0.75 to 1.4 of its speed before the next fits better; a
 * held clip reaches 0.1 m/s past its range (as the herd's). */
#define RUN_GAIT_RATE_MIN   0.75F
#define RUN_GAIT_RATE_MAX   1.4F
#define RUN_GAIT_BAND_MPS   0.1F
/* The run is scripted, so it stands at exactly 0: any motion walks. */
#define RUN_GAIT_STOP_MPS   0.0F
#define RUN_GAIT_STEPS_MAX  8
/* A seek replays the gait in steps this long to find the clip's time. */
#define RUN_SEEK_STEP_MS    10U

typedef struct {
    float scale, lane_z;
    entity_t root, entity;
    motion_profile_t run;
    gait_pick_t applied;
} runner_t;

enum { HERO, SMALL, RUNNER_COUNT };

static runner_t runners[RUNNER_COUNT] = {
    [HERO] = {.scale = 1.0F, .lane_z = RUN_HERO_LANE_Z},
    [SMALL] = {.scale = 0.5F, .lane_z = RUN_SMALL_LANE_Z},
};
static entity_t sponza;
static entity_t camera;
static gait_step_t steps[RUN_GAIT_STEPS_MAX];
static gait_t gait;
static uint32_t path_ms;
static bool held; /* a seek holds the run until "pose play" */

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[LAB_STATUS_LEN];

static void
fail(const char* what) {
    lab_scene_fail(TAG, failure, sizeof failure, what);
}

static bool
ready(void) {
    return !handle_is_none(runners[HERO].root) && failure[0] == '\0';
}

/* The gait from the runner's clips up to the gallop, slowest first, in the
 * capybara's own metres per second; the run peaks at the gallop's speed. */
static bool
build_gait(void) {
    const entity_t runner = runners[HERO].entity;
    const float gallop = capybara_run_gallop_mps(runner);
    if (gallop <= 0.0F) {
        return false;
    }
    const int count = gait_steps_from_clips(runner, 1.0F, gallop, steps, RUN_GAIT_STEPS_MAX);
    gait = (gait_t){.steps = steps,
                    .count = count,
                    .rate_min = RUN_GAIT_RATE_MIN,
                    .rate_max = RUN_GAIT_RATE_MAX,
                    .band = RUN_GAIT_BAND_MPS,
                    .stop_mps = RUN_GAIT_STOP_MPS,
                    .blend_ms = CAPYBARA_CLIP_BLEND_MS};
    for (int i = 0; i < RUNNER_COUNT; i++) {
        runners[i].run = capybara_run_profile(gallop, runners[i].scale);
    }
    return true;
}

/* The run `t_ms` from its start. */
static motion_profile_at_t
run_at(const runner_t* runner, uint32_t t_ms) {
    return motion_profile_at(&runner->run, (float)t_ms / (float)MATH_MS_PER_S);
}

/* The camera's angle round the capybara from its face (0) toward its back
 * (pi) where the run is at `at`: RUN_CAMERA_FROM at both ends and
 * RUN_CAMERA_REACH half way, eased between (sin^2 of the share run). */
static float
camera_angle(float at) {
    const motion_profile_t* run = &runners[HERO].run;
    const float s = sinf(MATH_PI * (at - run->start) / (run->end - run->start));
    return RUN_CAMERA_FROM + ((RUN_CAMERA_REACH - RUN_CAMERA_FROM) * s * s);
}

static void
place_runner(const runner_t* runner, motion_profile_at_t at) {
    const float world_scale = runner->scale * CAPYBARA_RUN_CM_PER_M;
    const transformf_t body = {.position = {at.at, RUN_FLOOR_Y, runner->lane_z},
                               .rotation = quatf_from_axis_angle(vec3f_unit_y(), RUN_HEADING),
                               .scale = {world_scale, world_scale, world_scale}};
    transform_set_local(runner->root, body);
}

static void
place_camera(float at) {
    const float angle = camera_angle(at);
    transformf_t eye = transform_local(camera);
    eye.position = (vec3f_t){at + (RUN_CAMERA_DISTANCE * cosf(angle)), RUN_FLOOR_Y + RUN_CAMERA_HEIGHT,
                             RUN_HERO_LANE_Z - (RUN_CAMERA_DISTANCE * sinf(angle))};
    transformf_look_at(&eye, (vec3f_t){at, RUN_FLOOR_Y + RUN_CAMERA_LOOK_Y, RUN_HERO_LANE_Z}, vec3f_unit_y());
    transform_set_local(camera, eye);
}

/* The gait's pick `t_ms` into the run, in the capybara's own units. A clip
 * takes the gait's blend to come in, so the pick reads the speed that far
 * ahead: the walk is in before the body moves, and the idle is in by the
 * time it stops. */
static gait_pick_t
pick_at(const runner_t* runner, gait_pick_t current, uint32_t t_ms) {
    const float ahead = run_at(runner, t_ms + gait.blend_ms).speed / (runner->scale * CAPYBARA_RUN_CM_PER_M);
    return gait_pick(&gait, current, ahead);
}

static void
advance(uint32_t dt_ms) {
    path_ms += dt_ms;
    for (int i = 0; i < RUNNER_COUNT; i++) {
        runner_t* runner = &runners[i];
        place_runner(runner, run_at(runner, path_ms));
        const gait_pick_t pick = pick_at(runner, runner->applied, path_ms);
        gait_apply(runner->entity, &gait, runner->applied, pick);
        runner->applied = pick;
    }
    place_camera(run_at(&runners[HERO], path_ms).at);
}

/* Replay clip picks and rates to restore the runner's clip time as well as its position. */
static void
seek_runner(runner_t* runner, uint32_t t_ms) {
    gait_pick_t pick = pick_at(runner, (gait_pick_t){-1, 1.0F}, 0);
    float clip_s = 0.0F;
    for (uint32_t t = RUN_SEEK_STEP_MS; t <= t_ms; t += RUN_SEEK_STEP_MS) {
        const gait_pick_t next = pick_at(runner, pick, t);
        const float dt = (float)RUN_SEEK_STEP_MS / (float)MATH_MS_PER_S;
        clip_s = next.step == pick.step ? clip_s + (next.rate * dt) : 0.0F;
        pick = next;
    }
    place_runner(runner, run_at(runner, t_ms));
    gait_hold(runner->entity, &gait, pick, clip_s, CAPYBARA_CLIP_CUT_MS);
    runner->applied = pick;
}

static bool
seek_run(uint32_t t_ms) {
    if (!ready()) {
        return false;
    }
    for (int i = 0; i < RUNNER_COUNT; i++) {
        seek_runner(&runners[i], t_ms);
    }
    path_ms = t_ms;
    place_camera(run_at(&runners[HERO], t_ms).at);
    held = true;
    return true;
}

/* Lets a held run go on from where it was sought. */
static void
play_run(void) {
    held = false;
    for (int i = 0; i < RUNNER_COUNT; i++) {
        (void)animator_set_paused(runners[i].entity, ANIMATOR_CURRENT, false);
    }
}

static void
apply_settings(void) {
    render_lab_apply_raster_settings();
    if (!handle_is_none(sponza)) {
        (void)render_lab_apply_camera_settings(camera);
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

/* Sponza with its bake, its camera held still for the run to place. */
static bool
load_sponza(void) {
    const scene_baked_t* family = &baked_sponza;
    sponza = lab_scene_load(TAG, failure, sizeof failure, family->scene, family->scene);
    if (handle_is_none(sponza)) {
        return false;
    }
    if (handle_is_none(scene_baked_find(sponza, family, failure, sizeof failure))) {
        return false;
    }
    camera = entity_find(sponza, name_hash(family->camera));
    if (handle_is_none(camera)) {
        fail("no Sponza camera");
        return false;
    }
    /* Posed once now, and paused: its flight no longer moves it. */
    (void)animator_seek(camera, ANIMATOR_CURRENT, 0);
    (void)animator_set_paused(camera, ANIMATOR_CURRENT, true);
    (void)camera_set_skip_hidden(camera, family->skip_hidden);
    return true;
}

static bool
load_capybara(runner_t* runner) {
    runner->root = lab_scene_load(TAG, failure, sizeof failure, CAPYBARA_SCENE, CAPYBARA_SCENE);
    if (handle_is_none(runner->root)) {
        return false;
    }
    entity_set_enabled(entity_find(runner->root, name_hash(CAPYBARA_MEADOW)), false);
    runner->entity = entity_find(runner->root, name_hash(CAPYBARA_SUBJECT));
    if (handle_is_none(runner->entity)) {
        fail("no capybara subject");
        return false;
    }
    scene_set_contact_blob(runner->entity, RUN_FLOOR_Y, SCENE_CONTACT_BLOB_STRENGTH_DEFAULT);
    scene_set_probes(runner->root, scene_probes(sponza));
    runner->applied = (gait_pick_t){-1, 1.0F};
    return true;
}

static void
scene_capybara_sponza_enter(const lab_scene_t* self) {
    (void)self;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    if (!load_sponza()) {
        return;
    }
    for (int i = 0; i < RUNNER_COUNT; i++) {
        if (!load_capybara(&runners[i])) {
            return;
        }
    }
    if (!build_gait()) {
        fail("no capybara gallop");
        return;
    }
    path_ms = 0;
    held = false;
    advance(0);
    (void)camera_activate(camera);
    apply_settings();
}

static void
scene_capybara_sponza_exit(void) {
    for (int i = RUNNER_COUNT - 1; i >= 0; i--) {
        scene_release(runners[i].root);
        runners[i].root = ENTITY_NONE;
    }
    scene_release(sponza);
    sponza = ENTITY_NONE;
}

/* The run goes on before the shell draws this frame; a held run stays put. */
static void
scene_capybara_sponza_update(uint32_t dt_ms) {
    if (ready() && !held) {
        advance(dt_ms);
    }
}

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_capybara_sponza_frame(uint32_t dt_ms) {
    (void)dt_ms;
    apply_settings();
}

static const char*
scene_capybara_sponza_status(void) {
    return render_lab_copies_status(failure, RUNNER_COUNT);
}

static lab_path_t
scene_capybara_sponza_path(void) {
    return (lab_path_t){
        .scene = runners[HERO].root,
        .entity = runners[HERO].entity,
        .seek = seek_run,
        .play = play_run,
        .duration_ms =
            ready() ? (uint32_t)lroundf(motion_profile_duration_s(&runners[SMALL].run) * (float)MATH_MS_PER_S) : 0};
}

const lab_scene_t scene_capybara_sponza = {.name = "Capybara in Sponza",
                                           .key = RUN_KEY,
                                           .family = "Capybara in Sponza",
                                           .enter = scene_capybara_sponza_enter,
                                           .frame = scene_capybara_sponza_frame,
                                           .exit = scene_capybara_sponza_exit,
                                           .update = scene_capybara_sponza_update,
                                           .status = LAB_SCENE_DEV(scene_capybara_sponza_status),
                                           .generates_frames = true,
                                           .path = scene_capybara_sponza_path};
