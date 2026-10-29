/*
 * scene_water: water in a tank the size of the screen and as deep as the
 * device, simulated by flip3 and driven by the device's gravity.
 *
 * Each update reads gravity and shake (gravity.h), steps the water, meshes
 * its density field (iso_surface.h), shades the surface and hands it to
 * the scene as the water entity's mesh (r3d_dynamic_mesh.h). The tank is a
 * unit box the scene scales to the device; the camera looks through the
 * front glass like a window, held to the panel whichever way the picture
 * is turned, its depth sliding while the board turns.
 *
 * Box axes are gravity's: x right, y down, z out of the screen, in mm from
 * the tank's back top-left corner. The scene's model axes put the front
 * glass at z = 0: (x - w/2, h/2 - y, d - z).
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/app_arena.h"
#include "board/board_geometry.h"
#include "core/job.h"
#include "core/timing.h"
#include "core/tune.h"
#include "display/display.h"
#include "draw/draw.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "image/pixel/rgb565.h"
#include "input/motion/gravity.h"
#include "math/geometry/iso_surface.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "math/scalar/units.h"
#include "physics/fluid/flip3.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "render/skin/r3d_skin_light.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "scene_water.h"
#include "scene_water_plane.h"
#include "ui/ui.h"

static const char* TAG = "water";

#define WATER_SCENE           "water"
#define WATER_CAMERA          "camera"
#define WATER_TANK            "tank"
#define WATER_ENTITY          "water"

/* The fill, in percent of the tank's volume: upright, of the screen. */
#define WATER_FILL_MIN        10
#define WATER_FILL_MAX        90
#define WATER_FILL_STEP       10
#define WATER_FILL_DEFAULT    50
#define WATER_FILL_LABEL      "WATER FILL: %d%%"
#define WATER_PERCENT         100

/* The tank is the room behind the screen: the visible area, as deep as the
 * case less the display module and PCB stacked behind the glass. */
#define WATER_TANK_DEPTH_MM   (BOARD_CASE_DEPTH_MM - BOARD_DEVICE_DEPTH_MM)

/* Grid cells across the depth; the cell is the depth over this. Three over
 * the tank's 7.6 mm keeps cells near 2.5 mm, the size the frame budget was
 * sketched for: 11 x 14 x 3 cells and about 2200 particles at most. */
#define WATER_CELLS_DEEP      3
/* Density samples per cell along each axis, for the surface, in hundredths
 * (WATER_PERCENT a whole sample): 1.5 keeps the crests and splash at about
 * 0.6 times the triangles 2 make; the field is sized for the most. */
#define WATER_SAMPLES_PCT     150
#define WATER_SAMPLES_PCT_MAX 200
/* The surface sits where the density is half the rest density. */
#define WATER_SURFACE_LEVEL   0.5F

/* The most simulated time one frame advances: a longer frame slows the
 * water rather than costing a burst. */
#define WATER_MAX_STEP_MS     32

/* Earth's gravity, mm/s^2. The water runs under a share of it: at full
 * gravity a tank this small sloshes faster than a frame can follow, and a
 * smaller g looks like a larger tank (Froude scaling). Ours, by eye. */
#define WATER_EARTH_MM_S2     9810
TUNE_OWNER(water);
TUNE(water, gravity_mm_s2, 2000, 100, WATER_EARTH_MM_S2);
/* Substeps a frame, each an equal share of its simulated time: the same
 * count every frame, so the cost does not alternate. Two settle nearly as
 * well as four at about two thirds of the frame; one never settles
 * (run_water_perf_suite's base, sub4 and sub1 rows). */
TUNE(water, substeps, 2, 1, 4);
/* 1 hands part of the step's particle passes, the surface field, its
 * meshing and the shading to core 1, each cut where it balances the two
 * cores' times (job_run_range()); 0 keeps them on the update's core. */
TUNE(water, two_core, 1, 0, 1);
/* 1 solves the pressure in red-black order, each colour split like the
 * particle passes, 3 ms a frame faster on the board; 0 in index order on
 * one core. */
TUNE(water, red_black, 1, 0, 1);

/* 1 draws the plane (scene_water_plane.h), a cheap option to flip3 with no
 * crests or splash; 0 (the default) runs flip3. */
TUNE(water, plane, 0, 0, 1);

/* The surface's density samples per cell, in hundredths: fewer mesh,
 * build and draw fewer triangles and lose the smallest ripples. */
TUNE(water, samples_pct, WATER_SAMPLES_PCT, WATER_PERCENT, WATER_SAMPLES_PCT_MAX);
/* The plane's swing: its natural frequency in tenths of a hertz, and its
 * damping as a percentage of critical. Ours, by eye. */
