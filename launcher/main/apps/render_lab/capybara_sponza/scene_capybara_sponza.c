/*
 * scene_capybara_sponza: the capybara's run down Sponza's atrium. Sponza
 * (its fitted full bake) and the capybara load as two scenes, both drawn
 * through Sponza's camera. The capybara idles between the first pair of
 * columns at the lion end, gallops down the centre line and comes to rest
 * between the last pair (a motion profile), its clip and rate from its
 * ground speed (gait). A third-person camera sees it from a front
 * three-quarter while it idles, turns slowly round toward its back as it
 * runs, never all the way, and back as it comes to rest. Everything is a
 * function of the path's time, so a frame profile seeks it.
 */
#include "apps/render_lab/capybara_sponza/scene_capybara_sponza.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "kinematics/profile/motion_profile.h"
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
#define CAPYBARA_MEADOW     "meadow" /* the capybara pack's ground, hidden in Sponza */

/* Sponza is in centimetres, the capybara in metres. */
#define RUN_SCALE           100.0F
/* The atrium's centre line (z) and floor (y), and the x of its first and
 * last pairs of columns from the lion's west wall (Sponza's source mesh). */
#define RUN_CENTRE_Z        (-34.0F)
#define RUN_FLOOR_Y         0.0F
#define RUN_START_X         (-616.0F)
#define RUN_END_X           487.0F
/* Still for 3 s at each end, 2.5 s easing to full gallop and as long back. */
#define RUN_REST_S          3.0F
#define RUN_RAMP_S          2.5F
/* The capybara faces -z; a quarter turn back about y faces it +x, down the run. */
#define RUN_HEADING         (-0.25F * MATH_TAU)

/* The third-person camera: this far from the capybara and this high, looking
 * at a point this high on it; inside the columns (225 cm off the centre line)
 * when it is at the capybara's side. It stands 15 degrees round from the
 * face at both ends and 135 mid-run, short of the back. */
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

static entity_t sponza;
static entity_t capybara;
static entity_t runner; /* the capybara's skinned entity */
static entity_t camera;
static motion_profile_t run;
static gait_step_t steps[RUN_GAIT_STEPS_MAX];
static gait_t gait;
static gait_pick_t applied; /* what the runner's animator was last given */
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
    return !handle_is_none(capybara) && failure[0] == '\0';
}

/* The gait from the runner's clips up to the gallop, slowest first, in the
 * capybara's own metres per second; the run peaks at the gallop's speed. */
static bool
build_gait(void) {
    const int clips = animator_clip_count(runner);
    float gallop = -1.0F;
    for (int i = 0; i < clips; i++) {
        if (strcmp(animator_clip_name(runner, i), CAPYBARA_GALLOP) == 0) {
            gallop = animator_clip_ground_mps(runner, i);
        }
    }
    if (gallop <= 0.0F) {
        return false;
    }
    int count = 0;
    for (int i = 0; i < clips && count < RUN_GAIT_STEPS_MAX; i++) {
        const float ground = animator_clip_ground_mps(runner, i);
        if (ground <= gallop) {
            steps[count++] = (gait_step_t){.clip = i, .ground_mps = ground};
        }
    }
    gait_sort(steps, count);
    gait = (gait_t){.steps = steps,
                    .count = count,
                    .rate_min = RUN_GAIT_RATE_MIN,
                    .rate_max = RUN_GAIT_RATE_MAX,
                    .band = RUN_GAIT_BAND_MPS,
                    .stop_mps = RUN_GAIT_STOP_MPS,
                    .blend_ms = CAPYBARA_CLIP_BLEND_MS};
    run = (motion_profile_t){.start = RUN_START_X,
                             .end = RUN_END_X,
                             .peak_speed = gallop * RUN_SCALE,
                             .rest_s = RUN_REST_S,
                             .ramp_s = RUN_RAMP_S};
    return true;
}

/* The run `t_ms` from its start. */
static motion_profile_at_t
run_at(uint32_t t_ms) {
    return motion_profile_at(&run, (float)t_ms / (float)MATH_MS_PER_S);
}

/* The camera's angle round the capybara from its face (0) toward its back
 * (pi) where the run is at `at`: RUN_CAMERA_FROM at both ends and
 * RUN_CAMERA_REACH half way, eased between (sin^2 of the share run). */
static float
camera_angle(float at) {
    const float s = sinf(MATH_PI * (at - run.start) / (run.end - run.start));
    return RUN_CAMERA_FROM + ((RUN_CAMERA_REACH - RUN_CAMERA_FROM) * s * s);
}

/* The capybara where the run is at `at`, and the camera round it. */
static void
place(motion_profile_at_t at) {
    const transformf_t body = {.position = {at.at, RUN_FLOOR_Y, RUN_CENTRE_Z},
                               .rotation = quatf_from_axis_angle(vec3f_unit_y(), RUN_HEADING),
                               .scale = {RUN_SCALE, RUN_SCALE, RUN_SCALE}};
    transform_set_local(capybara, body);
    /* Ahead of it at angle 0, toward behind it at pi, round its +z side. */
    const float angle = camera_angle(at.at);
    transformf_t eye = transform_local(camera);
    eye.position = (vec3f_t){at.at + (RUN_CAMERA_DISTANCE * cosf(angle)), RUN_FLOOR_Y + RUN_CAMERA_HEIGHT,
                             RUN_CENTRE_Z + (RUN_CAMERA_DISTANCE * sinf(angle))};
    transformf_look_at(&eye, (vec3f_t){at.at, RUN_FLOOR_Y + RUN_CAMERA_LOOK_Y, RUN_CENTRE_Z}, vec3f_unit_y());
    transform_set_local(camera, eye);
}

