/*
 * r3d_span_textured: r3d_texture's span fill. A triangle's u, v and light
 * level are planes set up in float once; each span steps them in fixed point,
 * u and v packed in one word. An INDEX4 span divides u and v by depth every
 * texture.divide_px pixels or more (render settings) and steps affine
 * between; 0 steps affine across the span. A pixel is one colormap load: the
 * texel's colour at the vertex light's dithered level. An INDEX4 level from
 * texture.far_level on draws untextured in one colour, its corners' mean:
 * the surface's mean lit colour. A MASK1 texture's span steps affine, skips the
 * pixels whose texel is 0 and draws the rest in its one colour.
 */
#include "render/texture/r3d_texture.h"

#include <stdbool.h>
#include <stdint.h>

#include "collections/bits.h"
#include "image/pixel/bayer.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "profile/frame_laps.h"
#include "render/code_layout.h"
#include "render/path/r3d_path.h"
#include "render/texture/r3d_span_textured_internal.h"

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
#define MASK1_KEPT   1 /* the colour a kept MASK1 texel draws */
/* The light level walks as 8.8. */
#define LIGHT_FRAC   8
#define LEVEL_MAX    ((R3D_TEXTURE_LIGHT_LEVELS - 1) << LIGHT_FRAC)
/* A Bayer threshold, 0..15, in 1/256 of a level. */
#define DITHER_SHIFT (LIGHT_FRAC - 4)
#define DEPTH_MAX    ((int32_t)R3D_DEPTH_NEAREST << R3D_DEPTH_SHIFT)
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

_Static_assert(RENDER_TEXTURE_LEVEL_MAX == R3D_TEXTURE_LEVELS_MAX - 1,
               "texture.affine_level and far_level can name every level");
_Static_assert(BAYER4_LEVELS << DITHER_SHIFT == 1 << LIGHT_FRAC, "a threshold stays under one level");
_Static_assert((1 << (UV_HALF_BITS - UV_FRAC)) == (int)UV_WRAP, "u and v wrap at their integer bits");

R3D_PATH(cut, "texture.cut");
R3D_PATH(divided, "texture.divided");
R3D_PATH(affine, "texture.affine");
R3D_PATH(far, "texture.far");

static inline float
plane_at(const r3d_span_plane_t* p, float x, float y) {
    return p->base + (p->dx * x) + (p->dy * y);
}

/* The mip level a sampler reads: level 0's side over its side, in powers of two. */
static inline int
sampler_level(const r3d_span_texture_sampler_t* sampler) {
    return (int)sampler->texture->size_log2 - __builtin_ctz(sampler->mask + 1U);
}

/* The values a span carries from one segment to the next. */
typedef struct {
    int32_t z, dz;
    uint32_t uv, duv;
    int32_t level, dlevel;
} carry_t;

/* The texel under `uv`, counted row-major across the level. */
static inline __attribute__((always_inline)) uint32_t
texel_offset(const r3d_span_texture_sampler_t* s, uint32_t uv) {
    return ((uv >> s->v_shift) & s->row_mask) | ((uv >> U_INT_SHIFT) & s->mask);
}

static inline __attribute__((always_inline)) uint32_t
texel(const r3d_span_texture_sampler_t* s, uint32_t uv) {
    const uint32_t o = texel_offset(s, uv);
    return (uint32_t)(s->texels[o >> 1] >> ((o & 1U) * NIBBLE_BITS)) & NIBBLE_MASK;
}

static inline __attribute__((always_inline)) bool
kept(const r3d_span_texture_sampler_t* s, uint32_t uv) {
    return bits8_get(s->texels, texel_offset(s, uv));
}

/* The colormap row of the level, dithered between levels by the screen's
 * 4x4 Bayer threshold at absolute coordinates; `masked`, only the pixels
 * whose MASK1 texel is kept. */
