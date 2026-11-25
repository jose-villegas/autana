/* ridge_theme: a seed colour expanded into the launcher's fixed lightness roles. */
#pragma once

#include <math.h>
#include <stdint.h>

#include "gfx/target/gfx_color.h"
#include "image/colour/oklab.h"
#include "image/colour/srgb.h"
#include "image/pixel/rgb565.h"

typedef struct {
    float l;
    float c;
    float h;
} ridge_oklch_t;

typedef struct {
    uint32_t seed_rgb;
    uint32_t background_rgb;
    uint32_t sky_top_rgb;
    uint32_t sky_bottom_rgb;
    uint32_t back0_rgb;
    uint32_t back1_rgb;
    gfx_color_t background_565;
    gfx_color_t sky_top_565;
    gfx_color_t sky_bottom_565;
    gfx_color_t back0_565;
    gfx_color_t back1_565;
} ridge_theme_t;

static inline ridge_oklch_t
ridge_theme_to_oklch(uint32_t rgb) {
    const oklabf_t lab =
        oklabf_from_linear((vec3f_t){srgbf_decode_byte((uint8_t)(rgb >> 16)), srgbf_decode_byte((uint8_t)(rgb >> 8)),
                                     srgbf_decode_byte((uint8_t)rgb)});
    return (ridge_oklch_t){lab.l, sqrtf((lab.a * lab.a) + (lab.b * lab.b)), atan2f(lab.b, lab.a)};
}

static inline vec3f_t
ridge_oklch_to_linear(float l, float c, float h) {
    return oklabf_to_linear((oklabf_t){l, c * cosf(h), c * sinf(h)});
}

static inline uint32_t
ridge_theme_from_oklch(float l, float c, float h) {
    const vec3f_t rgb = ridge_oklch_to_linear(l, c, h);
    return ((uint32_t)srgbf_encode_byte(rgb.x) << 16) | ((uint32_t)srgbf_encode_byte(rgb.y) << 8)
           | (uint32_t)srgbf_encode_byte(rgb.z);
}

static inline bool
ridge_theme_in_gamut(float l, float c, float h) {
    const vec3f_t rgb = ridge_oklch_to_linear(l, c, h);
    return rgb.x >= 0.0F && rgb.x <= 1.0F && rgb.y >= 0.0F && rgb.y <= 1.0F && rgb.z >= 0.0F && rgb.z <= 1.0F;
}

static inline uint32_t
ridge_theme_role(ridge_oklch_t seed, float l, float chroma_scale) {
    float c = seed.c * chroma_scale;
    for (int attempt = 0; attempt < 12; attempt++) {
        if (ridge_theme_in_gamut(l, c, seed.h)) {
            return ridge_theme_from_oklch(l, c, seed.h);
        }
        c *= 0.8F;
    }
    return ridge_theme_from_oklch(l, 0.0F, seed.h);
}

static inline ridge_theme_t
ridge_theme_from_rgb(uint32_t rgb) {
    const ridge_oklch_t seed = ridge_theme_to_oklch(rgb);
    const uint32_t background = ridge_theme_role(seed, 0.55F, 0.70F);
    const uint32_t sky_top = ridge_theme_role(seed, 0.55F, 1.0F);
    const uint32_t sky_bottom = ridge_theme_role(seed, 0.64F, 0.55F);
    const uint32_t back0 = ridge_theme_role(seed, 0.49F, 0.95F);
    const uint32_t back1 = ridge_theme_role(seed, 0.42F, 0.85F);
    return (ridge_theme_t){rgb,
                           background,
                           sky_top,
                           sky_bottom,
                           back0,
                           back1,
                           RGB565BE(background),
                           RGB565BE(sky_top),
                           RGB565BE(sky_bottom),
                           RGB565BE(back0),
                           RGB565BE(back1)};
}
