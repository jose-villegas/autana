/*
 * scene_water: water in a tank the size of the screen and as deep as the
 * device, simulated by flip3 and driven by the device's gravity.
 *
 * Each update reads gravity and shake (gravity.h), steps the water, meshes
 * its density field (iso_surface.h), shades the surface and hands it to
 * the scene as the water entity's mesh (r3d_dynamic_mesh.h). The tank is a
 * unit box the scene scales to the device; the camera looks through the
 * front glass and swings a little with the tilt, so the depth reads.
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
#include "board/board_size.h"
#include "core/memory.h"
#include "core/timing.h"
#include "display/display_size.h"
#include "draw/draw.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "input/motion/gravity.h"
#include "math/geometry/iso_surface.h"
#include "math/motion/flip3.h"
#include "math/scalar/math_const.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "services/tune.h"
#include "ui/ui.h"

static const char* TAG = "water";

#define WATER_SCENE         "water"
#define WATER_CAMERA        "camera"
#define WATER_TANK          "tank"
#define WATER_ENTITY        "water"

/* The fill, in percent of the tank's volume: upright, of the screen. */
#define WATER_FILL_MIN      10
#define WATER_FILL_MAX      90
#define WATER_FILL_STEP     10
#define WATER_FILL_DEFAULT  50
#define WATER_FILL_LABEL    "WATER FILL: %d%%"
#define WATER_PERCENT       100

/* Grid cells across the depth; the cell is the depth over this. */
#define WATER_CELLS_DEEP    5
/* Density samples per cell along each axis, for the surface. */
#define WATER_SAMPLES       2
/* The surface sits where the density is half the rest density. */
#define WATER_SURFACE_LEVEL 0.5F

/* Simulated time per substep, and the most substeps one frame runs: a long
 * frame drops the rest, slowing the water rather than costing a burst. */
#define WATER_SUBSTEP_MS    8U
#define WATER_MAX_SUBSTEPS  4U

/* Earth's gravity, mm/s^2. The water runs under a share of it: at full
 * gravity a tank this small sloshes faster than a frame can follow, and a
 * smaller g looks like a larger tank (Froude scaling). Ours, by eye. */
#define WATER_EARTH_MM_S2   9810
TUNE_OWNER(water);
TUNE(water, gravity_mm_s2, 2000, 100, WATER_EARTH_MM_S2);
/* 1 puts the grid in internal RAM, 0 in PSRAM: for measuring which the
 * step needs. Read when the scene enters. */
TUNE(water, grid_internal, 0, 0, 1);

/* Surface buffers: vertices and triangles iso_surface may make, and what
 * the clusters' copies need beyond them. */
#define WATER_MAX_VERTICES  8000
#define WATER_MAX_TRIANGLES 16000
#define WATER_CLUSTER_COPY  2

/* Position ticks per mm: the tank's largest side, 36 mm, stays inside int16. */
#define WATER_TICKS_PER_MM  512

/* The camera looks through the front glass from arm's length, and turns
 * this share of the tilt about the glass's centre. */
#define WATER_VIEW_MM       300.0F
#define WATER_PARALLAX      0.35F

/* Shading, in linear light. Deep and shallow water colours, absorbed over
 * WATER_ABSORB_MM of water (Beer-Lambert); Schlick's Fresnel toward the
 * sky colour, from water's reflectance at normal incidence; one light. */
#define WATER_DEEP          ((vec3f_t){0.01F, 0.05F, 0.16F})
#define WATER_SHALLOW       ((vec3f_t){0.20F, 0.55F, 0.80F})
#define WATER_SKY           ((vec3f_t){0.75F, 0.85F, 1.00F})
#define WATER_ABSORB_MM     8.0F
#define WATER_F0            0.02F
#define WATER_LIGHT         ((vec3f_t){-0.4F, -0.6F, 0.7F}) /* box axes: from up-left-front */
#define WATER_SPECULAR_EXP  48.0F
#define WATER_SPECULAR      0.6F
#define WATER_DIFFUSE       0.35F