static inline __attribute__((always_inline)) void
fill_with(uint16_t* depth, uint16_t* color, int count, carry_t* c, const r3d_span_texture_sampler_t* s, int x, int y,
          bool masked) {
    int32_t z = c->z;
    uint32_t uv = c->uv;
    int32_t level = c->level;
    const int32_t dz = c->dz;
    const uint32_t duv = c->duv;
    const int32_t dlevel = c->dlevel;
    const uint8_t* threshold = bayer4x4[y & (BAYER4_SIDE - 1)];
    const uint16_t (*colormap)[R3D_TEXTURE_COLOURS] = s->texture->colormap;
    for (int i = 0; i < count; i++) {
        const uint16_t zq = (uint16_t)(z >> R3D_DEPTH_SHIFT);
        if (zq > depth[i] && (!masked || kept(s, uv))) {
            const int32_t l = (level + ((int32_t)threshold[(x + i) & (BAYER4_SIDE - 1)] << DITHER_SHIFT)) >> LIGHT_FRAC;
            depth[i] = zq;
            color[i] = colormap[l][masked ? MASK1_KEPT : texel(s, uv)];
        }
        z += dz;
        uv += duv;
        level += dlevel;
    }
    c->z = z;
    c->uv = uv;
    c->level = level;
}

static __attribute__((noinline)) void
fill_colormap(uint16_t* depth, uint16_t* color, int count, carry_t* c, const r3d_span_texture_sampler_t* s, int x,
              int y) {
    fill_with(depth, color, count, c, s, x, y, false);
}

/* At the flash tail: a cut texture is rare beside a plain one. */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
fill_masked(uint16_t* depth, uint16_t* color, int count, carry_t* c, const r3d_span_texture_sampler_t* s, int x,
            int y) {
    fill_with(depth, color, count, c, s, x, y, true);
}

/* u as 8.8, wrapped into [0, UV_WRAP) first so the half word holds it. */
static inline int32_t
fixed_uv(float u) {
    /* Library call: fmodf only past MATHF_REPEAT_FAST_QUOTIENT. A span start
     * wraps u and v once, never per pixel. */
    const float wrapped = mathf_repeat(u, UV_WRAP);
    return (int32_t)(wrapped * (float)UV_ONE) & (int32_t)UV_LOW_MASK;
}

static inline uint32_t
pack_uv(int32_t u, int32_t v) {
    return ((uint32_t)u << UV_HALF_BITS) + (uint32_t)v;
}

/* The carry at a span's first pixel, `first` to `last` on row `y`; its u
 * and v only when the span steps affine, as fill_divided() sets its own. */
static inline __attribute__((always_inline)) void
span_carry(const r3d_span_texture_setup_t* su, int y, int first, int last, bool affine, carry_t* c) {
    const float fx = (float)(first - su->x0);
    const float fy = (float)(y - su->y0);
    const int steps = last - first;
    c->z = (int32_t)mathf_clamp(plane_at(&su->z, fx, fy), -2.0e9F, 2.0e9F);
    c->dz = (int32_t)su->z.dx;
    r3d_span_clamp_to(su->depth_max, steps, &c->z, &c->dz);
    c->level = (int32_t)mathf_clamp(plane_at(&su->level, fx, fy), -2.0e9F, 2.0e9F);
    c->dlevel = (int32_t)su->level.dx;
    r3d_span_clamp_to(LEVEL_MAX, steps, &c->level, &c->dlevel);
    if (!affine) {
        c->uv = 0;
        c->duv = 0;
        return;
    }
    c->uv = pack_uv(fixed_uv(plane_at(&su->s, fx, fy)), fixed_uv(plane_at(&su->t, fx, fy)));
    c->duv = pack_uv((int32_t)(su->s.dx * (float)UV_ONE), 0) + (uint32_t)(int32_t)(su->t.dx * (float)UV_ONE);
}

/* An INDEX4 span whose u and v divide by depth every su->divide pixels,
 * from s = u q, t = v q and q at its first pixel. u and v wrap once at the
 * start and then step in fixed point; each divide aims the next segment's
 * step at the true u and v, unwrapped, from where the steps so far reached.
 * In IRAM (RENDER_DIVIDE_ATTR). */