/* The gait's pick `t_ms` into the run, in the capybara's own units. A clip
 * takes the gait's blend to come in, so the pick reads the speed that far
 * ahead: the walk is in before the body moves, and the idle is in by the
 * time it stops. */
static gait_pick_t
pick_at(gait_pick_t current, uint32_t t_ms) {
    const float ahead = run_at(t_ms + gait.blend_ms).speed / RUN_SCALE;
    return gait_pick(&gait, current, ahead);
}

static void
advance(uint32_t dt_ms) {
    path_ms += dt_ms;
    const motion_profile_at_t at = run_at(path_ms);
    place(at);
    const gait_pick_t pick = pick_at(applied, path_ms);
    gait_apply(runner, &gait, applied, pick);
    applied = pick;
}

/* The run at t_ms, held: the runner in its clip at the time its rates have
 * carried it since its step began, cut in rather than faded. */
static bool
seek_run(uint32_t t_ms) {
    if (!ready()) {
        return false;
    }
    gait_pick_t pick = pick_at((gait_pick_t){-1, 1.0F}, 0);
    float clip_s = 0.0F;
    for (uint32_t t = RUN_SEEK_STEP_MS; t <= t_ms; t += RUN_SEEK_STEP_MS) {
        const gait_pick_t next = pick_at(pick, t);
        const float dt = (float)RUN_SEEK_STEP_MS / (float)MATH_MS_PER_S;
        clip_s = next.step == pick.step ? clip_s + (next.rate * dt) : 0.0F;
        pick = next;
    }
    path_ms = t_ms;
    place(run_at(t_ms));
    const int clip = gait.steps[pick.step].clip;
    const uint32_t duration_ms = animator_clip_duration_ms(runner, clip);
    (void)animator_play(runner, clip, CAPYBARA_CLIP_CUT_MS);
    (void)animator_set_speed(runner, ANIMATOR_CURRENT, pick.rate);
    if (duration_ms > 0) {
        (void)animator_seek(runner, ANIMATOR_CURRENT, (uint32_t)lroundf(clip_s * (float)MATH_MS_PER_S) % duration_ms);
    }
    (void)animator_set_paused(runner, ANIMATOR_CURRENT, true);
    applied = pick;
    held = true;
    return true;
}

/* Lets a held run go on from where it was sought. */
static void
play_run(void) {
    held = false;
    (void)animator_set_paused(runner, ANIMATOR_CURRENT, false);
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

/* Sponza with its fitted full bake alone, its camera held still for the run to place. */
static bool
load_sponza(void) {
    const scene_baked_t* family = &baked_sponza;
    sponza = lab_scene_load(TAG, failure, sizeof failure, family->scene, family->scene);
    if (handle_is_none(sponza)) {
        return false;
    }
    for (int i = 0; i < (int)SCENE_BAKED_VARIANT_COUNT; i++) {
        entity_set_enabled(entity_find(sponza, name_hash(family->bakes[i])), i == (int)SCENE_BAKED_FITTED_FULL);
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

/* The capybara without its meadow, its gait built from its clips. */
static bool
load_capybara(void) {
    capybara = lab_scene_load(TAG, failure, sizeof failure, CAPYBARA_SCENE, CAPYBARA_SCENE);
    if (handle_is_none(capybara)) {
        return false;
    }
    entity_set_enabled(entity_find(capybara, name_hash(CAPYBARA_MEADOW)), false);
    runner = entity_find(capybara, name_hash(CAPYBARA_SUBJECT));
    if (handle_is_none(runner) || !build_gait()) {
        fail("no capybara gallop");
        return false;
    }
    return true;
}

static void
scene_capybara_sponza_enter(const lab_scene_t* self) {
    (void)self;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    if (!load_sponza() || !load_capybara()) {
        return;
    }
    path_ms = 0;
    held = false;
    applied = (gait_pick_t){-1, 1.0F};
    advance(0);
    (void)camera_activate(camera);
    apply_settings();
}

static void
scene_capybara_sponza_exit(void) {
    scene_release(capybara);
    scene_release(sponza);
    capybara = ENTITY_NONE;
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
    return render_lab_copies_status(failure, 1);
}

static lab_path_t
scene_capybara_sponza_path(void) {
    return (lab_path_t){.scene = capybara,
                        .entity = runner,
                        .seek = seek_run,
                        .play = play_run,
                        .duration_ms =
                            ready() ? (uint32_t)lroundf(motion_profile_duration_s(&run) * (float)MATH_MS_PER_S) : 0};
}

/* The capybara moves on its own: frame generation draws it again over a
 * warp of the picture drawn without it. */
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