TUNE(water, slosh_decihz, 15, 1, 100);
TUNE(water, slosh_damping_pct, 10, 0, 100);
#define WATER_DECIHZ_PER_HZ 10.0F

/* Surface buffers: the vertices and triangles iso_surface may make. The
 * tour's busiest frame makes about 2500 and 5000. */
#define WATER_MAX_VERTICES  4000
#define WATER_MAX_TRIANGLES 8000

/* Position ticks per mm: the tank's largest side, 36 mm, stays inside int16. */
#define WATER_TICKS_PER_MM  512

/* A quarter turn, for holding the tank to the panel whatever way the shell
 * turns the picture. */
#define WATER_QUARTER_TURN  (0.5F * MATH_PI)

/* Parallax: the eye stays on the screen's normal at rest, in any pose, and
 * moves only while the board turns: it follows the turn since a reference
 * down that catches up over settle_ms (as phone wallpaper parallax does), so
 * the depth slides, then eases back to centre. parallax_mm is the eye's
 * offset per radian of turn about the panel's x and y; a roll within the
 * screen's plane moves nothing, as with a real box. Ours, by eye. */
TUNE(water, parallax_mm, 100, 0, 400);
TUNE(water, settle_ms, 1000, 50, 5000);

/* Shading, in linear light. Deep and shallow water colours, absorbed over
 * WATER_ABSORB_MM of water (Beer-Lambert); Schlick's Fresnel toward the
 * sky colour, from water's reflectance at normal incidence; one light. */
#define WATER_DEEP               ((vec3f_t){0.01F, 0.05F, 0.16F})
#define WATER_SHALLOW            ((vec3f_t){0.20F, 0.55F, 0.80F})
#define WATER_SKY                ((vec3f_t){0.75F, 0.85F, 1.00F})
#define WATER_ABSORB_MM          8.0F
#define WATER_F0                 0.02F
#define WATER_LIGHT              ((vec3f_t){-0.4F, -0.6F, 0.7F}) /* box axes: from up-left-front */
/* The highlight's exponent, 3 * 2^4 = 48: a cube squared this many times. */
#define WATER_SPECULAR_SQUARINGS 4
#define WATER_SPECULAR           0.6F
#define WATER_DIFFUSE            0.35F
/* The encode table's tone-map white: none, the colours are already in range. */
#define WATER_TONEMAP_NONE       0.0F

/* A scripted tour of the tank, for host renders and repeatable timings:
 * down vectors in box axes, eased between keys, with a shake at the end.
 * Each key names the phase that runs from it to the next, which the perf
 * suite times as one row. */
typedef struct {
    uint32_t at_ms;
    vec3f_t down;
    const char* phase;
} water_key_t;

static const water_key_t TOUR[] = {
    {0, {0.0F, 1.0F, 0.0F}, "rest"},       /* upright */
    {1500, {0.0F, 1.0F, 0.0F}, "tilt"},    /* upright, easing toward tipped */
    {3000, {0.7F, 0.7F, 0.0F}, "tilt"},    /* tipped right */
    {4500, {-0.7F, 0.7F, 0.0F}, "tilt"},   /* tipped left */
    {6000, {0.0F, 0.6F, -0.8F}, "flat"},   /* leaned back */
    {7500, {0.0F, 0.0F, -1.0F}, "flat"},   /* flat on its back */
    {9000, {0.0F, 0.6F, 0.8F}, "return"},  /* toward the viewer */
    {10500, {0.0F, 1.0F, 0.0F}, "settle"}, /* upright */
    {12000, {0.0F, 1.0F, 0.0F}, "shake"},  /* then shaken */
};
#define TOUR_KEYS         ((int)(sizeof TOUR / sizeof TOUR[0]))
#define TOUR_SHAKE_MS     2000U
#define TOUR_SHAKE_HZ     3.0F
#define TOUR_SHAKE_G      0.8F

/* The swing (`water swing`) for the overview clip: rocked side to side,
 * hard and then calmer, flipped over and back twice, leaned back so the
 * surface shows through the front glass. It swings about the picture's
 * down, whichever way the shell holds it. */
#define SWING_MS          16000U
#define SWING_START_DEG   45.0F   /* the first swing's reach either side */
#define SWING_DECAY_MS    3500.0F /* the reach falls by e over this */
#define SWING_HZ          0.7F
#define SWING_INCLINE_DEG 25.0F /* leaned back throughout */
#define SWING_EYE_RISE_MM 20.0F /* the eye above the glass's centre, looking down onto the surface */
/* Each flip snaps the tank half over, the same way, and the next soon
 * after brings it upright while the water is still falling, so it breaks
 * and splashes back down: two such pairs. */
