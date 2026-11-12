#include "apps/render_lab/stress/stress_preset.h"

#include <math.h>
#include <string.h>

#include "math/geometry/primitive_mesh.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathi.h"
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
    [STRESS_TEXTURED] = {STRESS_SHAPE_PLANE, 2, 1, STRESS_LAYOUT_WALL, 100, STRESS_SURFACE_OF(TEXTURED), STRESS_CAMERA,
                         6},
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

bool
stress_source(const stress_params_t* p, uint32_t t_ms, const stress_buffers_t* b, r3d_dynamic_source_t* out) {
    primitive_mesh_t m = {b->positions, stress_vertex_count(p), b->triangles, stress_triangle_count(p), 0, 0};
    if (!make(p, b, &m)) {
        return false;
    }
    colour_and_wave(p, t_ms, m.vertex_count, b);
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
        case STRESS_SHAPE_GRID:
        case STRESS_SHAPE_CARD:
        case STRESS_SHAPE_PLANE:
        case STRESS_SHAPE_CUBE: return width / (float)p->cuts;
        case STRESS_SHAPE_SPHERE: return MATH_PI * width / (float)sphere_segments(p->cuts);
        case STRESS_SHAPE_CYLINDER: return 0.5F * width;
    }
    return 0.0F;
}

/* The brick: courses STRESS_BRICK_TALL texels tall of bricks STRESS_BRICK_LONG
 * long, every other course shifted half a brick, a texel of mortar under and
 * after each; each brick takes one of the brick colours by hash. */
#define STRESS_BRICK_LONG     16
#define STRESS_BRICK_TALL     8
#define STRESS_BRICK_SEED     0xB21C7U
#define STRESS_MORTAR         0
#define STRESS_BRICK_COLOURS  (R3D_TEXTURE_COLOURS - 1)
#define STRESS_BRICK_TINT     128
#define STRESS_MORTAR_LEVEL   176
#define STRESS_BRICK_LOW      72
#define STRESS_BRICK_STEP     8
#define STRESS_TEXTURE_SIDE   (1 << STRESS_TEXTURE_SIZE_LOG2)
#define STRESS_TEXTURE_LEVELS (STRESS_TEXTURE_SIZE_LOG2 - R3D_TEXTURE_SIZE_LOG2_MIN + 1)
#define STRESS_PALETTE_BYTES  (R3D_TEXTURE_COLOURS * R3D_TEXTURE_PALETTE_STRIDE)

static int
brick_index(int x, int y) {
    const int course = y / STRESS_BRICK_TALL;
    const int along = x + ((course % 2) * (STRESS_BRICK_LONG / 2));
    if (y % STRESS_BRICK_TALL == STRESS_BRICK_TALL - 1 || along % STRESS_BRICK_LONG == STRESS_BRICK_LONG - 1) {
        return STRESS_MORTAR;
    }
    const uint32_t brick = (uint32_t)((along / STRESS_BRICK_LONG) % (STRESS_TEXTURE_SIDE / STRESS_BRICK_LONG));
    return 1 + (int)(rng_hash(STRESS_BRICK_SEED, brick, (uint32_t)course, 0) % STRESS_BRICK_COLOURS);
}

static size_t
texel_bytes(void) {
    size_t bytes = 0;
    for (int l = 0; l < STRESS_TEXTURE_LEVELS; l++) {
        bytes += r3d_texture_level_bytes(STRESS_TEXTURE_SIZE_LOG2 - l);
    }
    return bytes;
}

size_t
stress_texture_bytes(void) {
    return R3D_TEXTURE_HEADER_SIZE + STRESS_PALETTE_BYTES + texel_bytes();
}

static void
put_u32(uint8_t* at, uint32_t value) {
    memcpy(at, &value, sizeof value);
}

/* Mortar is pale; bricks run a warm ramp. A level's texel is its 2x2
 * block's top-left in the level above, sampled from level 0. */
void
stress_texture_write(uint8_t* ltex) {
    memset(ltex, 0, stress_texture_bytes());
    const uint16_t version = R3D_TEXTURE_VERSION;
    const uint16_t format = R3D_TEXTURE_INDEX4;
    memcpy(ltex + R3D_TEXTURE_AT_VERSION, &version, sizeof version);
    memcpy(ltex + R3D_TEXTURE_AT_FORMAT, &format, sizeof format);
    ltex[R3D_TEXTURE_AT_SIZE_LOG2] = STRESS_TEXTURE_SIZE_LOG2;
    ltex[R3D_TEXTURE_AT_LEVELS] = STRESS_TEXTURE_LEVELS;
    memset(ltex + R3D_TEXTURE_AT_TINT, STRESS_BRICK_TINT, R3D_TEXTURE_TINT_RGB);
    put_u32(ltex + R3D_TEXTURE_AT_PALETTE_OFF, R3D_TEXTURE_HEADER_SIZE);
    put_u32(ltex + R3D_TEXTURE_AT_TEXELS_OFF, R3D_TEXTURE_HEADER_SIZE + STRESS_PALETTE_BYTES);
    uint8_t* palette = ltex + R3D_TEXTURE_HEADER_SIZE;
    for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
        const int level = i == STRESS_MORTAR ? STRESS_MORTAR_LEVEL : STRESS_BRICK_LOW + (STRESS_BRICK_STEP * i);
        uint8_t* entry = palette + (i * R3D_TEXTURE_PALETTE_STRIDE);
        entry[0] = (uint8_t)mathi_min(level + (level / 4), UINT8_MAX);
        entry[1] = (uint8_t)level;
        entry[2] = (uint8_t)(level - (level / 4));
    }
    uint8_t* texels = palette + STRESS_PALETTE_BYTES;
    for (int l = 0; l < STRESS_TEXTURE_LEVELS; l++) {
        const int side = STRESS_TEXTURE_SIDE >> l;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                const int o = (y * side) + x;
                texels[o >> 1] |= (uint8_t)(brick_index(x << l, y << l) << ((o & 1) * 4));
            }
        }
        texels += r3d_texture_level_bytes(STRESS_TEXTURE_SIZE_LOG2 - l);
    }
}

void
stress_texture_uvs(const r3d_lit_mesh_t* mesh, int16_t (*uvs)[2]) {
    const float to_texels = (float)(STRESS_TEXTURE_SIDE * STRESS_TEXTURE_REPEATS * (1 << R3D_TEXTURE_UV_SHIFT))
                            / (float)mesh->position_scale;
    const float half = 0.5F * (float)mesh->position_scale;
    for (int v = 0; v < mesh->vertex_count; v++) {
        uvs[v][0] = (int16_t)lroundf(((float)mesh->positions[v][0] + half) * to_texels);
        uvs[v][1] = (int16_t)lroundf((half - (float)mesh->positions[v][1]) * to_texels);
    }
}