static RENDER_DIVIDE_ATTR __attribute__((noinline)) void
fill_divided(const r3d_span_texture_setup_t* su, uint16_t* depth, uint16_t* color, int count, carry_t* c, float s,
             float t, float q, int x, int y) {
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
        fill_with(depth + done, color + done, n, c, &su->sampler, x + done, y, false);
        u_at += (float)(du * n);
        v_at += (float)(dv * n);
        done += n;
    }
}

/* The target's writers over each run of kept MASK1 texels in the span the
 * carry `c` starts: a hole leaves its pixel, so no writer sees it, even
 * where the stored depth ties the card's. */
static inline __attribute__((always_inline)) void
write_kept_runs(const r3d_span_target_t* target, const r3d_span_texture_sampler_t* s, int y, int first, int last,
                const carry_t* c) {
    uint32_t uv = c->uv;
    int start = first;
    bool in_run = false;
    for (int x = first; x <= last; x++, uv += c->duv) {
        const bool k = kept(s, uv);
        if (k && !in_run) {
            start = x;
        } else if (!k && in_run) {
            r3d_span_write(target, y, start, x - 1, c->z + (c->dz * (start - first)), c->dz);
        }
        in_run = k;
    }
    if (in_run) {
        r3d_span_write(target, y, start, last, c->z + (c->dz * (start - first)), c->dz);
    }
}

/* What a row walk does with each span. */
typedef enum {
    WALK_FILL,    /* the INDEX4 fill, affine */
    WALK_DIVIDED, /* the INDEX4 fill, dividing by depth */
    WALK_CUT,     /* the MASK1 fill: kept texels only */
    WALK_WRITE,   /* the target's writers over what a fill walk drew */
} walk_t;

static inline __attribute__((always_inline)) void
walk_span(const r3d_span_target_t* target, const r3d_span_texture_setup_t* su, int y, int first, int last,
          walk_t walk) {
    carry_t c;
    span_carry(su, y, first, last, walk != WALK_DIVIDED, &c);
    if (walk == WALK_WRITE) {
        if (su->sampler.texture->format == R3D_TEXTURE_MASK1) {
            write_kept_runs(target, &su->sampler, y, first, last, &c);
        } else {
            r3d_span_write(target, y, first, last, c.z, c.dz);
        }
        return;
    }
    const r3d_span_row_t row = r3d_span_row(target, y);
    if (walk == WALK_CUT) {
        fill_masked(row.depth + first, row.color + first, last - first + 1, &c, &su->sampler, first, y);
    } else if (walk == WALK_DIVIDED) {
        const float fx = (float)(first - su->qx0);
        const float fy = (float)(y - su->qy0);
        fill_divided(su, row.depth + first, row.color + first, last - first + 1, &c, plane_at(&su->s, fx, fy),
                     plane_at(&su->t, fx, fy), plane_at(&su->q, fx, fy), first, y);
    } else {
        fill_colormap(row.depth + first, row.color + first, last - first + 1, &c, &su->sampler, first, y);
    }
}

/* The level a pixel steps about one texel of: floor(log2) of the longest
 * texel step, in level-0 texels, so the first level whose next one would
 * need a step of 2^(level + 1) or more. */
