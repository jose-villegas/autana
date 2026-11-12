#include "apps/render_lab/stress/stress_preset.h"

#include <math.h>

#include "math/geometry/primitive_mesh.h"
#include "math/scalar/math_const.h"
#include "math/scalar/rng.h"

/* Each preset's surface, as STRESS_PRESETS names it. */
#define STRESS_PRESET_SURFACE(id_, key_, surface_) STRESS_##id_##_SURFACE = STRESS_SURFACE_##surface_,

enum { STRESS_PRESETS(STRESS_PRESET_SURFACE) };

#define STRESS_SURFACE_OF(id_) ((stress_surface_t)STRESS_##id_##_SURFACE)

/* Shape, cuts, copies, layout, coverage percent, surface, camera and replay poses. */
const stress_params_t stress_presets[STRESS_PRESET_COUNT] = {
    [STRESS_OPEN] = {STRESS_SHAPE_PLANE, 8, 16, STRESS_LAYOUT_WALL, 60, STRESS_SURFACE_OF(OPEN), STRESS_CAMERA, 6},
    [STRESS_STACK] = {STRESS_SHAPE_PLANE, 8, 8, STRESS_LAYOUT_STACK, 90, STRESS_SURFACE_OF(STACK), STRESS_CAMERA, 6},
    [STRESS_TINY_COPIES] = {STRESS_SHAPE_SPHERE, 4, 64, STRESS_LAYOUT_WALL, 40, STRESS_SURFACE_OF(TINY_COPIES),
                            STRESS_CAMERA, 6},
    [STRESS_BIG_TRIS] = {STRESS_SHAPE_PLANE, 2, 1, STRESS_LAYOUT_WALL, 100, STRESS_SURFACE_OF(BIG_TRIS), STRESS_CAMERA,
                         6},
    [STRESS_PER_FRAME] = {STRESS_SHAPE_PLANE, 24, 1, STRESS_LAYOUT_WALL, 80, STRESS_SURFACE_OF(PER_FRAME),
                          STRESS_CAMERA, 6},
    [STRESS_TWO_SIDED] = {STRESS_SHAPE_CUBE, 4, 16, STRESS_LAYOUT_WALL, 60, STRESS_SURFACE_OF(TWO_SIDED), STRESS_CAMERA,
                          6},
    [STRESS_MIX] = {STRESS_SHAPE_GRID, 2, 16, STRESS_LAYOUT_WALL, 80, STRESS_SURFACE_OF(MIX), STRESS_CAMERA, 6},
    [STRESS_NOISY_TINY] = {STRESS_SHAPE_CYLINDER, 16, 36, STRESS_LAYOUT_WALL, 75, STRESS_SURFACE_OF(NOISY_TINY),
                           STRESS_CAMERA, 6},
    [STRESS_STATIC_COPIES] = {STRESS_SHAPE_PLANE, 1, 64, STRESS_LAYOUT_WALL, 80, STRESS_SURFACE_OF(STATIC_COPIES),
                              STRESS_CAMERA, 6},
    [STRESS_UNIFORM_MID] = {STRESS_SHAPE_PLANE, 2, 1, STRESS_LAYOUT_STACK, 70, STRESS_SURFACE_OF(UNIFORM_MID),
                            STRESS_CAMERA, 4},
    [STRESS_UNIFORM_FULL] = {STRESS_SHAPE_PLANE, 2, 1, STRESS_LAYOUT_STACK, 160, STRESS_SURFACE_OF(UNIFORM_FULL),
                             STRESS_CAMERA, 4},
    [STRESS_CARD] = {STRESS_SHAPE_CARD, 16, 16, STRESS_LAYOUT_WALL, 140, STRESS_SURFACE_OF(CARD), STRESS_CAMERA, 4},
    [STRESS_CULLED] = {STRESS_SHAPE_PLANE, 16, 1, STRESS_LAYOUT_WALL, 80, STRESS_SURFACE_OF(CULLED),
                       STRESS_CAMERA_CULLED, 8},
    [STRESS_NEAR] = {STRESS_SHAPE_SPHERE, 24, 4, STRESS_LAYOUT_STACK, 80, STRESS_SURFACE_OF(NEAR), STRESS_CAMERA_NEAR,
                     6},
    [STRESS_FEW_COPIES] = {STRESS_SHAPE_SPHERE, 24, 2, STRESS_LAYOUT_WALL, 70, STRESS_SURFACE_OF(FEW_COPIES),
                           STRESS_CAMERA, 6},
    /* Foliage cards near, a few bushes over the whole view, and far, many small
     * ones; each beside its opaque twin, whose cards differ only by the mask. */
    [STRESS_FOLIAGE_NEAR] = {STRESS_SHAPE_FOLIAGE, 24, 3, STRESS_LAYOUT_STACK, 120, STRESS_SURFACE_OF(FOLIAGE_NEAR),
                             STRESS_CAMERA, 6},
    [STRESS_FOLIAGE_NEAR_OPAQUE] = {STRESS_SHAPE_FOLIAGE, 24, 3, STRESS_LAYOUT_STACK, 120,
                                    STRESS_SURFACE_OF(FOLIAGE_NEAR_OPAQUE), STRESS_CAMERA, 6},
    [STRESS_FOLIAGE_FAR] = {STRESS_SHAPE_FOLIAGE, 24, 36, STRESS_LAYOUT_WALL, 70, STRESS_SURFACE_OF(FOLIAGE_FAR),
                            STRESS_CAMERA, 6},
    [STRESS_FOLIAGE_FAR_OPAQUE] = {STRESS_SHAPE_FOLIAGE, 24, 36, STRESS_LAYOUT_WALL, 70,
                                   STRESS_SURFACE_OF(FOLIAGE_FAR_OPAQUE), STRESS_CAMERA, 6},
};