static const uint32_t SWING_FLIPS_MS[] = {3500U, 3900U, 7500U, 7900U};
#define SWING_FLIP_MS 250.0F /* how long a half turn takes */

static scene_t* tank_scene;
static scene_entity_t camera_entity, water_entity;
static flip3_t water;
static float* field;
static iso_field_t surface_field;
static float* through; /* light left after crossing each field column's water */
static r3d_skin_light_transfer_t* transfer;
static bool truncated;
static vec3f_t reference_down; /* down as the eye last settled, box axes */
static vec3f_t* positions;
static vec3f_t* normals;
static uint8_t (*colors)[3];
static uint16_t (*triangles)[3];
static int32_t* scratch[2]; /* one per meshing part */
static r3d_dynamic_mesh_t surface;
static vec3f_t box;
static gravity_t gravity;
static bool motion_seen;
static input_motion_batch_t batch;
static int fill_percent = WATER_FILL_DEFAULT;
static int filled_percent;

/* The scripted motion playing, if any, and how far into it. */
typedef enum { SCRIPT_NONE, SCRIPT_TOUR, SCRIPT_SWING } water_script_t;

static water_script_t script;
static uint32_t tour_ms;
static uint32_t step_us;
static scene_water_stats_t stats;
static water_plane_t plane_water;

/* The scene's own splits after flip3's, each with its core-1 share. */
enum { WATER_SPLIT_NETS = FLIP3_SPLIT_COUNT, WATER_SPLIT_SHADE, WATER_SPLITS };

static int16_t split_share[WATER_SPLITS];

scene_water_stats_t
scene_water_take_stats(void) {
    const scene_water_stats_t taken = stats;
    stats = (scene_water_stats_t){.particles = water.count, .peak_speed_mm_s = taken.peak_speed_mm_s};
    return taken;
}

static char failure[RENDER_LAB_STATUS_LEN];

static void
fail(const char* what) {
    render_lab_scene_fail(TAG, failure, sizeof failure, what);
}

/* From the app arena, which render_lab rewinds when the scene goes. */
static void*
take(size_t bytes) {
    return app_arena_take(bytes, _Alignof(max_align_t));
}

static bool
allocate(void) {
    flip3_config_t config = {box, box.z / (float)WATER_CELLS_DEEP, 1};
    void* grid = take(flip3_grid_bytes(&config));
    /* One particle to learn the seed lattice, then room for the most water
     * the slider allows; the grid is the same at any particle count. */
    if (grid == NULL || !flip3_init(&water, &config, grid, take(flip3_particle_bytes(&config)))) {
        return false;
    }
    config.max_particles = ((water.seed_count * WATER_FILL_MAX) + WATER_PERCENT - 1) / WATER_PERCENT;
    if (!flip3_init(&water, &config, grid, take(flip3_particle_bytes(&config)))) {
        return false;
    }
    const float most = (float)WATER_SAMPLES_PCT_MAX / (float)WATER_PERCENT;
    field = take(sizeof(float) * flip3_field_count(&water, most));
    if (field == NULL) {
        return false;
    }
    surface_field = flip3_field(&water, most, field);
    through = take(sizeof(float) * (size_t)(surface_field.nx * surface_field.ny));
    positions = take(sizeof(vec3f_t) * WATER_MAX_VERTICES);
    normals = take(sizeof(vec3f_t) * WATER_MAX_VERTICES);
    colors = take(sizeof(uint8_t[3]) * WATER_MAX_VERTICES);
    triangles = take(sizeof(uint16_t[3]) * WATER_MAX_TRIANGLES);
    for (int half = 0; half < 2; half++) {
        scratch[half] = take(sizeof(int32_t) * iso_surface_scratch_count(surface_field.ny, surface_field.nz));
    }
    transfer = take(sizeof *transfer);
    if (transfer != NULL) {
        r3d_skin_light_transfer_build(WATER_TONEMAP_NONE, transfer);
    }
    const r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(WATER_MAX_VERTICES, WATER_MAX_TRIANGLES);
    const size_t mesh_bytes = r3d_dynamic_mesh_bytes(capacity);
    void* mesh_block = take(mesh_bytes);
    return through != NULL && transfer != NULL && positions != NULL && normals != NULL && colors != NULL
           && triangles != NULL && scratch[0] != NULL && scratch[1] != NULL && mesh_block != NULL
           && r3d_dynamic_mesh_init(&surface, mesh_block, mesh_bytes, capacity, WATER_TICKS_PER_MM);
}

/* Box axes to the scene's model axes. */
static vec3f_t
to_model(vec3f_t p) {
    return (vec3f_t){p.x - (0.5F * box.x), (0.5F * box.y) - p.y, box.z - p.z};
}

