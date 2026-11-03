#include "apps/render_lab/stress/stress_preset.h"

#include <math.h>

#include "math/geometry/primitive_mesh.h"
#include "math/scalar/math_const.h"

#define STRESS_PRESET_PARAMS(context_, id_, key_, label_, per_frame_, shape_, cuts_, copies_, layout_, coverage_)      \
    [STRESS_##id_] = {shape_, cuts_, copies_, layout_, coverage_, per_frame_},

const stress_params_t stress_presets[STRESS_PRESET_COUNT] = {STRESS_PRESETS(STRESS_PRESET_PARAMS, )};

/* The share of its cell a wall copy fills: the rest is the gap that keeps
 * copies apart, so nothing on a wall covers anything. */
#define STRESS_WALL_FILL_PERCENT     70
/* The gap from one stacked copy to the next, as a share of their size. */
#define STRESS_STACK_SPACING_PERCENT 10
#define STRESS_PERCENT               100.0F

/* A copy's colours run between these in STRESS_BANDS bands from bottom to
 * top, so a triangle's corners differ as a lit surface's would. */
static const uint8_t stress_colour_low[3] = {0x2A, 0x4F, 0x8C};
static const uint8_t stress_colour_high[3] = {0xE8, 0xB0, 0x5A};
#define STRESS_BANDS          3
/* A per-frame copy's wave: the bands run up it once a period, and push its
 * surface out along the normal by up to this much of a unit. */
#define STRESS_WAVE_PERIOD_MS 2000
#define STRESS_WAVE_HEIGHT    0.05F

/* The unit shape's width: the grid's side, the sphere's diameter. */
static float
shape_span(stress_shape_t shape) {
    return shape == STRESS_SHAPE_GRID ? 1.0F : 2.0F;
}

int
stress_vertex_count(const stress_params_t* p) {
    return p->shape == STRESS_SHAPE_GRID ? primitive_grid_vertex_count(p->cuts)
                                         : primitive_sphere_vertex_count(p->cuts);
}

int
stress_triangle_count(const stress_params_t* p) {
    return p->shape == STRESS_SHAPE_GRID ? primitive_grid_triangle_count(p->cuts)
                                         : primitive_sphere_triangle_count(p->cuts);
}

/* The wave's height at a vertex `up` of the way from the shape's bottom to
 * its top, `turns` periods in: from 0 to 1, in STRESS_BANDS bands. */
static float
band(float up, float turns) {
    return 0.5F + (0.5F * cosf(MATH_TAU * ((up * (float)STRESS_BANDS) - turns)));
}

static void
colour_and_wave(const stress_params_t* p, uint32_t t_ms, int vertex_count, const stress_buffers_t* b) {
    const float turns = p->per_frame ? (float)(t_ms % STRESS_WAVE_PERIOD_MS) / (float)STRESS_WAVE_PERIOD_MS : 0.0F;
    const float span = shape_span(p->shape);
    for (int v = 0; v < vertex_count; v++) {
        const float s = band((b->positions[v].y / span) + 0.5F, turns);
        for (int c = 0; c < 3; c++) {
            b->colors[v][c] =
                (uint8_t)lroundf(stress_colour_low[c] + ((float)(stress_colour_high[c] - stress_colour_low[c]) * s));
        }
        if (p->per_frame) {
            const vec3f_t normal = p->shape == STRESS_SHAPE_GRID ? vec3f_unit_z() : b->positions[v];
            b->positions[v] = vec3f_add(b->positions[v], vec3f_scale(normal, STRESS_WAVE_HEIGHT * ((2.0F * s) - 1.0F)));
        }
    }
}

bool
stress_source(const stress_params_t* p, uint32_t t_ms, const stress_buffers_t* b, r3d_dynamic_source_t* out) {
    primitive_mesh_t m = {b->positions, stress_vertex_count(p), b->triangles, stress_triangle_count(p), 0, 0};
    const bool made = p->shape == STRESS_SHAPE_GRID ? primitive_grid(p->cuts, &m) : primitive_sphere(p->cuts, &m);
    if (!made) {
        return false;
    }
    colour_and_wave(p, t_ms, m.vertex_count, b);
    *out = (r3d_dynamic_source_t){b->positions,     (const uint8_t (*)[3])b->colors,
                                  m.vertex_count,   (const uint16_t (*)[3])b->triangles,
                                  m.triangle_count, false};
    return true;
}

/* The arrangement's size: coverage_percent of what the camera sees across
 * the picture's short side, at the origin. */
static float
extent(const stress_params_t* p, stress_framing_t framing) {
    return 2.0F * framing.distance * framing.half_fov_short_tan * (float)p->coverage_percent / STRESS_PERCENT;
}

/* A wall's columns: the fewest that keep it no taller than it is wide. */
static int
wall_columns(const stress_params_t* p) {
    return (int)ceilf(sqrtf((float)p->copies));
}

/* A front copy's width, as a share of the arrangement's size. */
static float
copy_share(const stress_params_t* p) {
    if (p->layout == STRESS_LAYOUT_STACK) {
        return 1.0F;
    }
    return (float)STRESS_WALL_FILL_PERCENT / STRESS_PERCENT / (float)wall_columns(p);
}

transformf_t
stress_copy_place(const stress_params_t* p, int copy, stress_framing_t framing) {
    const float size = extent(p, framing);
    const float scale = size * copy_share(p) / shape_span(p->shape);
    vec3f_t at = {0.0F, 0.0F, -(float)copy * size * (float)STRESS_STACK_SPACING_PERCENT / STRESS_PERCENT};
    if (p->layout == STRESS_LAYOUT_WALL) {
        const int columns = wall_columns(p);
        const int rows = (p->copies + columns - 1) / columns;
        const float cell = size / (float)columns;
        at = (vec3f_t){((float)(copy % columns) - (0.5F * (float)(columns - 1))) * cell,
                       ((float)(copy / columns) - (0.5F * (float)(rows - 1))) * cell, 0.0F};
    }
    transformf_t place = transformf_identity();
    transformf_set_position(&place, at);
    transformf_set_scale(&place, (vec3f_t){scale, scale, scale});
    return place;
}

/* A sphere's longest cut is round its equator: its circumference over its
 * segments, as long as a band is tall. */
float
stress_cut_px(const stress_params_t* p, int short_px) {
    const float width = (float)short_px * (float)p->coverage_percent / STRESS_PERCENT * copy_share(p);
    if (p->shape == STRESS_SHAPE_GRID) {
        return width / (float)p->cuts;
    }
    return MATH_PI * width / (float)primitive_sphere_segments(p->cuts);
}