/* A scripted tour of the tank, for host renders and repeatable timings:
 * down vectors in box axes, eased between keys, with a shake at the end. */
typedef struct {
    uint32_t at_ms;
    vec3f_t down;
} water_key_t;

static const water_key_t TOUR[] = {
    {0, {0.0F, 1.0F, 0.0F}},     /* upright */
    {1500, {0.0F, 1.0F, 0.0F}},  /* */
    {3000, {0.7F, 0.7F, 0.0F}},  /* tipped right */
    {4500, {-0.7F, 0.7F, 0.0F}}, /* tipped left */
    {6000, {0.0F, 0.6F, -0.8F}}, /* leaned back */
    {7500, {0.0F, 0.0F, -1.0F}}, /* flat on its back */
    {9000, {0.0F, 0.6F, 0.8F}},  /* toward the viewer */
    {10500, {0.0F, 1.0F, 0.0F}}, /* upright */
    {12000, {0.0F, 1.0F, 0.0F}}, /* then shaken */
};
#define TOUR_KEYS     ((int)(sizeof TOUR / sizeof TOUR[0]))
#define TOUR_SHAKE_MS 2000U
#define TOUR_SHAKE_HZ 3.0F
#define TOUR_SHAKE_G  0.8F

static scene_t* tank_scene;
static scene_entity_t camera_entity, water_entity;
static flip3_t water;
static void* grid_internal_block;
static float* field;
static int fx, fy, fz;
static float* thickness; /* water along z per field column, mm */
static vec3f_t* positions;
static vec3f_t* normals;
static uint8_t (*colors)[3];
static uint16_t (*triangles)[3];
static int32_t* scratch;
static r3d_dynamic_mesh_t surface;
static vec3f_t box;
static gravity_t gravity;
static bool motion_seen;
static input_motion_batch_t batch;
static int fill_percent = WATER_FILL_DEFAULT;
static int filled_percent;
static bool touring;
static uint32_t tour_ms;
static uint32_t carried_ms;
static uint32_t step_us;

static char failure[RENDER_LAB_STATUS_LEN];

static void
fail(const char* what) {
    ESP_LOGE(TAG, "%s; the scene stays blank", what);
    if (snprintf(failure, sizeof failure, "%s", what) < 0) {
        failure[0] = '\0';
    }
}

/* From the app arena, which render_lab rewinds when the scene goes. */
static void*
take(size_t bytes) {
    return app_arena_take(bytes, _Alignof(max_align_t));
}

/* Only an internal grid, taken for measuring, is freed by hand. */
static void
release(void) {
    if (grid_internal_block != NULL) {
        memory_free(grid_internal_block);
        grid_internal_block = NULL;
    }
}