static vec3f_t
direction_to_model(vec3f_t d) {
    return (vec3f_t){d.x, -d.y, -d.z};
}

/* The tank: a unit box scaled to the device's inside, its open face on the glass. */
static bool
place_tank(void) {
    const scene_entity_t tank = scene_find(tank_scene, name_hash(WATER_TANK));
    if (tank == SCENE_ENTITY_NONE) {
        return false;
    }
    transformf_t t = scene_entity_local(tank_scene, tank);
    transformf_set_position(&t, (vec3f_t){-0.5F * box.x, -0.5F * box.y, box.z});
    transformf_set_scale(&t, box);
    scene_entity_set_local(tank_scene, tank, t);
    return true;
}

/* A window onto the tank: the eye sits at `eye` across the glass's plane
 * and the lens shifts back by as much, so the front glass always fills the
 * picture exactly, from where it fills the short side as the screen's width
 * does, while the back wall and the water behind it slide (an off-axis
 * frustum). The camera rolls against the shell's quarter turn, so the tank
 * stays fixed to the panel, as the water's gravity is, whichever way the
 * picture is held. */
static void
place_camera(int quarter, vec3f_t eye) {
    const camera_t* lens = scene_camera_lens(tank_scene, name_hash(WATER_CAMERA));
    const float distance = 0.5F * box.x / lens->half_fov_short_tan;
    const quatf_t roll = quatf_from_axis_angle((vec3f_t){0.0F, 0.0F, 1.0F}, -(float)quarter * WATER_QUARTER_TURN);
    transformf_t t = scene_entity_local(tank_scene, camera_entity);
    transformf_set_rotation(&t, roll);
    transformf_set_position(&t, (vec3f_t){eye.x, eye.y, -distance});
    scene_entity_set_local(tank_scene, camera_entity, t);
    /* The glass's centre lands a fraction eye / width of the short side off
     * the picture's middle, against the eye; the optical centre follows the
     * eye by as much, in the camera's own axes. */
    const vec3f_t right = quatf_rotate(roll, (vec3f_t){1.0F, 0.0F, 0.0F});
    const vec3f_t up = quatf_rotate(roll, (vec3f_t){0.0F, 1.0F, 0.0F});
    const vec3f_t follow = vec3f_scale(eye, 1.0F / box.x);
    (void)scene_camera_set_shift(tank_scene, camera_entity, (vec2f_t){vec3f_dot(follow, right), vec3f_dot(follow, up)});
}

/* The eye, in model axes (x right, y up), from the turn since the reference
 * down: reference x down is that turn as a rotation vector in box axes, for
 * small angles. Lifting the right edge toward the viewer moves the eye left;
 * leaning the top away moves it up. Both signs checked on the board, the
 * second held landscape, where it is the left-right tilt. */
static vec3f_t
parallax_eye(vec3f_t down, uint32_t dt_ms) {
    if (vec3f_dot(reference_down, reference_down) == 0.0F) {
        reference_down = down; /* the first reading: start centred, whatever the pose */
    }
    const float catch_up = 1.0F - expf(-(float)dt_ms / (float)settle_ms);
    reference_down = vec3f_normalize(vec3f_lerp(reference_down, down, catch_up));
    const vec3f_t turn = vec3f_cross(reference_down, down);
    return vec3f_scale((vec3f_t){-turn.y, -turn.x, 0.0F}, (float)parallax_mm);
}

uint32_t
scene_water_tour_length_ms(void) {
    return TOUR[TOUR_KEYS - 1].at_ms + TOUR_SHAKE_MS;
}

const char*
scene_water_tour_phase(void) {
    int k = 0;
    while (k + 1 < TOUR_KEYS && tour_ms >= TOUR[k + 1].at_ms) {
        k++;
    }
    return TOUR[k].phase;
}

/* Down, eased between the tour's keys; a shake past its last key. */
static vec3f_t
tour_accel(uint32_t at_ms, vec3f_t* down) {
    const uint32_t end = TOUR[TOUR_KEYS - 1].at_ms;
    vec3f_t d = TOUR[TOUR_KEYS - 1].down;
    for (int k = 0; k + 1 < TOUR_KEYS; k++) {
        if (at_ms < TOUR[k + 1].at_ms) {
            const float t = (float)(at_ms - TOUR[k].at_ms) / (float)(TOUR[k + 1].at_ms - TOUR[k].at_ms);
            const float eased = t * t * (3.0F - (2.0F * t));
            d = vec3f_lerp(TOUR[k].down, TOUR[k + 1].down, eased);
            break;
        }
    }
    *down = vec3f_normalize(d);
    vec3f_t accel = *down;
    if (at_ms >= end && at_ms < end + TOUR_SHAKE_MS) {
        const float phase = MATH_TAU * TOUR_SHAKE_HZ * (float)(at_ms - end) / (float)MATH_MS_PER_S;
        accel.x += TOUR_SHAKE_G * sinf(phase);
    }
    return accel;
}

