/*
 * r3d_span_textured: r3d_texture's span fill. A triangle's u, v and light
 * level are planes set up in float once; each span steps them in fixed point,
 * u and v packed in one word. u and v divide by depth every texture.divide_px
 * pixels or more (render settings) and step affine between; 0 steps affine
 * across the span. A pixel is one colormap load: the texel's colour at the
 * vertex light's dithered level.
 */
#include "render/texture/r3d_texture.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "image/pixel/bayer.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "profile/frame_laps.h"
#include "render/r3d_span_internal.h"

/* u and v walk as 8.8 texels, packed u high and v low in one word; a carry
 * out of v moves u by 1/256 texel. 8 integer bits wrap at 256 texels, a
 * multiple of every level's side. */
#define UV_FRAC      8
#define UV_ONE       (1 << UV_FRAC)
#define UV_WRAP      256.0F
#define UV_HALF_BITS 16
#define UV_LOW_MASK  0xFFFFU
#define U_INT_SHIFT  (UV_HALF_BITS + UV_FRAC)
#define V_INT_SHIFT  UV_FRAC
#define NIBBLE_BITS  4
#define NIBBLE_MASK  0xFU
/* The light level walks as 8.8. */
#define LIGHT_FRAC   8
#define LEVEL_MAX    ((R3D_TEXTURE_LIGHT_LEVELS - 1) << LIGHT_FRAC)
/* A Bayer threshold, 0..15, in 1/256 of a level. */
#define DITHER_SHIFT (LIGHT_FRAC - 4)
#define DEPTH_MAX    ((int32_t)R3D_DEPTH_NEAREST << R3D_DEPTH_SHIFT)
#define CORNERS      3
/* Affine bends u and v by about the triangle's texel extent times the
 * relative spread of its inverse depth, divided by this. */
#define BEND_SPREAD  4.0F

/* 1 / n for every segment length n, so a span's short last segment costs no divide. */
#define INVERSE_1(n) (1.0F / (float)(n)),
#define INVERSE_8(n)                                                                                                   \
    INVERSE_1(n)                                                                                                       \
    INVERSE_1((n) + 1)                                                                                                 \
    INVERSE_1((n) + 2) INVERSE_1((n) + 3) INVERSE_1((n) + 4) INVERSE_1((n) + 5) INVERSE_1((n) + 6) INVERSE_1((n) + 7)
static const float segment_inverse[] = {0.0F, INVERSE_8(1) INVERSE_8(9) INVERSE_8(17) INVERSE_8(25)};
_Static_assert(sizeof segment_inverse / sizeof segment_inverse[0] == RENDER_TEXTURE_DIVIDE_PX_MAX + 1,
               "an inverse for every segment length");

_Static_assert(BAYER4_LEVELS << DITHER_SHIFT == 1 << LIGHT_FRAC, "a threshold stays under one level");
_Static_assert((1 << (UV_HALF_BITS - UV_FRAC)) == (int)UV_WRAP, "u and v wrap at their integer bits");

static inline float
plane_at(const r3d_span_plane_t* p, float x, float y) {
    return p->base + (p->dx * x) + (p->dy * y);
}

/* What every span of one triangle reads. */
typedef struct {
    const uint8_t* texels;
    uint32_t mask;     /* the level's side less one */
    uint32_t row_mask; /* mask, moved up to v's place in a texel offset */
    uint32_t v_shift;  /* what lands v's integer bits at that place */
    const r3d_texture_t* texture;
} sampler_t;

/* The values a span carries from one segment to the next. */
typedef struct {
    int32_t z, dz;
    uint32_t uv, duv;
    int32_t level, dlevel;
} carry_t;

static inline __attribute__((always_inline)) uint32_t
texel(const sampler_t* s, uint32_t uv) {
    const uint32_t o = ((uv >> s->v_shift) & s->row_mask) | ((uv >> U_INT_SHIFT) & s->mask);
    return (uint32_t)(s->texels[o >> 1] >> ((o & 1U) * NIBBLE_BITS)) & NIBBLE_MASK;
}

/* u as 8.8, wrapped into [0, UV_WRAP) first so the half word holds it. */
static inline int32_t
fixed_uv(float u) {
    const float wrapped = u - (floorf(u * (1.0F / UV_WRAP)) * UV_WRAP);
    return (int32_t)(wrapped * (float)UV_ONE) & (int32_t)UV_LOW_MASK;
}

static inline uint32_t
pack_uv(int32_t u, int32_t v) {
    return ((uint32_t)u << UV_HALF_BITS) + (uint32_t)v;
}

