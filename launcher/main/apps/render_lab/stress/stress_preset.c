#include "apps/render_lab/stress/stress_preset.h"

#include <math.h>
#include <string.h>

#include "math/geometry/primitive_mesh.h"
#include "math/scalar/math_const.h"

/* Each preset's surface, as STRESS_PRESETS names it. */
#define STRESS_PRESET_SURFACE(id_, key_, surface_) STRESS_##id_##_SURFACE = STRESS_SURFACE_##surface_,

enum { STRESS_PRESETS(STRESS_PRESET_SURFACE) };

#define STRESS_SURFACE_OF(id_) ((stress_surface_t)STRESS_##id_##_SURFACE)

/* Shape, cuts, copies, layout, coverage percent, surface and replay poses. */
const stress_params_t stress_presets[STRESS_PRESET_COUNT] = {
    [STRESS_OPEN] = {STRESS_SHAPE_PLANE, 8, 16, STRESS_LAYOUT_WALL, 60, STRESS_SURFACE_OF(OPEN), 8},
    [STRESS_STACK] = {STRESS_SHAPE_PLANE, 8, 8, STRESS_LAYOUT_STACK, 90, STRESS_SURFACE_OF(STACK), 8},
    [STRESS_TINY_COPIES] = {STRESS_SHAPE_SPHERE, 4, 64, STRESS_LAYOUT_WALL, 40, STRESS_SURFACE_OF(TINY_COPIES), 8},
    [STRESS_BIG_TRIS] = {STRESS_SHAPE_PLANE, 2, 1, STRESS_LAYOUT_WALL, 100, STRESS_SURFACE_OF(BIG_TRIS), 8},
    [STRESS_PER_FRAME] = {STRESS_SHAPE_PLANE, 24, 1, STRESS_LAYOUT_WALL, 80, STRESS_SURFACE_OF(PER_FRAME), 8},
    [STRESS_TWO_SIDED] = {STRESS_SHAPE_CUBE, 4, 16, STRESS_LAYOUT_WALL, 60, STRESS_SURFACE_OF(TWO_SIDED), 8},
    [STRESS_MIX] = {STRESS_SHAPE_PLANE, 2, 16, STRESS_LAYOUT_WALL, 80, STRESS_SURFACE_OF(MIX), 8},
    [STRESS_NOISY_TINY] = {STRESS_SHAPE_CYLINDER, 16, 36, STRESS_LAYOUT_WALL, 75, STRESS_SURFACE_OF(NOISY_TINY), 8},
    [STRESS_STATIC_COPIES] = {STRESS_SHAPE_PLANE, 1, 64, STRESS_LAYOUT_WALL, 80, STRESS_SURFACE_OF(STATIC_COPIES), 8},
    [STRESS_UNIFORM_MID] = {STRESS_SHAPE_PLANE, 2, 1, STRESS_LAYOUT_STACK, 70, STRESS_SURFACE_OF(UNIFORM_MID), 4},
    [STRESS_UNIFORM_FULL] = {STRESS_SHAPE_PLANE, 2, 1, STRESS_LAYOUT_STACK, 160, STRESS_SURFACE_OF(UNIFORM_FULL), 4},
};

stress_params_t
stress_copy_params(const stress_params_t* p, int copy) {
    static const int cuts[STRESS_MIX_LEVELS] = {2, 8, 24, 48};
    stress_params_t result = *p;
    if (p->surface == STRESS_SURFACE_MIXED) {
        result.cuts = cuts[copy % STRESS_MIX_LEVELS];
    }
    return result;
}

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
/* A noisy copy's two colours, every channel as far apart as it goes: no
 * small-fill gate passes corners this far apart. */
static const uint8_t stress_noise_low[3] = {0, 0, 0};
static const uint8_t stress_noise_high[3] = {UINT8_MAX, UINT8_MAX, UINT8_MAX};
#define STRESS_BANDS          3
/* A per-frame copy's wave: the bands run up it once a period, and push its
 * surface out along the normal by up to this much of a unit. */
#define STRESS_WAVE_PERIOD_MS 2000
#define STRESS_WAVE_HEIGHT    0.05F