/* The shell's quarter turn of the picture as an angle about the box's z,
 * the camera's roll: the picture's down in the box is turned by it. */
static float
held_turn(void) {
    return -(float)display_quarter_now() * WATER_QUARTER_TURN;
}

/* Down while swinging: the reach about the box's z decays, each flip adds a
 * half turn about it, eased, and the lean back about its x holds. */
static vec3f_t
swing_accel(uint32_t at_ms, vec3f_t* down) {
    const float t_s = (float)at_ms / (float)MATH_MS_PER_S;
    const float reach = SWING_START_DEG * MATH_RADIANS_PER_DEGREE * expf(-(float)at_ms / SWING_DECAY_MS);
    float angle = held_turn() + (reach * sinf(MATH_TAU * SWING_HZ * t_s));
    for (size_t i = 0; i < sizeof SWING_FLIPS_MS / sizeof SWING_FLIPS_MS[0]; i++) {
        if (at_ms > SWING_FLIPS_MS[i]) {
            const float t = fminf((float)(at_ms - SWING_FLIPS_MS[i]) / SWING_FLIP_MS, 1.0F);
            angle += MATH_PI * t * t * (3.0F - (2.0F * t));
        }
    }
    const float lean = SWING_INCLINE_DEG * MATH_RADIANS_PER_DEGREE;
    *down = (vec3f_t){sinf(angle) * cosf(lean), cosf(angle) * cosf(lean), -sinf(lean)};
    return *down;
}

/* Gravity plus the device's own acceleration, in g, box axes. Upright until
 * a motion sample arrives, as on a host. */
static vec3f_t
read_accel(uint32_t dt_ms, vec3f_t* down) {
    if (script == SCRIPT_TOUR) {
        tour_ms = (tour_ms + dt_ms) % scene_water_tour_length_ms();
        return tour_accel(tour_ms, down);
    }
    if (script == SCRIPT_SWING) {
        tour_ms = (tour_ms + dt_ms) % SWING_MS;
        return swing_accel(tour_ms, down);
    }
    if (input_read_motion_batch(&batch)) {
        gravity_update_batch(&gravity, &batch);
        motion_seen = true;
    }
    if (!motion_seen) {
        *down = (vec3f_t){0.0F, 1.0F, 0.0F};
        return *down;
    }
    *down = gravity_down(&gravity);
    return vec3f_add(*down, gravity_linear(&gravity));
}

/* The light left after crossing each field column's water along z
 * (Beer-Lambert), for the colour's depth: one exponential a column. */
static void
measure_through(void) {
    const iso_field_t* f = &surface_field;
    for (int x = 0; x < f->nx; x++) {
        for (int y = 0; y < f->ny; y++) {
            const float* column = &f->values[((x * f->ny) + y) * f->nz];
            float depth = 0.0F;
            for (int z = 0; z < f->nz; z++) {
                depth += mathf_clamp(column[z], 0.0F, 1.0F);
            }
            through[(x * f->ny) + y] = expf(-depth * f->step.z / WATER_ABSORB_MM);
        }
    }
}

static float
through_at(vec3f_t p) {
    const iso_field_t* f = &surface_field;
    const int x = (int)mathf_clamp(((p.x - f->origin.x) / f->step.x) + 0.5F, 0.0F, (float)(f->nx - 1));
    const int y = (int)mathf_clamp(((p.y - f->origin.y) / f->step.y) + 0.5F, 0.0F, (float)(f->ny - 1));
    return through[(x * f->ny) + y];
}

/* x^5, for Schlick's Fresnel. */
static float
pow5(float x) {
    const float x2 = x * x;
    return x2 * x2 * x;
}

/* x^(3 * 2^WATER_SPECULAR_SQUARINGS), for the highlight. */
static float
specular_power(float x) {
    float p = x * x * x;
    for (int i = 0; i < WATER_SPECULAR_SQUARINGS; i++) {
        p *= p;
    }
    return p;
}

/* flip3's splits and the scene's: the last items on core 1, as many as
 * `site`'s share says, while this core does the first; with water.two_core
 * off, all on this core. */
static void
water_split(int site, job_range_fn_t fn, void* ctx, int count) {
    if (two_core != 0) {
        (void)job_run_range(&split_share[site], fn, ctx, count);
    } else {
        fn(ctx, 0, count);
    }
}