/* `count` pixels of the colormap row of the level, dithered between levels
 * by the screen's 4x4 Bayer threshold at absolute coordinates. */
static inline __attribute__((always_inline)) void
colormap_run(uint16_t* depth, uint16_t* color, int count, carry_t* c, const sampler_t* s, int x,
             const uint8_t* threshold) {
    int32_t z = c->z;
    uint32_t uv = c->uv;
    int32_t level = c->level;
    const int32_t dz = c->dz;
    const uint32_t duv = c->duv;
    const int32_t dlevel = c->dlevel;
    const uint16_t (*colormap)[R3D_TEXTURE_COLOURS] = s->texture->colormap;
    for (int i = 0; i < count; i++) {
        const uint16_t zq = (uint16_t)(z >> R3D_DEPTH_SHIFT);
        if (zq > depth[i]) {
            const int32_t l = (level + ((int32_t)threshold[(x + i) & (BAYER4_SIDE - 1)] << DITHER_SHIFT)) >> LIGHT_FRAC;
            depth[i] = zq;
            color[i] = colormap[l][texel(s, uv)];
        }
        z += dz;
        uv += duv;
        level += dlevel;
    }
    c->z = z;
    c->uv = uv;
    c->level = level;
}

/* A span stepped affine the whole way. */
static __attribute__((noinline)) void
fill_colormap(uint16_t* depth, uint16_t* color, int count, carry_t* c, const sampler_t* s, int x, int y) {
    colormap_run(depth, color, count, c, s, x, bayer4x4[y & (BAYER4_SIDE - 1)]);
}

/* What the set-up hands the row walk. */
typedef struct {
    r3d_span_plane_t z;     /* 16.8 depth */
    r3d_span_plane_t s, t;  /* u, v in the level's texels, times q when dividing */
    r3d_span_plane_t q;     /* inverse depth, which u and v divide by */
    r3d_span_plane_t level; /* 8.8 light level */
    int32_t depth_max;
    int x0, y0; /* the origin pixel */
    int divide; /* pixels between divides, 0 when the triangle steps affine */
    sampler_t sampler;
} setup_t;

/* A span whose u and v divide by depth every su->divide pixels, from s = u q,
 * t = v q and q at its first pixel. u and v wrap once at the start and then
 * step in fixed point; each divide aims the next segment's step at the true
 * u and v, unwrapped, from where the steps so far reached. */
static __attribute__((noinline)) void
fill_divided(const setup_t* su, uint16_t* depth, uint16_t* color, int count, carry_t* c, float s, float t, float q,
             int x, int y) {
    const uint8_t* threshold = bayer4x4[y & (BAYER4_SIDE - 1)];
    float r = mathf_recip_approx(q);
    float u_at = s * r * (float)UV_ONE;
    float v_at = t * r * (float)UV_ONE;
    c->uv = pack_uv(fixed_uv(s * r), fixed_uv(t * r));
    for (int done = 0; done < count;) {
        const int n = mathi_min(su->divide, count - done);
        const float steps = (float)n;
        const float inverse = segment_inverse[n];
        s += su->s.dx * steps;
        t += su->t.dx * steps;
        q += su->q.dx * steps;
        r = mathf_recip_approx(q);
        const int32_t du = (int32_t)(((s * r * (float)UV_ONE) - u_at) * inverse);
        const int32_t dv = (int32_t)(((t * r * (float)UV_ONE) - v_at) * inverse);
        c->duv = pack_uv(du, 0) + (uint32_t)dv;
        colormap_run(depth + done, color + done, n, c, &su->sampler, x + done, threshold);
        u_at += (float)(du * n);
        v_at += (float)(dv * n);
        done += n;
    }
}

static void
fill_span(const setup_t* su, const r3d_span_row_t* row, int y, int first, int last) {
    const float fx = (float)(first - su->x0);
    const float fy = (float)(y - su->y0);
    const int steps = last - first;
    carry_t c;
    c.z = (int32_t)mathf_clamp(plane_at(&su->z, fx, fy), -2.0e9F, 2.0e9F);
    c.dz = (int32_t)su->z.dx;
    r3d_span_clamp_to(su->depth_max, steps, &c.z, &c.dz);
    c.level = (int32_t)mathf_clamp(plane_at(&su->level, fx, fy), -2.0e9F, 2.0e9F);
    c.dlevel = (int32_t)su->level.dx;
    r3d_span_clamp_to(LEVEL_MAX, steps, &c.level, &c.dlevel);
    uint16_t* depth = row->depth + first;
    uint16_t* color = row->color + first;
    float s = plane_at(&su->s, fx, fy);
    float t = plane_at(&su->t, fx, fy);
    if (su->divide == 0) {
        c.uv = pack_uv(fixed_uv(s), fixed_uv(t));
        c.duv = pack_uv((int32_t)(su->s.dx * (float)UV_ONE), 0) + (uint32_t)(int32_t)(su->t.dx * (float)UV_ONE);
        fill_colormap(depth, color, steps + 1, &c, &su->sampler, first, y);
        return;
    }
    fill_divided(su, depth, color, steps + 1, &c, s, t, plane_at(&su->q, fx, fy), first, y);
}

