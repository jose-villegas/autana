/* ridge_theme - a seed colour expanded into the launcher's fixed lightness roles. */
#pragma once

#include <math.h>
#include <stdint.h>

#include "gfx/gfx_color.h"

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

static inline float
ridge_theme_linear(float v) {
    return v <= 0.04045F ? v / 12.92F : powf((v + 0.055F) / 1.055F, 2.4F);
}

static inline float
ridge_theme_srgb(float v) {
    return v <= 0.0031308F ? v * 12.92F : (1.055F * powf(v, 1.0F / 2.4F)) - 0.055F;
}

static inline ridge_oklch_t
ridge_theme_to_oklch(uint32_t rgb) {
    const float r = ridge_theme_linear((float)((rgb >> 16) & 0xff) / 255.0F);
    const float g = ridge_theme_linear((float)((rgb >> 8) & 0xff) / 255.0F);
    const float b = ridge_theme_linear((float)(rgb & 0xff) / 255.0F);
    const float l = cbrtf((0.4122214708F * r) + (0.5363325363F * g) + (0.0514459929F * b));
    const float m = cbrtf((0.2119034982F * r) + (0.6806995451F * g) + (0.1073969566F * b));
    const float s = cbrtf((0.0883024619F * r) + (0.2817188376F * g) + (0.6299787005F * b));
    const float a = (1.9779984951F * l) - (2.4285922050F * m) + (0.4505937099F * s);
    const float bb = (0.0259040371F * l) + (0.7827717662F * m) - (0.8086757660F * s);
    return (ridge_oklch_t){(0.2104542553F * l) + (0.7936177850F * m) - (0.0040720468F * s), sqrtf((a * a) + (bb * bb)),
                           atan2f(bb, a)};
}

static inline uint32_t
ridge_theme_from_oklch(float l, float c, float h) {
    const float a = c * cosf(h);
    const float b = c * sinf(h);
    const float ll = l + (0.3963377774F * a) + (0.2158037573F * b);
    const float mm = l - (0.1055613458F * a) - (0.0638541728F * b);
    const float ss = l - (0.0894841775F * a) - (1.2914855480F * b);
    const float ll3 = ll * ll * ll;
    const float mm3 = mm * mm * mm;
    const float ss3 = ss * ss * ss;
    const float r = ridge_theme_srgb((4.0767416621F * ll3) - (3.3077115913F * mm3) + (0.2309699292F * ss3));
    const float g = ridge_theme_srgb((-1.2684380046F * ll3) + (2.6097574011F * mm3) - (0.3413193965F * ss3));
    const float bb = ridge_theme_srgb((-0.0041960863F * ll3) - (0.7034186147F * mm3) + (1.7076147010F * ss3));
    const int ri = r <= 0.0F ? 0 : (r >= 1.0F ? 255 : (int)((r * 255.0F) + 0.5F));
    const int gi = g <= 0.0F ? 0 : (g >= 1.0F ? 255 : (int)((g * 255.0F) + 0.5F));
    const int bi = bb <= 0.0F ? 0 : (bb >= 1.0F ? 255 : (int)((bb * 255.0F) + 0.5F));
    return ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

static inline bool
ridge_theme_in_gamut(float l, float c, float h) {
    const float a = c * cosf(h);
    const float b = c * sinf(h);
    const float ll = l + (0.3963377774F * a) + (0.2158037573F * b);
    const float mm = l - (0.1055613458F * a) - (0.0638541728F * b);
    const float ss = l - (0.0894841775F * a) - (1.2914855480F * b);
    const float ll3 = ll * ll * ll;
    const float mm3 = mm * mm * mm;
    const float ss3 = ss * ss * ss;
    const float r = (4.0767416621F * ll3) - (3.3077115913F * mm3) + (0.2309699292F * ss3);
    const float g = (-1.2684380046F * ll3) + (2.6097574011F * mm3) - (0.3413193965F * ss3);
    const float bb = (-0.0041960863F * ll3) - (0.7034186147F * mm3) + (1.7076147010F * ss3);
    return r >= 0.0F && r <= 1.0F && g >= 0.0F && g <= 1.0F && bb >= 0.0F && bb <= 1.0F;
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
    return (ridge_theme_t){rgb,           background,          sky_top,          sky_bottom,          back0,
                           back1,         GFX_RGB(background), GFX_RGB(sky_top), GFX_RGB(sky_bottom), GFX_RGB(back0),
                           GFX_RGB(back1)};
}