/* Absorbed body colour, a diffuse light, Fresnel sky and a highlight, for
 * vertices [from, to). The eye looks along -z in box axes, through the
 * front glass. */
static void
shade_range(void* ctx, int from, int to) {
    (void)ctx;
    const vec3f_t light = vec3f_normalize(WATER_LIGHT);
    const vec3f_t eye = {0.0F, 0.0F, 1.0F};
    const vec3f_t half = vec3f_normalize(vec3f_add(light, eye));
    for (int i = from; i < to; i++) {
        const vec3f_t n = normals[i];
        vec3f_t c = vec3f_lerp(WATER_DEEP, WATER_SHALLOW, through_at(positions[i]));
        c = vec3f_scale(c, 1.0F - WATER_DIFFUSE + (WATER_DIFFUSE * fmaxf(vec3f_dot(n, light), 0.0F)));
        const float facing = fmaxf(vec3f_dot(n, eye), 0.0F);
        const float fresnel = WATER_F0 + ((1.0F - WATER_F0) * pow5(1.0F - facing));
        c = vec3f_lerp(c, WATER_SKY, fresnel);
        const float spec = WATER_SPECULAR * specular_power(fmaxf(vec3f_dot(n, half), 0.0F));
        c = vec3f_add(c, (vec3f_t){spec, spec, spec});
        colors[i][0] = r3d_skin_light_encode(c.x, transfer);
        colors[i][1] = r3d_skin_light_encode(c.y, transfer);
        colors[i][2] = r3d_skin_light_encode(c.z, transfer);
    }
}

/* The surface meshed in two parts of the cell slabs, the first into the
 * buffers' first half and the last into their second. */
typedef struct {
    iso_mesh_t part[2];
    bool fitted[2];
} nets_work_t;

/* A range from slab 0 is the first part, any other the last; an empty one
 * leaves its part empty. */
static void
nets_range(void* ctx, int from, int to) {
    nets_work_t* w = ctx;
    if (from == to) {
        return;
    }
    const int which = from == 0 ? 0 : 1;
    w->fitted[which] = iso_surface_nets_slabs(&surface_field, WATER_SURFACE_LEVEL, from, to, &w->part[which]);
}

/* The surface on both cores, joined into one mesh at the buffers' start;
 * meshed again whole on this core when a part outgrew its half. */
static bool
mesh_split(iso_mesh_t* m) {
    const int vertices = WATER_MAX_VERTICES / 2;
    const int tris = WATER_MAX_TRIANGLES / 2;
    nets_work_t w = {
        .part = {{positions, normals, vertices, triangles, tris, scratch[0], 0, 0},
                 {&positions[vertices], &normals[vertices], vertices, &triangles[tris], tris, scratch[1], 0, 0}},
        .fitted = {true, true},
    };
    water_split(WATER_SPLIT_NETS, nets_range, &w, surface_field.nx - 1);
    *m = w.part[0];
    m->vertex_capacity = WATER_MAX_VERTICES;
    m->triangle_capacity = WATER_MAX_TRIANGLES;
    if (w.fitted[0] && w.fitted[1] && iso_surface_append(m, &w.part[1])) {
        return true;
    }
    stats.remeshed++;
    return iso_surface_nets(&surface_field, WATER_SURFACE_LEVEL, m);
}

/* The surface field meshed, shaded, moved into model axes and handed to the
 * scene. A surface too big for the buffers is cut, said once and shown. */
static void
rebuild_surface(void) {
    FRAME_COST_BEGIN(field_from);
    if (plane != 0) {
        water_plane_field(&plane_water, &surface_field, (float)fill_percent / (float)WATER_PERCENT);
    } else {
        surface_field = flip3_field(&water, (float)samples_pct / (float)WATER_PERCENT, field);
    }
    FRAME_COST_END(field_from, "water.field");
    FRAME_COST_BEGIN(through_from);
    measure_through();
    FRAME_COST_END(through_from, "water.through");
    FRAME_COST_BEGIN(nets_from);
    iso_mesh_t m = {positions, normals, WATER_MAX_VERTICES, triangles, WATER_MAX_TRIANGLES, scratch[0], 0, 0};
    bool fitted = two_core != 0 ? mesh_split(&m) : iso_surface_nets(&surface_field, WATER_SURFACE_LEVEL, &m);
    FRAME_COST_END(nets_from, "water.nets");
    FRAME_COST_BEGIN(shade_from);
    water_split(WATER_SPLIT_SHADE, shade_range, NULL, m.vertex_count);
    FRAME_COST_END(shade_from, "water.shade");
    FRAME_COST_BEGIN(build_from);
    for (int i = 0; i < m.vertex_count; i++) {
        positions[i] = to_model(positions[i]);
        normals[i] = direction_to_model(normals[i]);
    }
    const r3d_dynamic_source_t source = {positions,        (const uint8_t (*)[3])colors,
                                         m.vertex_count,   (const uint16_t (*)[3])triangles,
                                         m.triangle_count, false};
    fitted = r3d_dynamic_mesh_build(&surface, &source) && fitted;
    if (!fitted && !truncated) {
        ESP_LOGW(TAG, "the surface outgrew its buffers and is cut");
    }
    truncated = !fitted;
    (void)scene_renderer_set_mesh(tank_scene, water_entity, &surface.mesh);
    FRAME_COST_END(build_from, "water.build");
}