/* The level a pixel steps about one texel of: floor(log2) of the longest
 * texel step, in level-0 texels, so the first level whose next one would
 * need a step of 2^(level + 1) or more. */
static int
pick_level(const setup_t* su, const r3d_texture_t* texture) {
    const float dudx = su->s.dx;
    const float dvdx = su->t.dx;
    const float dudy = su->s.dy;
    const float dvdy = su->t.dy;
    const float rho2 = mathf_max((dudx * dudx) + (dvdx * dvdx), (dudy * dudy) + (dvdy * dvdy));
    int level = 0;
    while (level + 1 < texture->levels && rho2 >= (float)(1U << (2 * (level + 1)))) {
        level++;
    }
    return level;
}

/* A vertex colour's luminance, 0..255, as an 8.8 level, 255 the last. */
static inline float
light_level(const r3d_span_vertex_t* v) {
    const float luma =
        (((float)R3D_TEXTURE_LUMA_R * v->r) + ((float)R3D_TEXTURE_LUMA_G * v->g) + ((float)R3D_TEXTURE_LUMA_B * v->b))
        * (1.0F / (float)(1 << R3D_TEXTURE_LUMA_SHIFT));
    return luma * ((float)LEVEL_MAX / (float)UINT8_MAX);
}

int
r3d_texture_segment_px(int divide_px, float affine_bend, float extent, float z_near, float z_far, float side) {
    const float bent = extent * (z_near - z_far);
    const float allowed = affine_bend * BEND_SPREAD * z_far;
    if (divide_px <= 0 || bent <= allowed) {
        return 0;
    }
    /* A segment of n pixels bends by (n / side)^2 of the whole: double n while that stays allowed. */
    int divide = divide_px;
    while (divide * 2 <= RENDER_TEXTURE_DIVIDE_PX_MAX && bent * (float)(4 * divide * divide) <= allowed * side * side) {
        divide *= 2;
    }
    return divide;
}

/* Sets `su` up to fill the triangle's part in window box `box`. Whether it
 * divides, and how far apart, are judged on `whole`, its unclipped box, so
 * every window that draws a part of it maps u and v alike. */
