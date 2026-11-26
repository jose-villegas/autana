/* r3d_span_textured_internal: scratch shared between triangle setup and its span walk. */
#pragma once

#include "render/r3d_span_internal.h"
#include "render/texture/r3d_texture.h"

/* What every span of one triangle reads. */
typedef struct {
    const uint8_t* texels;
    uint32_t mask;     /* the level's side less one */
    uint32_t row_mask; /* mask, moved up to v's place in a texel offset */
    uint32_t v_shift;  /* what lands v's integer bits at that place */
    const r3d_texture_t* texture;
} r3d_span_texture_sampler_t;

/* What the set-up hands the row walk. */
typedef struct {
    r3d_span_plane_t z;     /* 16.8 depth */
    r3d_span_plane_t s, t;  /* u, v in the level's texels */
    r3d_span_plane_t level; /* 8.8 light level */
    r3d_span_plane_t q;     /* inverse depth, when the triangle divides */
    int32_t depth_max;
    int x0, y0;   /* the origin pixel */
    int qx0, qy0; /* the dividing planes' origin pixel: the unclipped box's, alike in every window */
    int divide;   /* pixels between divides, 0 when the triangle steps affine */
    r3d_span_texture_sampler_t sampler;
} r3d_span_texture_setup_t;

bool r3d_span_textured_fill_with(const r3d_span_target_t* target, const r3d_span_vertex_t* a,
                                 const r3d_span_vertex_t* b, const r3d_span_vertex_t* c,
                                 const vec2f_t uv[R3D_SPAN_CORNERS], const r3d_texture_t* texture,
                                 r3d_span_texture_setup_t* su);

static inline __attribute__((always_inline)) void
r3d_span_textured_triangle_with(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                                const r3d_span_vertex_t* c, const vec2f_t uv[R3D_SPAN_CORNERS],
                                const r3d_texture_t* texture, r3d_span_texture_setup_t* setup) {
    if (!r3d_span_textured_fill_with(target, a, b, c, uv, texture, setup)) {
        r3d_span_triangle_mean(target, a, b, c);
    }
}