static void
refill(vec3f_t down) {
    (void)flip3_fill(&water, (float)fill_percent / (float)WATER_PERCENT, down);
    filled_percent = fill_percent;
}

/* render_lab's debug view (`render_lab.view`), as every scene applies it. */
static void
apply_view(void) {
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

static void
scene_water_enter(const render_lab_scene_t* self) {
    (void)self;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    box = (vec3f_t){BOARD_SCREEN_WIDTH_MM, BOARD_SCREEN_HEIGHT_MM, WATER_TANK_DEPTH_MM};
    scene_failure_t why;
    tank_scene = scene_load(WATER_SCENE, &why);
    if (tank_scene == NULL) {
        fail(why.asset == ASSET_ERR_NOT_FOUND || why.asset == ASSET_ERR_NO_PACK ? "no water pack: flash it"
                                                                                : "water did not load");
        return;
    }
    camera_entity = scene_find(tank_scene, name_hash(WATER_CAMERA));
    water_entity = scene_find(tank_scene, name_hash(WATER_ENTITY));
    if (camera_entity == SCENE_ENTITY_NONE || water_entity == SCENE_ENTITY_NONE || !place_tank()) {
        fail("no tank, water or camera");
        return;
    }
    if (!allocate()) {
        fail("no memory for the water");
        return;
    }
    gravity_reset(&gravity);
    motion_seen = false;
    truncated = false;
    tour_ms = 0;
#if FRAME_COST_ENABLED
    flip3_time_stages(&water, timing_now_us, stats.flip3_us);
#endif
    flip3_split_with(&water, water_split);
    memset(split_share, 0, sizeof split_share);
    refill((vec3f_t){0.0F, 1.0F, 0.0F});
    water_plane_reset(&plane_water, box);
    rebuild_surface();
    (void)scene_water_take_stats();
    reference_down = (vec3f_t){0}; /* set by the first reading */
    place_camera(display_quarter_now(), (vec3f_t){0});
    (void)scene_activate(tank_scene, name_hash(WATER_CAMERA));
    render_lab_apply_raster_settings();
    (void)render_lab_apply_camera_settings(tank_scene, camera_entity);
    apply_view();
}

static void
scene_water_exit(void) {
    scene_unload(tank_scene);
    tank_scene = NULL;
}

/* The plane's swing, or flip3's substeps, over the frame's dt_ms. */
static void
step_water(vec3f_t accel_g, uint32_t dt_ms) {
    FRAME_COST_BEGIN(step_from);
    const vec3f_t accel = vec3f_scale(accel_g, (float)gravity_mm_s2);
    water.tuning.red_black = red_black != 0;
    const float step_s = (float)mathi_min((int)dt_ms, WATER_MAX_STEP_MS) / (float)MATH_MS_PER_S;
    if (plane != 0) {
        water_plane_step(&plane_water, step_s, accel_g, (float)slosh_decihz / WATER_DECIHZ_PER_HZ,
                         (float)slosh_damping_pct / (float)WATER_PERCENT);
    } else if (dt_ms > 0) {
        for (int i = 0; i < substeps; i++) {
            flip3_step(&water, step_s / (float)substeps, accel);
        }
        stats.substeps += (uint32_t)substeps;
        if (FRAME_COST_ENABLED) {
            stats.peak_speed_mm_s = sqrtf(flip3_peak_speed_sq(&water));
        }
    }
    FRAME_COST_END(step_from, "water.step");
}

/* The swing's eye above the glass's centre, against the picture's down; the
 * eye's x runs opposite the box's. */
static vec3f_t
eye_rise(void) {
    const float rise_mm = script == SCRIPT_SWING ? SWING_EYE_RISE_MM : 0.0F;
    return (vec3f_t){-rise_mm * sinf(held_turn()), rise_mm * cosf(held_turn()), 0.0F};
}

/* Gravity, the fill, the steps and the surface, while the last frame goes out. */
static void
scene_water_update(uint32_t dt_ms) {
    if (tank_scene == NULL || failure[0] != '\0') {
        return;
    }
    vec3f_t down;
    const vec3f_t accel_g = read_accel(dt_ms, &down);
    if (filled_percent != fill_percent) {
        refill(down);
    }
    FRAME_COST_BEGIN(updated_from);
    const int64_t started = timing_now_us();
    stats.updates++;
    step_water(accel_g, dt_ms);
    rebuild_surface();
    stats.triangles += surface.mesh.triangle_count;
    stats.particles = water.count;
    stats.update_us += timing_now_us() - started;
    step_us = (uint32_t)(timing_now_us() - started);
    FRAME_COST_END(updated_from, "water.update");
    place_camera(display_quarter_now(), vec3f_add(parallax_eye(down, dt_ms), eye_rise()));
}

static void
scene_water_frame(uint32_t dt_ms) {
    (void)dt_ms;
    render_lab_apply_raster_settings();
    apply_view();
    if (tank_scene != NULL) {
        (void)render_lab_apply_camera_settings(tank_scene, camera_entity);
    }
}

static inline const char*
water_status(void) {
    static char buf[RENDER_LAB_STATUS_LEN];
    if (failure[0] != '\0') {
        return failure;
    }
    if (snprintf(buf, sizeof buf, "%d%% %d tris%s %u.%u ms", fill_percent, surface.mesh.triangle_count,
                 truncated ? " cut" : "", (unsigned)(step_us / MATH_US_PER_MS),
                 (unsigned)((step_us % MATH_US_PER_MS) / 100U))
        < 0) {
        buf[0] = '\0';
    }
    return buf;
}

/* False, with the range as the reply, for a fill outside it. */
static bool
fill_in_range(int percent, console_reply_fn reply) {
    if (percent >= WATER_FILL_MIN && percent <= WATER_FILL_MAX) {
        return true;
    }
    char line[RENDER_LAB_STATUS_LEN];
    if (snprintf(line, sizeof line, "RENDER_ERR water fill is %d to %d", WATER_FILL_MIN, WATER_FILL_MAX) >= 0) {
        reply(line);
    }
    return false;
}

/* The playing script's length, 0 for none. */
static uint32_t
script_ms(void) {
    switch (script) {
        case SCRIPT_TOUR: return scene_water_tour_length_ms();
        case SCRIPT_SWING: return SWING_MS;
        default: return 0;
    }
}

/* `water fill <percent>`, and `water tour` or `water swing`, which restart
 * that scripted motion. The reply gives the script's length. */
static inline bool
scene_water_console(const char* args, console_reply_fn reply) {
    int percent;
    char line[RENDER_LAB_STATUS_LEN];
    if (sscanf(args, "water fill %d", &percent) == 1) {
        if (!fill_in_range(percent, reply)) {
            return true;
        }
        fill_percent = percent;
    } else if (strcmp(args, "water tour") == 0 || strcmp(args, "water swing") == 0) {
        script = strcmp(args, "water tour") == 0 ? SCRIPT_TOUR : SCRIPT_SWING;
        tour_ms = 0;
        filled_percent = 0; /* refill as the script starts */
    } else {
        return false;
    }
    if (snprintf(line, sizeof line, "RENDER water fill=%d script_ms=%u", fill_percent, (unsigned)script_ms()) >= 0) {
        reply(line);
    }
    reply("RENDER_END");
    return true;
}

static void
scene_water_controls(int width) {
    char label[sizeof WATER_FILL_LABEL];
    if (snprintf(label, sizeof label, WATER_FILL_LABEL, fill_percent) < 0) {
        label[0] = '\0';
    }
    render_lab_menu_caption(width, label);
    /* The slider moves in whole steps, so it counts steps, not percent. */
    ui_row_centre(width, UI_ROW_HEIGHT);
    int steps = fill_percent / WATER_FILL_STEP;
    if (ui_slider(ui_next(), "WATER FILL", &steps, WATER_FILL_MIN / WATER_FILL_STEP,
                  WATER_FILL_MAX / WATER_FILL_STEP)) {
        fill_percent = steps * WATER_FILL_STEP;
    }
}

const render_lab_scene_t scene_water = {.name = "Water",
                                        .key = WATER_SCENE,
                                        .family = "Water",
                                        .enter = scene_water_enter,
                                        .frame = scene_water_frame,
                                        .exit = scene_water_exit,
                                        .status = RENDER_LAB_SCENE_DEV(water_status),
                                        .update = scene_water_update,
                                        .controls = scene_water_controls,
                                        .console = RENDER_LAB_SCENE_DEV(scene_water_console)};