static bool
allocate(void) {
    flip3_config_t config = {box, box.z / (float)WATER_CELLS_DEEP, 1};
    const size_t grid_bytes = flip3_grid_bytes(&config);
    if (grid_internal != 0) {
        grid_internal_block = memory_alloc(grid_bytes, MEMORY_INTERNAL);
    }
    void* grid = grid_internal != 0 ? grid_internal_block : take(grid_bytes);
    /* One particle to learn the seed lattice, then room for the most water
     * the slider allows; the grid is the same at any particle count. */
    if (grid == NULL || !flip3_init(&water, &config, grid, take(flip3_particle_bytes(&config)))) {
        return false;
    }
    config.max_particles = ((water.seed_count * WATER_FILL_MAX) + WATER_PERCENT - 1) / WATER_PERCENT;
    if (!flip3_init(&water, &config, grid, take(flip3_particle_bytes(&config)))) {
        return false;
    }
    fx = (flip3_cells_x(&water) * WATER_SAMPLES) + 2;
    fy = (flip3_cells_y(&water) * WATER_SAMPLES) + 2;
    fz = (flip3_cells_z(&water) * WATER_SAMPLES) + 2;
    field = take(sizeof(float) * (size_t)(fx * fy * fz));
    thickness = take(sizeof(float) * (size_t)(fx * fy));
    positions = take(sizeof(vec3f_t) * WATER_MAX_VERTICES);
    normals = take(sizeof(vec3f_t) * WATER_MAX_VERTICES);
    colors = take(sizeof(uint8_t[3]) * WATER_MAX_VERTICES);
    triangles = take(sizeof(uint16_t[3]) * WATER_MAX_TRIANGLES);
    scratch = take(sizeof(int32_t) * iso_surface_scratch_count(fy, fz));
    const r3d_dynamic_capacity_t capacity = {WATER_CLUSTER_COPY * WATER_MAX_VERTICES, WATER_MAX_TRIANGLES,
                                             WATER_MAX_VERTICES};
    const size_t mesh_bytes = r3d_dynamic_mesh_bytes(capacity);
    void* mesh_block = take(mesh_bytes);
    return field != NULL && thickness != NULL && positions != NULL && normals != NULL && colors != NULL
           && triangles != NULL && scratch != NULL && mesh_block != NULL
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
    const scene_entity_t tank = scene_find(tank_scene, WATER_TANK);
    if (tank == SCENE_ENTITY_NONE) {
        return false;
    }
    transformf_t t = *scene_entity_transform(tank_scene, tank);
    transformf_set_position(&t, (vec3f_t){-0.5F * box.x, -0.5F * box.y, box.z});
    transformf_set_scale(&t, box);
    scene_set_transform(tank_scene, tank, &t);
    return true;
}

/* The camera turns about the glass's centre by a share of the tilt, so the
 * walls show on the side the device leans away from. */