stress_params_t
stress_copy_params(const stress_params_t* p, int copy) {
    static const int cuts[STRESS_MIX_LEVELS] = {2, 8, 24, 48};
    stress_params_t result = *p;
    if (p->shape == STRESS_SHAPE_GRID) {
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

/* The card's mask and texture are independent of any imported scene's bake closure. */
#define STRESS_CARD_HOLE_PERIOD  5
#define STRESS_CARD_TEXTURE_SEED 0xCA4D12U
#define STRESS_CARD_TEXTURE_SIZE 64
#define STRESS_CARD_COLOUR_BASE  32
#define STRESS_CARD_COLOUR_RANGE 160
#define STRESS_CARD_JITTER       0.3F

static bool
card_cell(int x, int y) {
    return rng_hash(STRESS_CARD_TEXTURE_SEED, (uint32_t)x, (uint32_t)y, 0) % STRESS_CARD_HOLE_PERIOD != 0;
}

static int
card_triangles(int cuts) {
    int count = 0;
    for (int y = 0; y < cuts; y++) {
        for (int x = 0; x < cuts; x++) {
            count += card_cell(x, y) ? 2 : 0;
        }
    }
    return count;
}

static float
card_coordinate(int at, int across, int cuts) {
    const uint32_t texel =
        rng_hash(STRESS_CARD_TEXTURE_SEED, (uint32_t)at, (uint32_t)across, 0) % STRESS_CARD_TEXTURE_SIZE;
    const float sample = ((float)texel / (float)(STRESS_CARD_TEXTURE_SIZE - 1)) - 0.5F;
    const float jitter = at == 0 || at == cuts ? 0.0F : STRESS_CARD_JITTER * sample;
    const float u = (2.0F * ((float)at + jitter) / (float)cuts) - 1.0F;
    return (0.5F * u) * fabsf(u);
}

static void
card_corner(const stress_params_t* p, const stress_buffers_t* b, int v, int x, int y) {
    const vec3f_t at = {card_coordinate(x, y, p->cuts), card_coordinate(y, x, p->cuts), 0.0F};
    b->positions[v] = at;
}

static const uint8_t quad_corners[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
#define QUAD_VERTICES            6

/* A foliage card's side, as a share of the bush's unit square, and how deep
 * the bush is: its cards' centres spread over this much of a unit in z. */
#define STRESS_FOLIAGE_CARD_SIDE 0.35F
#define STRESS_FOLIAGE_DEPTH     0.5F
#define STRESS_FOLIAGE_SEED      0xF011A6EU
/* How far a card turns about the vertical either way, radians: upright, as
 * foliage cards stand, and never edge on. */
#define STRESS_FOLIAGE_YAW       1.0F
/* The hash's low bits as a share of [0, 1]. */
#define STRESS_UNIT_BITS         0xFFFFU

/* Card `card`'s draw `site`, a share of [0, 1]. */
static float
foliage_draw(int card, int site) {
    return (float)(rng_hash(STRESS_FOLIAGE_SEED, (uint32_t)card, (uint32_t)site, 0) & STRESS_UNIT_BITS)
           / (float)STRESS_UNIT_BITS;
}

/* Cards placed at random in the unit square, each turned about the vertical
 * and set at a random depth, so they overlap several deep; every card is its
 * own two triangles and six corners. */
static void
make_foliage(const stress_params_t* p, const stress_buffers_t* b) {
    const float half = 0.5F * STRESS_FOLIAGE_CARD_SIDE;
    const float reach = 0.5F - half;
    for (int card = 0; card < p->cuts; card++) {
        const vec3f_t centre = {reach * ((2.0F * foliage_draw(card, 0)) - 1.0F),
                                reach * ((2.0F * foliage_draw(card, 1)) - 1.0F),
                                STRESS_FOLIAGE_DEPTH * (foliage_draw(card, 2) - 0.5F)};
        const float turn = STRESS_FOLIAGE_YAW * ((2.0F * foliage_draw(card, 3)) - 1.0F);
        const float c = cosf(turn);
        const float s = sinf(turn);
        for (int k = 0; k < QUAD_VERTICES; k++) {
            const int v = (QUAD_VERTICES * card) + k;
            const float x = STRESS_FOLIAGE_CARD_SIDE * ((float)quad_corners[k][0] - 0.5F);
            const float y = STRESS_FOLIAGE_CARD_SIDE * ((float)quad_corners[k][1] - 0.5F);
            b->positions[v] = (vec3f_t){centre.x + (c * x), centre.y + y, centre.z - (s * x)};
            b->triangles[(2 * card) + (k / 3)][k % 3] = (uint16_t)v;
        }
    }
}

static void
make_card(const stress_params_t* p, const stress_buffers_t* b) {
    int t = 0;
    for (int cell = 0; cell < p->cuts * p->cuts; cell++) {
        const int x = cell % p->cuts;
        const int y = cell / p->cuts;
        if (!card_cell(x, y)) {
            continue;
        }
        for (int k = 0; k < 6; k++) {
            const int v = (3 * t) + k;
            card_corner(p, b, v, x + quad_corners[k][0], y + quad_corners[k][1]);
            b->triangles[t + (k / 3)][k % 3] = (uint16_t)v;
        }
        t += 2;
    }
}

/* The preset's shape's counts. */
static primitive_counts_t
counts_of(const stress_params_t* p) {
    const shapes_t s = shapes_of(p);
    switch (p->shape) {
        case STRESS_SHAPE_CARD: {
            const int triangles = card_triangles(p->cuts);
            return (primitive_counts_t){3 * triangles, triangles};
        }
        case STRESS_SHAPE_GRID: {
            const int triangles = 2 * p->cuts * p->cuts;
            return (primitive_counts_t){3 * triangles, triangles};
        }
        case STRESS_SHAPE_FOLIAGE: return (primitive_counts_t){QUAD_VERTICES * p->cuts, 2 * p->cuts};
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

static void
make_mixed(const stress_params_t* p, const stress_buffers_t* b) {
    for (int t = 0; t < stress_triangle_count(p); t++) {
        const int cell = t / 2;
        for (int k = 0; k < 3; k++) {
            const int v = (3 * t) + k;
            const uint8_t* corner = quad_corners[(3 * (t % 2)) + k];
            b->positions[v] = (vec3f_t){((float)((cell % p->cuts) + corner[0]) / (float)p->cuts) - 0.5F,
                                        ((float)((cell / p->cuts) + corner[1]) / (float)p->cuts) - 0.5F, 0.0F};
            b->triangles[t][k] = (uint16_t)v;
        }
    }
}

static bool
make(const stress_params_t* p, const stress_buffers_t* b, primitive_mesh_t* m) {
    const shapes_t s = shapes_of(p);
    switch (p->shape) {
        case STRESS_SHAPE_GRID: make_mixed(p, b); break;
        case STRESS_SHAPE_CARD: make_card(p, b); break;
        case STRESS_SHAPE_FOLIAGE: make_foliage(p, b); break;
        case STRESS_SHAPE_PLANE: return primitive_plane(&s.plane, m);
        case STRESS_SHAPE_SPHERE: return primitive_sphere(&s.sphere, m);
        case STRESS_SHAPE_CUBE: return primitive_cube(&s.cube, m);
        case STRESS_SHAPE_CYLINDER: return primitive_cylinder(&s.cylinder, m);
        default: return false;
    }
    m->vertex_count = stress_vertex_count(p);
    m->triangle_count = stress_triangle_count(p);
    return true;
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
    if (p->surface == STRESS_SURFACE_MIXED) {
        return ((v / 3) % STRESS_MIX_UNIFORM_PERIOD) == 0 || v % 3 == 0 ? 0.0F : 1.0F;
    }
    if (p->surface == STRESS_SURFACE_UNIFORM) {
        return 0.0F;
    }
    if (p->surface == STRESS_SURFACE_NOISY) {
        return (float)(v % 2);
    }
    return band((at.y / shape_span(p->shape)) + 0.5F, turns);
}

static void
colour_card(int vertex_count, const stress_buffers_t* b) {
    for (int v = 0; v < vertex_count; v++) {
        const vec3f_t at = b->positions[v];
        const int tx = (int)floorf((at.x + 0.5F) * STRESS_CARD_TEXTURE_SIZE);
        const int ty = (int)floorf((at.y + 0.5F) * STRESS_CARD_TEXTURE_SIZE);
        for (int c = 0; c < 3; c++) {
            b->colors[v][c] = (uint8_t)(STRESS_CARD_COLOUR_BASE
                                        + (rng_hash(STRESS_CARD_TEXTURE_SEED, (uint32_t)tx, (uint32_t)ty, (uint32_t)c)
                                           % STRESS_CARD_COLOUR_RANGE));
        }
    }
}

static void
colour_and_wave(const stress_params_t* p, uint32_t t_ms, int vertex_count, const stress_buffers_t* b) {
    if (p->surface == STRESS_SURFACE_CARD) {
        colour_card(vertex_count, b);
        return;
    }
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

/* One texel in this many inside the disc is a hole: the disc keeps pi/4 of
 * the square, so the mask keeps about 0.7 of a card. */
#define STRESS_MASK_HOLE_PERIOD 10
#define STRESS_MASK_SEED        0x1EAFU

/* Texel (x, y) of the mask: inside the disc and not a hole. Doubled
 * coordinates put the disc's centre between the middle texels. */
static bool
mask_kept(int x, int y) {
    const int dx = (2 * x) + 1 - (int)LMSK_MASK_WIDTH;
    const int dy = (2 * y) + 1 - (int)LMSK_MASK_WIDTH;
    const bool in_disc = (dx * dx) + (dy * dy) < (int)(LMSK_MASK_WIDTH * LMSK_MASK_WIDTH);
    return in_disc && rng_hash(STRESS_MASK_SEED, (uint32_t)x, (uint32_t)y, 0) % STRESS_MASK_HOLE_PERIOD != 0;
}

static uint32_t
mask_row(int y) {
    uint32_t row = 0;
    for (int x = 0; x < (int)LMSK_MASK_WIDTH; x++) {
        row |= mask_kept(x, y) ? 1U << x : 0U;
    }
    return row;
}

/* Made on first use: a pure function of the constants above. */
lmsk_atlas_t
stress_mask_atlas(void) {
    static uint32_t rows[STRESS_MASK_ROWS];
    static bool made;
    if (!made) {
        for (int y = 0; y < STRESS_MASK_ROWS; y++) {
            rows[y] = mask_row(y);
        }
        made = true;
    }
    return (lmsk_atlas_t){rows, STRESS_MASK_ROWS};
}

/* A card corner's u or v across `texels` texels: 0 or the last. */
static uint16_t
mask_corner(uint8_t corner, int texels) {
    return (uint16_t)(corner == 0 ? 0 : (texels << LMSK_UV_FRACTION_BITS) - 1);
}

/* Each card vertex's u, v at its corner of the atlas, the card spanning it. */
static void
mask_cards(int vertex_count, uint16_t (*uvs)[2]) {
    for (int v = 0; v < vertex_count; v++) {
        const uint8_t* corner = quad_corners[v % QUAD_VERTICES];
        uvs[v][0] = mask_corner(corner[0], (int)LMSK_MASK_WIDTH);
        uvs[v][1] = mask_corner(corner[1], STRESS_MASK_ROWS);
    }
}

bool
stress_source(const stress_params_t* p, uint32_t t_ms, const stress_buffers_t* b, r3d_dynamic_source_t* out) {
    primitive_mesh_t m = {b->positions, stress_vertex_count(p), b->triangles, stress_triangle_count(p), 0, 0};
    if (!make(p, b, &m)) {
        return false;
    }
    colour_and_wave(p, t_ms, m.vertex_count, b);
    *out = (r3d_dynamic_source_t){
        .positions = b->positions,
        .colors = (const uint8_t (*)[3])b->colors,
        .vertex_count = m.vertex_count,
        .triangles = (const uint16_t (*)[3])b->triangles,
        .triangle_count = m.triangle_count,
        .double_sided = p->surface == STRESS_SURFACE_TWO_SIDED,
    };
    if (p->surface == STRESS_SURFACE_MASKED) {
        if (b->mask_uvs == NULL) {
            return false;
        }
        mask_cards(m.vertex_count, b->mask_uvs);
        out->mask_uvs = (const uint16_t (*)[2])b->mask_uvs;
        out->mask = stress_mask_atlas();
    }
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
        case STRESS_SHAPE_GRID:
        case STRESS_SHAPE_CARD:
        case STRESS_SHAPE_PLANE:
        case STRESS_SHAPE_CUBE: return width / (float)p->cuts;
        case STRESS_SHAPE_SPHERE: return MATH_PI * width / (float)sphere_segments(p->cuts);
        case STRESS_SHAPE_CYLINDER: return 0.5F * width;
        case STRESS_SHAPE_FOLIAGE: return STRESS_FOLIAGE_CARD_SIDE * width;
    }
    return 0.0F;
}