static bool
set_up(const r3d_span_target_t* target, const r3d_span_vertex_t* const v[CORNERS], const vec2f_t uv[CORNERS],
       const r3d_texture_t* texture, recti_t box, recti_t whole, setup_t* su) {
    r3d_span_frame_t f;
    if (!r3d_span_frame_of(v[0], v[1], v[2], box.x, box.y, &f)) {
        return false;
    }
    su->x0 = box.x;
    su->y0 = box.y;
    const float depth_scale = (float)DEPTH_MAX;
    su->z = r3d_span_plane_of(&f, v[0]->z * depth_scale, v[1]->z * depth_scale, v[2]->z * depth_scale);
    const uint16_t cap =
        target->skip_hidden ? r3d_span_depth_limit(mathf_max(v[0]->z, mathf_max(v[1]->z, v[2]->z))) : R3D_DEPTH_NEAREST;
    su->depth_max = cap == R3D_DEPTH_NEAREST ? DEPTH_MAX : (((int32_t)cap << R3D_DEPTH_SHIFT) | 0xFF);
    /* u and v from the texture's tile holding vertex 0, so a far tile's
     * large u keeps its fraction through the float planes and divides. */
    const float tile = (float)(1U << texture->size_log2);
    const vec2f_t origin = {floorf(uv[0].x / tile) * tile, floorf(uv[0].y / tile) * tile};
    vec2f_t local[CORNERS];
    for (int i = 0; i < CORNERS; i++) {
        local[i] = (vec2f_t){uv[i].x - origin.x, uv[i].y - origin.y};
    }
    su->s = r3d_span_plane_of(&f, local[0].x, local[1].x, local[2].x);
    su->t = r3d_span_plane_of(&f, local[0].y, local[1].y, local[2].y);
    const int level = pick_level(su, texture);
    const float scale = 1.0F / (float)(1U << level);
    su->s = (r3d_span_plane_t){su->s.base * scale, su->s.dx * scale, su->s.dy * scale};
    su->t = (r3d_span_plane_t){su->t.base * scale, su->t.dx * scale, su->t.dy * scale};
    const float z_near = mathf_max(v[0]->z, mathf_max(v[1]->z, v[2]->z));
    const float z_far = mathf_min(v[0]->z, mathf_min(v[1]->z, v[2]->z));
    const float extent = mathf_max(mathf_abs(su->s.dx) + mathf_abs(su->t.dx), mathf_abs(su->s.dy) + mathf_abs(su->t.dy))
                         * (float)mathi_max(whole.w, whole.h);
    su->divide = r3d_texture_segment_px(target->texture_divide_px,
                                        (float)target->texture_bend_steps * (1.0F / (float)R3D_SPAN_BEND_STEPS), extent,
                                        z_near, z_far, (float)mathi_max(whole.w, whole.h));
    if (su->divide != 0) {
        /* The level stays the affine planes' pick; u and v become u q and v q. */
        su->q = r3d_span_plane_of(&f, v[0]->z, v[1]->z, v[2]->z);
        su->s = r3d_span_plane_of(&f, local[0].x * scale * v[0]->z, local[1].x * scale * v[1]->z,
                                  local[2].x * scale * v[2]->z);
        su->t = r3d_span_plane_of(&f, local[0].y * scale * v[0]->z, local[1].y * scale * v[1]->z,
                                  local[2].y * scale * v[2]->z);
    }
    const int side_log2 = texture->size_log2 - level;
    const uint32_t mask = (1U << side_log2) - 1U;
    su->sampler =
        (sampler_t){texture->texels[level], mask, mask << side_log2, (uint32_t)(V_INT_SHIFT - side_log2), texture};
    su->level = r3d_span_plane_of(&f, light_level(v[0]), light_level(v[1]), light_level(v[2]));
    return true;
}

/* r3d_span's walk: the long edge v0-v2 runs the whole height, and v1 lies
 * right of it when v0-v1-v2 winds positive; the short edge is v0-v1 above
 * v1's row and v1-v2 from it. */
void
r3d_span_textured_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                           const r3d_span_vertex_t* c, const vec2f_t uv[CORNERS], const r3d_texture_t* texture) {
    const recti_t whole = r3d_span_extent_limits(a, b, c, 0, 0).centres;
    const recti_t box = r3d_span_window_box(target, whole);
    const int32_t area2 = r3d_span_area2(a, b, c);
    if (recti_is_empty(box) || area2 == 0 || r3d_span_stop_after != 0) {
        return;
    }
    setup_t su;
    if (!set_up(target, (const r3d_span_vertex_t* const[CORNERS]){a, b, c}, uv, texture, box, whole, &su)) {
        return;
    }
    const r3d_span_vertex_t* v0 = a;
    const r3d_span_vertex_t* v1 = b;
    const r3d_span_vertex_t* v2 = c;
    const bool long_on_left = (area2 > 0) != r3d_span_sort_by_y(&v0, &v1, &v2);
    const int end = recti_bottom(box);
    const int split = mathi_clamp(r3d_span_first_centre(v1->y), box.y, end);
    const int last_column = target->rows.attachment[GFX_ATTACHMENT_COLOR].width - 1;
    r3d_span_walk_edge_t long_edge = r3d_span_walk_edge_at(v0, v2, box.y);
    r3d_span_walk_edge_t short_edge =
        box.y < split ? r3d_span_walk_edge_at(v0, v1, box.y) : r3d_span_walk_edge_at(v1, v2, box.y);
    FRAME_LAP("r3d.draw.fill");
    for (int y = box.y; y < end; y++) {
        if (y == split && box.y < split) {
            short_edge = r3d_span_walk_edge_at(v1, v2, y);
        }
        const r3d_span_walk_edge_t* left = long_on_left ? &long_edge : &short_edge;
        const r3d_span_walk_edge_t* right = long_on_left ? &short_edge : &long_edge;
        const int first = mathi_max(left->q, 0);
        const int last = mathi_min(right->q - 1, last_column);
        if (first <= last) {
            const r3d_span_row_t row = r3d_span_row(target, y);
            fill_span(&su, &row, y, first, last);
        }
        r3d_span_walk_edge_step(&long_edge);
        r3d_span_walk_edge_step(&short_edge);
    }
}
