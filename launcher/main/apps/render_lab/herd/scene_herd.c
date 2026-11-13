/*
 * scene_herd: loads the herd pack, shows the size's first copies and hides
 * the rest of the pool, walks the shown copies round the meadow (herd_wander,
 * each clip and rate from its ground speed by gait) and orbits the camera
 * round the area they walk in (lab/lab_orbit.h); see scene_herd.h.
 */

#include "apps/render_lab/herd/scene_herd.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "app/app.h"
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/herd/herd_wander.h"
#include "apps/render_lab/locomotion/gait.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "lab/lab_orbit.h"
#include "lab/lab_scene.h"
#include "math/linear/quatf.h"
#include "math/scalar/math_const.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/entity/entity.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"
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
static lab_orbit_t orbit;
static entity_t* copies; /* the pool, in name order */
static int pool;

/* The walk: a circle round the meadow's centre, 2 m inside its 8 m half
 * width, legs at least a third of its radius long, and grazes of 1 to 4 s.
 * Steering keeps copies 1.2 m apart (a body and a half), looks a second
 * ahead and turns them at most half a turn a second. */
#define HERD_AREA_RADIUS_M   6.0F
#define HERD_LEG_MIN_M       2.0F
#define HERD_GRAZE_MIN_MS    1000
#define HERD_GRAZE_MAX_MS    4000
#define HERD_MAX_ACCEL       6.0F
#define HERD_ARRIVE_RADIUS_M 1.0F
#define HERD_SEPARATION_M    1.2F
#define HERD_HORIZON_S       1.0F
#define HERD_TURN_RATE       MATH_PI
#define HERD_HEADING_SPEED   0.05F /* m/s below which a copy keeps its heading */
/* A clip plays at 0.75 to 1.4 of its speed before the next one fits better;
 * a held clip reaches 0.1 m/s past its range. */
#define HERD_GAIT_RATE_MIN   0.75F
#define HERD_GAIT_RATE_MAX   1.4F
#define HERD_GAIT_BAND_MPS   0.1F
/* The most clips a copy's gait chooses among. */
#define HERD_GAIT_STEPS_MAX  8
#define HERD_SEED            0x4E5Du
/* The path "pose <ms>" seeks along: 18 s of walking. */
#define HERD_PATH_MS         18000U

/* The walk and its parts; config points into gait. */
static gait_step_t steps[HERD_GAIT_STEPS_MAX];
static gait_t gait;
static herd_wander_config_t config;
static herd_wander_t wander;
static gait_pick_t applied[HERD_WANDER_MAX]; /* what each copy's animator was last given */
static bool held;                            /* a seek holds the herd until "pose play" */
static bool still;                           /* scene_herd_hold(): the next enter walks none */

/* The orbit starts a little above level and the eye stays above the meadow;
 * a zoom reaches in among the copies and out to twice the fitting distance. */
static const lab_orbit_frame_t HERD_FRAME = {
    .home_pitch = 15.0F * MATH_RADIANS_PER_DEGREE,
    .pitch_min = 5.0F * MATH_RADIANS_PER_DEGREE,
    .zoom_in = 0.25F,
    .zoom_out = 2.0F,
    .near_clearance = 2.0F,
};

/* What the panel says in place of the triangle count when the scene could not load. */
static char failure[LAB_STATUS_LEN];

static void
fail(const char* what) {
    lab_scene_fail(TAG, failure, sizeof failure, what);
}