static int
pick_level(const r3d_span_texture_setup_t* su, const r3d_texture_t* texture) {
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

/* Whether mip `level` is a level from setting level `from` on (0: none). */
static inline bool
level_from(int level, uint8_t from) {
    return from != 0 && level >= from;
}

/* Whether mip `level` is an INDEX4 level from target->texture_far_level on,
 * drawn untextured. */
static inline bool
is_far(const r3d_span_target_t* target, const r3d_texture_t* texture, int level) {
    return texture->format == R3D_TEXTURE_INDEX4 && level_from(level, target->texture_far_level);
}

/* What set_up() left: nothing to draw, a far level for r3d_span_triangle_mean(), or
 * `su` set up. */
typedef enum { SET_UP_EMPTY, SET_UP_FAR, SET_UP_DONE } set_up_result_t;

/* A vertex colour's luminance, 0..255, as an 8.8 level, 255 the last. */
static inline float
light_level(const r3d_span_vertex_t* v) {
    const float luma =
        (((float)R3D_TEXTURE_LUMA_R * v->r) + ((float)R3D_TEXTURE_LUMA_G * v->g) + ((float)R3D_TEXTURE_LUMA_B * v->b))
        * (1.0F / (float)(1 << R3D_TEXTURE_LUMA_SHIFT));
    return luma * ((float)LEVEL_MAX / (float)UINT8_MAX);
}

RENDER_TAIL_ATTR int
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

/* Whether an INDEX4 triangle set up affine in `su` divides, and how far
 * apart, judged on `whole`, its unclipped box, so every window that draws a
 * part of it maps u and v alike. When it does, its u and v planes become
 * u q and v q, from the texture's tile holding vertex 0, so a far tile's
 * large u keeps its fraction through the products and divides; the level
 * stays the affine planes' pick. It sits at the flash tail, apart from
 * set_up(). */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
set_up_divide(const r3d_span_target_t* target, const r3d_span_vertex_t* const v[R3D_SPAN_CORNERS],
              const vec2f_t uv[R3D_SPAN_CORNERS], const r3d_texture_t* texture, recti_t whole,
              r3d_span_texture_setup_t* su) {
    const float z_near = mathf_max(v[0]->z, mathf_max(v[1]->z, v[2]->z));
    const float z_far = mathf_min(v[0]->z, mathf_min(v[1]->z, v[2]->z));
    const float side = (float)mathi_max(whole.w, whole.h);
    const float extent =
        mathf_max(mathf_abs(su->s.dx) + mathf_abs(su->t.dx), mathf_abs(su->s.dy) + mathf_abs(su->t.dy)) * side;
    su->divide = r3d_texture_segment_px(target->texture_divide_px,
                                        (float)target->texture_bend_steps * (1.0F / (float)RENDER_TEXTURE_BEND_STEPS),
                                        extent, z_near, z_far, side);
    r3d_span_frame_t f;
    if (su->divide == 0 || !r3d_span_frame_of(v[0], v[1], v[2], whole.x, whole.y, &f)) {
        su->divide = 0;
        return;
    }
    /* The level's side over level 0's: the scale set_up() gave u and v. */
    const float scale = 1.0F / (float)(1U << sampler_level(&su->sampler));
    /* The start of the tile holding vertex 0. */
    const float tile = (float)(1U << texture->size_log2);
    const vec2f_t origin = {uv[0].x - mathf_repeat(uv[0].x, tile), uv[0].y - mathf_repeat(uv[0].y, tile)};
    float s_q[R3D_SPAN_CORNERS];
    float t_q[R3D_SPAN_CORNERS];
    for (int i = 0; i < R3D_SPAN_CORNERS; i++) {
        s_q[i] = (uv[i].x - origin.x) * scale * v[i]->z;
        t_q[i] = (uv[i].y - origin.y) * scale * v[i]->z;
    }
    su->qx0 = whole.x;
    su->qy0 = whole.y;
    su->q = r3d_span_plane_of(&f, v[0]->z, v[1]->z, v[2]->z);
    su->s = r3d_span_plane_of(&f, s_q[0], s_q[1], s_q[2]);
    su->t = r3d_span_plane_of(&f, t_q[0], t_q[1], t_q[2]);
}

/* Sets `su` up to fill the triangle's part in window box `box` affine, off
 * the row walks' stack chain: the frame lives only here. A far INDEX4 level
 * (is_far()) stops at its mip: r3d_span_triangle_mean() draws it. */
static __attribute__((noinline)) set_up_result_t
set_up(const r3d_span_target_t* target, const r3d_span_vertex_t* const v[R3D_SPAN_CORNERS],
       const vec2f_t uv[R3D_SPAN_CORNERS], const r3d_texture_t* texture, recti_t box, r3d_span_texture_setup_t* su) {
    r3d_span_frame_t f;
    if (!r3d_span_frame_of(v[0], v[1], v[2], box.x, box.y, &f)) {
        return SET_UP_EMPTY;
    }
    su->s = r3d_span_plane_of(&f, uv[0].x, uv[1].x, uv[2].x);
    su->t = r3d_span_plane_of(&f, uv[0].y, uv[1].y, uv[2].y);
    const int level = pick_level(su, texture);
    if (is_far(target, texture, level)) {
        R3D_PATH_HIT(far);
        return SET_UP_FAR;
    }
    su->x0 = box.x;
    su->y0 = box.y;
    const float depth_scale = (float)DEPTH_MAX;
    su->z = r3d_span_plane_of(&f, v[0]->z * depth_scale, v[1]->z * depth_scale, v[2]->z * depth_scale);
    const uint16_t cap =
        target->skip_hidden ? r3d_span_depth_limit(mathf_max(v[0]->z, mathf_max(v[1]->z, v[2]->z))) : R3D_DEPTH_NEAREST;
    su->depth_max = cap == R3D_DEPTH_NEAREST ? DEPTH_MAX : (((int32_t)cap << R3D_DEPTH_SHIFT) | 0xFF);
    const float scale = 1.0F / (float)(1U << level);
    su->s = (r3d_span_plane_t){su->s.base * scale, su->s.dx * scale, su->s.dy * scale};
    su->t = (r3d_span_plane_t){su->t.base * scale, su->t.dx * scale, su->t.dy * scale};
    const int side_log2 = texture->size_log2 - level;
    const uint32_t mask = (1U << side_log2) - 1U;
    su->sampler = (r3d_span_texture_sampler_t){texture->texels[level], mask, mask << side_log2,
                                               (uint32_t)(V_INT_SHIFT - side_log2), texture};
    su->level = r3d_span_plane_of(&f, light_level(v[0]), light_level(v[1]), light_level(v[2]));
    return SET_UP_DONE;
}

/* r3d_span's walk: the long edge v0-v2 runs the whole height, and v1 lies
 * right of it when v0-v1-v2 winds positive; the short edge is v0-v1 above
 * v1's row and v1-v2 from it. */
static inline __attribute__((always_inline)) void
walk_rows(const r3d_span_target_t* target, const r3d_span_texture_setup_t* su, recti_t box, const r3d_span_vertex_t* v0,
          const r3d_span_vertex_t* v1, const r3d_span_vertex_t* v2, bool long_on_left, walk_t walk) {
    const int end = recti_bottom(box);
    const int split = mathi_clamp(r3d_span_first_centre(v1->y), box.y, end);
    const int last_column = target->rows.attachment[GFX_ATTACHMENT_COLOR].width - 1;
    r3d_span_walk_edge_t long_edge = r3d_span_walk_edge_at(v0, v2, box.y);
    r3d_span_walk_edge_t short_edge =
        box.y < split ? r3d_span_walk_edge_at(v0, v1, box.y) : r3d_span_walk_edge_at(v1, v2, box.y);
    for (int y = box.y; y < end; y++) {
        if (y == split && box.y < split) {
            short_edge = r3d_span_walk_edge_at(v1, v2, y);
        }
        const r3d_span_walk_edge_t* left = long_on_left ? &long_edge : &short_edge;
        const r3d_span_walk_edge_t* right = long_on_left ? &short_edge : &long_edge;
        const int first = mathi_max(left->q, 0);
        const int last = mathi_min(right->q - 1, last_column);
        if (first <= last) {
            walk_span(target, su, y, first, last, walk);
        }
        r3d_span_walk_edge_step(&long_edge);
        r3d_span_walk_edge_step(&short_edge);
    }
}

static __attribute__((noinline)) void
walk_plain(const r3d_span_target_t* target, const r3d_span_texture_setup_t* su, recti_t box,
           const r3d_span_vertex_t* v0, const r3d_span_vertex_t* v1, const r3d_span_vertex_t* v2, bool long_on_left) {
    walk_rows(target, su, box, v0, v1, v2, long_on_left, WALK_FILL);
}

/* In IRAM (RENDER_DIVIDE_ATTR). */
static RENDER_DIVIDE_ATTR __attribute__((noinline)) void
walk_divided(const r3d_span_target_t* target, const r3d_span_texture_setup_t* su, recti_t box,
             const r3d_span_vertex_t* v0, const r3d_span_vertex_t* v1, const r3d_span_vertex_t* v2, bool long_on_left) {
    walk_rows(target, su, box, v0, v1, v2, long_on_left, WALK_DIVIDED);
}

/* At the flash tail, as fill_masked(). */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
walk_cut(const r3d_span_target_t* target, const r3d_span_texture_setup_t* su, recti_t box, const r3d_span_vertex_t* v0,
         const r3d_span_vertex_t* v1, const r3d_span_vertex_t* v2, bool long_on_left) {
    walk_rows(target, su, box, v0, v1, v2, long_on_left, WALK_CUT);
}

/* After the fill, as r3d_span's writers: each pixel's depth is then the
 * triangle's where it drew. At the flash tail: only debug views write. */
static RENDER_TAIL_ATTR __attribute__((noinline, cold)) void
walk_writers(const r3d_span_target_t* target, const r3d_span_texture_setup_t* su, recti_t box,
             const r3d_span_vertex_t* v0, const r3d_span_vertex_t* v1, const r3d_span_vertex_t* v2, bool long_on_left) {
    walk_rows(target, su, box, v0, v1, v2, long_on_left, WALK_WRITE);
}

/* Whether a triangle with window box `box` and doubled area `area2` draws
 * nothing: off the window, degenerate, or past a debug stop. */
static inline bool
draws_nothing(recti_t box, int32_t area2) {
    return recti_is_empty(box) || area2 == 0 || r3d_span_stop_after != 0;
}

/* Sets up `texture` over the triangle, then walks its rows: the INDEX4
 * fill or MASK1's cut, then the target's writers over what it drew. An
 * INDEX4 level from far_level on returns false, set up no further. */
bool
r3d_span_textured_fill_with(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                            const r3d_span_vertex_t* c, const vec2f_t uv[R3D_SPAN_CORNERS],
                            const r3d_texture_t* texture, r3d_span_texture_setup_t* su) {
    const recti_t whole = r3d_span_extent_limits(a, b, c, 0, 0).centres;
    const recti_t box = r3d_span_window_box(target, whole);
    const int32_t area2 = r3d_span_area2(a, b, c);
    if (draws_nothing(box, area2)) {
        return true;
    }
    const r3d_span_vertex_t* const corners[R3D_SPAN_CORNERS] = {a, b, c};
    const set_up_result_t set = set_up(target, corners, uv, texture, box, su);
    if (set != SET_UP_DONE) {
        return set == SET_UP_EMPTY;
    }
    const r3d_span_vertex_t* v0 = a;
    const r3d_span_vertex_t* v1 = b;
    const r3d_span_vertex_t* v2 = c;
    const bool long_on_left = (area2 > 0) != r3d_span_sort_by_y(&v0, &v1, &v2);
    FRAME_LAP("r3d.draw.fill");
    if (texture->format == R3D_TEXTURE_MASK1) {
        R3D_PATH_HIT(cut);
        walk_cut(target, su, box, v0, v1, v2, long_on_left);
    } else {
        /* A triangle no wider than one segment steps affine: its bend is at
         * most one segment's. So does one reading a level from
         * texture.affine_level on (0: none): its pattern shifts rather than bends. */
        su->divide = 0;
        if (target->texture_divide_px != 0 && !level_from(sampler_level(&su->sampler), target->texture_affine_level)
            && mathi_max(whole.w, whole.h) > target->texture_divide_px) {
            set_up_divide(target, corners, uv, texture, whole, su);
        }
        if (su->divide != 0) {
            R3D_PATH_HIT(divided);
            walk_divided(target, su, box, v0, v1, v2, long_on_left);
        } else {
            R3D_PATH_HIT(affine);
            walk_plain(target, su, box, v0, v1, v2, long_on_left);
        }
    }
    if (target->writer_count != 0) {
        walk_writers(target, su, box, v0, v1, v2, long_on_left);
    }
    return true;
}

bool
r3d_span_textured_fill(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c, const vec2f_t uv[R3D_SPAN_CORNERS], const r3d_texture_t* texture) {
    r3d_span_texture_setup_t setup;
    return r3d_span_textured_fill_with(target, a, b, c, uv, texture, &setup);
}