static void
place_camera(vec3f_t down) {
    const float yaw = -WATER_PARALLAX * down.x;
    const float pitch = WATER_PARALLAX * (down.z < 0.0F ? down.z : 0.0F);
    const quatf_t turn = quatf_from_euler((vec3f_t){pitch, yaw, 0.0F});
    const vec3f_t forward = quatf_rotate(turn, (vec3f_t){0.0F, 0.0F, 1.0F});
    transformf_t t = *scene_entity_transform(tank_scene, camera_entity);
    transformf_set_rotation(&t, turn);
    transformf_set_position(&t, vec3f_scale(forward, -WATER_VIEW_MM));
    scene_set_transform(tank_scene, camera_entity, &t);
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

/* Gravity plus the device's own acceleration, in g, box axes. Upright until
 * a motion sample arrives, as on a host. */
static vec3f_t
read_accel(uint32_t dt_ms, vec3f_t* down) {
    if (touring) {
        tour_ms = (tour_ms + dt_ms) % (TOUR[TOUR_KEYS - 1].at_ms + TOUR_SHAKE_MS);
        return tour_accel(tour_ms, down);
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

/* One [1 2 1] / 4 pass along each axis, inside the empty border: a film a
 * sample thin falls under the surface level instead of meshing as noise,
 * and the surface rounds off. The walls mirror, so water touching glass
 * keeps its density there and an edge between two walls opens no crack.
 * In place, carrying the sample before. */
static void
smooth_axis(int stride, int count, int step_a, int count_a, int step_b, int count_b) {
    for (int a = 1; a < count_a - 1; a++) {
        for (int b = 1; b < count_b - 1; b++) {
            float* line = &field[(a * step_a) + (b * step_b)];
            float before = line[stride];
            for (int i = 1; i < count - 1; i++) {
                const float here = line[i * stride];
                const float after = i + 1 < count - 1 ? line[(i + 1) * stride] : here;
                line[i * stride] = 0.25F * (before + (2.0F * here) + after);
                before = here;
            }
        }
    }
}

static void
smooth_field(void) {
    smooth_axis(fy * fz, fx, fz, fy, 1, fz);
    smooth_axis(fz, fy, fy * fz, fx, 1, fz);
    smooth_axis(1, fz, fy * fz, fx, fz, fy);
}

/* Water along z through each field column, for the colour's depth. */
static void
measure_thickness(void) {
    const float dz = water.h.z / (float)WATER_SAMPLES;
    for (int x = 0; x < fx; x++) {
        for (int y = 0; y < fy; y++) {
            float t = 0.0F;
            for (int z = 0; z < fz; z++) {
                t += fminf(field[(((x * fy) + y) * fz) + z], 1.0F);
            }
            thickness[(x * fy) + y] = t * dz;
        }
    }
}

static float
thickness_at(vec3f_t p) {
    const vec3f_t step = vec3f_scale(water.h, 1.0F / (float)WATER_SAMPLES);
    const int x = (int)fminf(fmaxf((p.x / step.x) + 0.5F, 0.0F), (float)(fx - 1));
    const int y = (int)fminf(fmaxf((p.y / step.y) + 0.5F, 0.0F), (float)(fy - 1));
    return thickness[(x * fy) + y];
}

static uint8_t
to_srgb8(float linear) {
    const float c = fminf(fmaxf(linear, 0.0F), 1.0F);
    const float s = c <= 0.0031308F ? 12.92F * c : (1.055F * powf(c, 1.0F / 2.4F)) - 0.055F;
    return (uint8_t)lroundf(s * 255.0F);
}

/* Absorbed body colour, a diffuse light, Fresnel sky and a highlight. The
 * eye looks along -z in box axes, through the front glass. */
static void
shade(int count) {
    const vec3f_t light = vec3f_normalize(WATER_LIGHT);
    const vec3f_t eye = {0.0F, 0.0F, 1.0F};
    const vec3f_t half = vec3f_normalize(vec3f_add(light, eye));
    for (int i = 0; i < count; i++) {
        const vec3f_t n = normals[i];
        const float through = expf(-thickness_at(positions[i]) / WATER_ABSORB_MM);
        vec3f_t c = vec3f_lerp(WATER_DEEP, WATER_SHALLOW, through);
        c = vec3f_scale(c, 1.0F - WATER_DIFFUSE + (WATER_DIFFUSE * fmaxf(vec3f_dot(n, light), 0.0F)));
        const float facing = fmaxf(vec3f_dot(n, eye), 0.0F);
        const float fresnel = WATER_F0 + ((1.0F - WATER_F0) * powf(1.0F - facing, 5.0F));
        c = vec3f_lerp(c, WATER_SKY, fresnel);
        const float spec = WATER_SPECULAR * powf(fmaxf(vec3f_dot(n, half), 0.0F), WATER_SPECULAR_EXP);
        c = vec3f_add(c, (vec3f_t){spec, spec, spec});
        colors[i][0] = to_srgb8(c.x);
        colors[i][1] = to_srgb8(c.y);
        colors[i][2] = to_srgb8(c.z);
    }
}

/* The density field meshed, moved into model axes and wound for them. */
static void
rebuild_surface(void) {
    (void)flip3_density_field(&water, WATER_SAMPLES, field, fx, fy, fz);
    smooth_field();
    measure_thickness();
    const vec3f_t step = vec3f_scale(water.h, 1.0F / (float)WATER_SAMPLES);
    const iso_field_t f = {field, fx, fy, fz, vec3f_scale(step, -0.5F), step};
    iso_mesh_t m = {positions, normals, WATER_MAX_VERTICES, triangles, WATER_MAX_TRIANGLES, scratch, 0, 0};
    (void)iso_surface_nets(&f, WATER_SURFACE_LEVEL, &m);
    shade(m.vertex_count);
    for (int i = 0; i < m.vertex_count; i++) {
        positions[i] = to_model(positions[i]);
        normals[i] = direction_to_model(normals[i]);
    }
    /* The model axes keep handedness, and the raster's front faces wind the
     * other way about their outward normal, so each triangle turns over. */
    for (int t = 0; t < m.triangle_count; t++) {
        const uint16_t keep = triangles[t][1];
        triangles[t][1] = triangles[t][2];
        triangles[t][2] = keep;
    }
    const r3d_dynamic_source_t source = {positions,        (const uint8_t (*)[3])colors,
                                         m.vertex_count,   (const uint16_t (*)[3])triangles,
                                         m.triangle_count, false};
    (void)r3d_dynamic_mesh_build(&surface, &source);
    (void)scene_renderer_set_mesh(tank_scene, water_entity, &surface.mesh);
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
    draw_clear(gfx_rgb(UI_BACKGROUND));
    failure[0] = '\0';
    box = (vec3f_t){display_width_mm(), display_height_mm(), BOARD_THICKNESS_MM};
    scene_failure_t why;
    tank_scene = scene_load(WATER_SCENE, &why);
    if (tank_scene == NULL) {
        fail(why.asset == ASSET_ERR_NOT_FOUND || why.asset == ASSET_ERR_NO_PACK ? "no water pack: flash it"
                                                                                : "water did not load");
        return;
    }
    camera_entity = scene_find(tank_scene, WATER_CAMERA);
    water_entity = scene_find(tank_scene, WATER_ENTITY);
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
    carried_ms = 0;
    tour_ms = 0;
    refill((vec3f_t){0.0F, 1.0F, 0.0F});
    rebuild_surface();
    place_camera((vec3f_t){0.0F, 1.0F, 0.0F});
    (void)scene_activate(tank_scene, WATER_CAMERA);
    render_lab_apply_raster_settings();
    (void)render_lab_apply_camera_settings(tank_scene, camera_entity);
    apply_view();
}

static void
scene_water_exit(void) {
    scene_unload(tank_scene);
    tank_scene = NULL;
    release();
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
    const vec3f_t accel = vec3f_scale(accel_g, (float)gravity_mm_s2);
    carried_ms += dt_ms;
    uint32_t steps = 0;
    const int64_t started = timing_now_us();
    while (carried_ms >= WATER_SUBSTEP_MS && steps < WATER_MAX_SUBSTEPS) {
        flip3_step(&water, (float)WATER_SUBSTEP_MS / (float)MATH_MS_PER_S, accel);
        carried_ms -= WATER_SUBSTEP_MS;
        steps++;
    }
    if (steps == WATER_MAX_SUBSTEPS) {
        carried_ms = 0;
    }
    rebuild_surface();
    step_us = (uint32_t)(timing_now_us() - started);
    place_camera(down);
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

static const char*
water_status(void) {
    static char buf[RENDER_LAB_STATUS_LEN];
    if (failure[0] != '\0') {
        return failure;
    }
    if (snprintf(buf, sizeof buf, "%d%% %d tris %u.%u ms", fill_percent, surface.mesh.triangle_count,
                 (unsigned)(step_us / MATH_US_PER_MS), (unsigned)((step_us % MATH_US_PER_MS) / 100U))
        < 0) {
        buf[0] = '\0';
    }
    return buf;
}

/* `water fill <percent>` and `water tour`, which restarts the scripted tank. */
static inline bool
scene_water_console(const char* args, console_reply_fn reply) {
    int percent;
    char line[RENDER_LAB_STATUS_LEN];
    if (sscanf(args, "water fill %d", &percent) == 1) {
        if (percent < WATER_FILL_MIN || percent > WATER_FILL_MAX) {
            if (snprintf(line, sizeof line, "RENDER_ERR water fill is %d to %d", WATER_FILL_MIN, WATER_FILL_MAX) >= 0) {
                reply(line);
            }
            return true;
        }
        fill_percent = percent;
    } else if (strcmp(args, "water tour") == 0) {
        touring = true;
        tour_ms = 0;
        filled_percent = 0; /* refill upright as the tour starts */
    } else {
        return false;
    }
    if (snprintf(line, sizeof line, "RENDER water fill=%d tour=%d", fill_percent, touring) >= 0) {
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
                                        .status = water_status,
                                        .update = scene_water_update,
                                        .controls = scene_water_controls,
                                        .console = RENDER_LAB_SCENE_CONSOLE(scene_water_console)};