/* The pool is every copy the pack names, from 0 up to the first it lacks. */
static bool
find_pool(void) {
    copies = lab_scene_copies(loaded, HERD_COPY_FORMAT, &pool);
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

/* The walk's gait from the copies' clips, slowest first, each at the ground
 * speed its import measured times the copies' scale. */
static void
build_walk(void) {
    const transformf_t placed = transform_local(copies[0]);
    const int clips = animator_clip_count(copies[0]);
    const int count = clips < HERD_GAIT_STEPS_MAX ? clips : HERD_GAIT_STEPS_MAX;
    for (int i = 0; i < count; i++) {
        steps[i] = (gait_step_t){.clip = i, .ground_mps = animator_clip_ground_mps(copies[0], i) * placed.scale.x};
    }
    gait_sort(steps, count);
    gait = (gait_t){.steps = steps,
                    .count = count,
                    .rate_min = HERD_GAIT_RATE_MIN,
                    .rate_max = HERD_GAIT_RATE_MAX,
                    .band = HERD_GAIT_BAND_MPS,
                    .blend_ms = CAPYBARA_CLIP_BLEND_MS};
    config = (herd_wander_config_t){
        .limits = {.max_speed = count > 0 ? steps[count - 1].ground_mps : 0.0F,
                   .max_accel = HERD_MAX_ACCEL,
                   .turn_rate = HERD_TURN_RATE,
                   .arrive_radius = HERD_ARRIVE_RADIUS_M,
                   .separation_radius = HERD_SEPARATION_M,
                   .heading_speed = HERD_HEADING_SPEED,
                   .horizon_s = HERD_HORIZON_S},
        .area_radius = HERD_AREA_RADIUS_M,
        .leg_min_m = HERD_LEG_MIN_M,
        .graze_min_ms = HERD_GRAZE_MIN_MS,
        .graze_max_ms = HERD_GRAZE_MAX_MS,
        .gait = &gait,
    };
}

/* The shown copies start still where the pack places them, facing as it turns them. */
static void
start_walk(void) {
    steer2f_agent_t start[HERD_WANDER_MAX];
    for (int i = 0; i < *shown; i++) {
        const transformf_t placed = transform_local(copies[i]);
        start[i] = (steer2f_agent_t){
            {placed.position.x, placed.position.z}, vec2f_zero(), herd_wander_heading(placed.rotation)};
    }
    herd_wander_start(&wander, &config, HERD_SEED, start, *shown);
    for (int i = 0; i < *shown; i++) {
        applied[i] = (gait_pick_t){-1, 0.0F};
    }
}

/* Each shown copy where its walker is, facing its heading, on the meadow's plane. */
static void
place_walkers(void) {
    for (int i = 0; i < wander.count; i++) {
        const herd_walker_t* walker = &wander.walker[i];
        transformf_t placed = transform_local(copies[i]);
        placed.position.x = walker->agent.position.x;
        placed.position.z = walker->agent.position.y;
        placed.rotation = herd_wander_facing(walker->agent.heading);
        transform_set_local(copies[i], placed);
    }
}

/* Moves the herd on: places the copies and gives their animators any new clip and rate. */
static void
walk(uint32_t dt_ms) {
    herd_wander_advance(&wander, dt_ms);
    place_walkers();
    for (int i = 0; i < wander.count; i++) {
        gait_apply(copies[i], &gait, applied[i], wander.walker[i].gait);
        applied[i] = wander.walker[i].gait;
    }
}

/* The herd at t_ms from its start, held: every copy in its clip at the time
 * its rate has carried it, cut in rather than faded. */
static bool
seek_walk(uint32_t t_ms) {
    if (handle_is_none(loaded) || failure[0] != '\0' || still) {
        return false;
    }
    herd_wander_seek(&wander, t_ms);
    place_walkers();
    for (int i = 0; i < wander.count; i++) {
        const herd_walker_t* walker = &wander.walker[i];
        const int clip = gait.steps[walker->gait.step].clip;
        const uint32_t duration_ms = animator_clip_duration_ms(copies[i], clip);
        (void)animator_play(copies[i], clip, CAPYBARA_CLIP_CUT_MS);
        (void)animator_set_speed(copies[i], ANIMATOR_CURRENT, walker->gait.rate);
        if (duration_ms > 0) {
            (void)animator_seek(copies[i], ANIMATOR_CURRENT,
                                (uint32_t)lroundf(walker->clip_s * (float)MATH_MS_PER_S) % duration_ms);
        }
        (void)animator_set_paused(copies[i], ANIMATOR_CURRENT, true);
        applied[i] = walker->gait;
    }
    held = true;
    return true;
}

/* Lets a held herd walk on from where it was sought. */
static void
play_walk(void) {
    held = false;
    for (int i = 0; i < wander.count; i++) {
        (void)animator_set_paused(copies[i], ANIMATOR_CURRENT, false);
    }
}

void
scene_herd_hold(bool hold) {
    still = hold;
}

/* A sphere holding every copy shown: round their mean place, out to the
 * farthest, widened by a copy's own sphere. */
static spheref_t
shown_sphere(void) {
    const spheref_t copy = r3d_lit_mesh_bounding_sphere(renderer_mesh(copies[0]));
    vec3f_t centre = vec3f_zero();
    for (int i = 0; i < *shown; i++) {
        centre = vec3f_add(centre, transform_world(copies[i]).position);
    }
    centre = vec3f_scale(centre, 1.0F / (float)*shown);
    float reach = 0.0F;
    for (int i = 0; i < *shown; i++) {
        reach = fmaxf(reach, vec3f_length(vec3f_sub(transform_world(copies[i]).position, centre)));
    }
    return (spheref_t){centre, reach + vec3f_length(copy.centre) + copy.radius};
}

/* A sphere holding the area the copies walk in, widened by a copy's own
 * sphere, so the camera does not move as they do. */
static spheref_t
walk_sphere(void) {
    const spheref_t copy = r3d_lit_mesh_bounding_sphere(renderer_mesh(copies[0]));
    return (spheref_t){vec3f_zero(), HERD_AREA_RADIUS_M + vec3f_length(copy.centre) + copy.radius};
}

static void
apply_settings(void) {
    render_lab_apply_raster_settings();
    if (!handle_is_none(loaded)) {
        (void)render_lab_apply_camera_settings(orbit.camera);
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static bool
load(void) {
    loaded = lab_scene_load(TAG, failure, sizeof failure, HERD_SCENE, HERD_KEY);
    if (handle_is_none(loaded)) {
        return false;
    }
    if (!find_pool()) {
        fail("no memory for the herd");
        return false;
    }
    if (*shown > pool || renderer_mesh(copies[0]) == NULL) {
        fail("too few capybaras in the herd");
        return false;
    }
    if (!lab_orbit_fit(&orbit, entity_find(loaded, name_hash(HERD_CAMERA)), still ? shown_sphere() : walk_sphere(),
                       &HERD_FRAME)) {
        fail("no herd camera");
        return false;
    }
    return true;
}

static void
scene_herd_enter(const lab_scene_t* self) {
    shown = self->data;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    if (!load()) {
        return;
    }
    show_copies();
    held = false;
    wander.count = 0;
    if (!still) {
        build_walk();
        start_walk();
        walk(0);
    }
    (void)camera_activate(orbit.camera);
    apply_settings();
}

static void
scene_herd_exit(void) {
    scene_release(loaded);
    loaded = ENTITY_NONE;
    copies = NULL;
    pool = 0;
    wander.count = 0;
}

/* The finger turns the camera before the shell draws this frame. */
static void
scene_herd_steer(uint32_t dt_ms, const input_t* input) {
    if (!handle_is_none(loaded) && failure[0] == '\0') {
        lab_orbit_steer(&orbit, dt_ms, input, shell_home_edge(), lab_view_orbit_ms(&render_lab_view_tunables));
    }
}

/* The copies walk on, before the shell draws this frame; a held herd stays put. */
static void
scene_herd_update(uint32_t dt_ms) {
    if (!handle_is_none(loaded) && failure[0] == '\0' && !held) {
        walk(dt_ms);
    }
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
    return render_lab_copies_status(failure, *shown);
}

/* The herd walking, sought as a whole; a still herd's path is the first copy playing its clip. */
static lab_path_t
scene_herd_path(void) {
    const lab_path_t first = {.scene = loaded, .entity = copies == NULL ? ENTITY_NONE : copies[0]};
    if (still) {
        return first;
    }
    return (lab_path_t){.scene = first.scene,
                        .entity = first.entity,
                        .seek = seek_walk,
                        .play = play_walk,
                        .duration_ms = HERD_PATH_MS};
}

#define HERD_COPIES(id_, key_, copies_)                                                                                \
    static const int herd_copies_##id_ = copies_;                                                                      \
    _Static_assert((copies_) <= HERD_WANDER_MAX, "a herd size herd_wander can walk");
HERD_SIZES(HERD_COPIES)

/* The copies move, so no frame is generated from the camera's pose alone. */
#define HERD_SCENE_ROW(id_, key_, copies_)                                                                             \
    [HERD_##id_] = {.name = "Herd " key_,                                                                              \
                    .key = HERD_KEY "-" key_,                                                                          \
                    .family = "Herd",                                                                                  \
                    .variant = (key_),                                                                                 \
                    .data = &herd_copies_##id_,                                                                        \
                    .enter = scene_herd_enter,                                                                         \
                    .frame = scene_herd_frame,                                                                         \
                    .exit = scene_herd_exit,                                                                           \
                    .steer = scene_herd_steer,                                                                         \
                    .update = scene_herd_update,                                                                       \
                    .status = LAB_SCENE_DEV(scene_herd_status),                                                        \
                    .path = scene_herd_path},

const lab_scene_t scene_herd[HERD_SIZE_COUNT] = {HERD_SIZES(HERD_SCENE_ROW)};