/* The unit shape's width: the plane's, cube's and cylinder's side, the sphere's diameter. */
static float
shape_span(stress_shape_t shape) {
    return shape == STRESS_SHAPE_SPHERE ? 2.0F : 1.0F;
}

/* A sphere's slices round its axis: twice its bands, so its cuts are as long as they are tall at the equator. */
static int
sphere_segments(int rings) {
    return 2 * rings;
}

/* A cylinder's side in bands no taller than its radius, so no side
 * triangle is longer than an end's slivers. */
#define STRESS_CYLINDER_STACKS 2

/* Each shape's parameters for a preset, one of them the preset's own. */
typedef struct {
    primitive_plane_t plane;
    primitive_sphere_t sphere;
    primitive_cube_t cube;
    primitive_cylinder_t cylinder;
} shapes_t;

static shapes_t
shapes_of(const stress_params_t* p) {
    return (shapes_t){
        .plane = {.subdivisions = p->cuts},
        .sphere = {.segments = sphere_segments(p->cuts), .rings = p->cuts},
        .cube = {.subdivisions = p->cuts},
        .cylinder = {.top_radius = shape_span(STRESS_SHAPE_CYLINDER) / 2.0F,
                     .bottom_radius = shape_span(STRESS_SHAPE_CYLINDER) / 2.0F,
                     .height = shape_span(STRESS_SHAPE_CYLINDER),
                     .segments = p->cuts,
                     .stacks = STRESS_CYLINDER_STACKS},
    };
}

/* The preset's shape's counts. */
static primitive_counts_t
counts_of(const stress_params_t* p) {
    if (p->surface == STRESS_SURFACE_MIXED) {
        const int triangles = 2 * p->cuts * p->cuts;
        return (primitive_counts_t){3 * triangles, triangles};
    }
    const shapes_t s = shapes_of(p);
    switch (p->shape) {
        case STRESS_SHAPE_PLANE: return primitive_plane_counts(&s.plane);
        case STRESS_SHAPE_SPHERE: return primitive_sphere_counts(&s.sphere);
        case STRESS_SHAPE_CUBE: return primitive_cube_counts(&s.cube);
        case STRESS_SHAPE_CYLINDER: return primitive_cylinder_counts(&s.cylinder);
    }
    return (primitive_counts_t){0, 0};
}

int
stress_vertex_count(const stress_params_t* p) {
    return counts_of(p).vertices;
}

int
stress_triangle_count(const stress_params_t* p) {
    return counts_of(p).triangles;
}

static bool
make(const stress_params_t* p, primitive_mesh_t* m) {
    const shapes_t s = shapes_of(p);
    switch (p->shape) {
        case STRESS_SHAPE_PLANE: return primitive_plane(&s.plane, m);
        case STRESS_SHAPE_SPHERE: return primitive_sphere(&s.sphere, m);
        case STRESS_SHAPE_CUBE: return primitive_cube(&s.cube, m);
        case STRESS_SHAPE_CYLINDER: return primitive_cylinder(&s.cylinder, m);
    }
    return false;
}

/* The wave's height at a vertex `up` of the way from the shape's bottom to
 * its top, `turns` periods in: from 0 to 1, in STRESS_BANDS bands. */
static float
band(float up, float turns) {
    return 0.5F + (0.5F * cosf(MATH_TAU * ((up * (float)STRESS_BANDS) - turns)));
}

/* Banded by height, or for a noisy surface its low colour and its high by
 * turns: every triangle has two vertices in a row, so its corners differ by
 * the whole range. */
static float
shade(const stress_params_t* p, int v, vec3f_t at, float turns) {
    if (p->surface == STRESS_SURFACE_UNIFORM) {
        return 0.0F;
    }
    if (p->surface == STRESS_SURFACE_NOISY) {
        return (float)(v % 2);
    }
    return band((at.y / shape_span(p->shape)) + 0.5F, turns);
}

static void
colour_and_wave(const stress_params_t* p, uint32_t t_ms, int vertex_count, const stress_buffers_t* b) {
    const bool per_frame = stress_per_frame(p);
    const float turns = per_frame ? (float)(t_ms % STRESS_WAVE_PERIOD_MS) / (float)STRESS_WAVE_PERIOD_MS : 0.0F;
    const bool noisy = p->surface == STRESS_SURFACE_NOISY;
    const uint8_t* low = noisy ? stress_noise_low : stress_colour_low;
    const uint8_t* high = noisy ? stress_noise_high : stress_colour_high;
    for (int v = 0; v < vertex_count; v++) {
        const float s = shade(p, v, b->positions[v], turns);
        for (int c = 0; c < 3; c++) {
            b->colors[v][c] = (uint8_t)lroundf((float)low[c] + ((float)(high[c] - low[c]) * s));
        }
        if (per_frame) {
            /* The position is the normal only on the unit sphere. */
            const vec3f_t normal = p->shape == STRESS_SHAPE_PLANE ? vec3f_unit_z() : b->positions[v];
            b->positions[v] = vec3f_add(b->positions[v], vec3f_scale(normal, STRESS_WAVE_HEIGHT * ((2.0F * s) - 1.0F)));
        }
    }
}

static void
make_mixed(const stress_params_t* p, const stress_buffers_t* b) {
    static const uint8_t corners[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    for (int t = 0; t < stress_triangle_count(p); t++) {
        const int cell = t / 2;
        for (int k = 0; k < 3; k++) {
            const int v = (3 * t) + k;
            const uint8_t* corner = corners[(3 * (t % 2)) + k];
            b->positions[v] = (vec3f_t){((float)((cell % p->cuts) + corner[0]) / (float)p->cuts) - 0.5F,
                                        ((float)((cell / p->cuts) + corner[1]) / (float)p->cuts) - 0.5F, 0.0F};
            const bool uniform = (t % STRESS_MIX_UNIFORM_PERIOD) == 0;
            memcpy(b->colors[v], uniform || k == 0 ? stress_colour_low : stress_colour_high, sizeof b->colors[v]);
            b->triangles[t][k] = (uint16_t)v;
        }
    }
}

bool
stress_source(const stress_params_t* p, uint32_t t_ms, const stress_buffers_t* b, r3d_dynamic_source_t* out) {
    primitive_mesh_t m = {b->positions, stress_vertex_count(p), b->triangles, stress_triangle_count(p), 0, 0};
    if (p->surface == STRESS_SURFACE_MIXED) {
        make_mixed(p, b);
        m.vertex_count = stress_vertex_count(p);
        m.triangle_count = stress_triangle_count(p);
    } else if (!make(p, &m)) {
        return false;
    }
    if (p->surface != STRESS_SURFACE_MIXED) {
        colour_and_wave(p, t_ms, m.vertex_count, b);
    }
    *out = (r3d_dynamic_source_t){b->positions,     (const uint8_t (*)[3])b->colors,
                                  m.vertex_count,   (const uint16_t (*)[3])b->triangles,
                                  m.triangle_count, p->surface == STRESS_SURFACE_TWO_SIDED};
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
    if (p->shape == STRESS_SHAPE_CYLINDER) {
        /* Its axis, +y, turned to +z: the top's fan faces the camera. */
        transformf_set_rotation(&place, quatf_from_axis_angle(vec3f_unit_x(), MATH_PI * 0.5F));
    }
    transformf_set_scale(&place, (vec3f_t){scale, scale, scale});
    return place;
}

/* A sphere's longest cut is round its equator: its circumference over its
 * segments, as long as a band is tall. A cylinder's is an end's sliver,
 * its radius, as long as a side band is tall. */
float
stress_cut_px(const stress_params_t* p, int short_px) {
    const float width = (float)short_px * (float)p->coverage_percent / STRESS_PERCENT * copy_share(p);
    switch (p->shape) {
        case STRESS_SHAPE_PLANE:
        case STRESS_SHAPE_CUBE: return width / (float)p->cuts;
        case STRESS_SHAPE_SPHERE: return MATH_PI * width / (float)sphere_segments(p->cuts);
        case STRESS_SHAPE_CYLINDER: return 0.5F * width;
    }
    return 0.0F;
}
