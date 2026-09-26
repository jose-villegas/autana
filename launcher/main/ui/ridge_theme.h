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
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static inline float
ridge_theme_srgb(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static inline ridge_oklch_t
ridge_theme_to_oklch(uint32_t rgb) {
    const float r = ridge_theme_linear((float)((rgb >> 16) & 0xff) / 255.0f);
    const float g = ridge_theme_linear((float)((rgb >> 8) & 0xff) / 255.0f);
    const float b = ridge_theme_linear((float)(rgb & 0xff) / 255.0f);
    const float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    const float a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    const float bb = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    return (ridge_oklch_t){0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, sqrtf(a * a + bb * bb),
                           atan2f(bb, a)};
}

static inline uint32_t
ridge_theme_from_oklch(float l, float c, float h) {
    const float a = c * cosf(h);
    const float b = c * sinf(h);
    const float ll = l + 0.3963377774f * a + 0.2158037573f * b;
    const float mm = l - 0.1055613458f * a - 0.0638541728f * b;
    const float ss = l - 0.0894841775f * a - 1.2914855480f * b;
    const float ll3 = ll * ll * ll;
    const float mm3 = mm * mm * mm;
    const float ss3 = ss * ss * ss;
    const float r = ridge_theme_srgb(4.0767416621f * ll3 - 3.3077115913f * mm3 + 0.2309699292f * ss3);
    const float g = ridge_theme_srgb(-1.2684380046f * ll3 + 2.6097574011f * mm3 - 0.3413193965f * ss3);
    const float bb = ridge_theme_srgb(-0.0041960863f * ll3 - 0.7034186147f * mm3 + 1.7076147010f * ss3);
    const int ri = r <= 0.0f ? 0 : (r >= 1.0f ? 255 : (int)(r * 255.0f + 0.5f));
    const int gi = g <= 0.0f ? 0 : (g >= 1.0f ? 255 : (int)(g * 255.0f + 0.5f));
    const int bi = bb <= 0.0f ? 0 : (bb >= 1.0f ? 255 : (int)(bb * 255.0f + 0.5f));
    return ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

static inline bool
ridge_theme_in_gamut(float l, float c, float h) {
    const float a = c * cosf(h);
    const float b = c * sinf(h);
    const float ll = l + 0.3963377774f * a + 0.2158037573f * b;
    const float mm = l - 0.1055613458f * a - 0.0638541728f * b;
    const float ss = l - 0.0894841775f * a - 1.2914855480f * b;
    const float ll3 = ll * ll * ll;
    const float mm3 = mm * mm * mm;
    const float ss3 = ss * ss * ss;
    const float r = 4.0767416621f * ll3 - 3.3077115913f * mm3 + 0.2309699292f * ss3;
    const float g = -1.2684380046f * ll3 + 2.6097574011f * mm3 - 0.3413193965f * ss3;
    const float bb = -0.0041960863f * ll3 - 0.7034186147f * mm3 + 1.7076147010f * ss3;
    return r >= 0.0f && r <= 1.0f && g >= 0.0f && g <= 1.0f && bb >= 0.0f && bb <= 1.0f;
}

static inline uint32_t
ridge_theme_role(ridge_oklch_t seed, float l, float chroma_scale) {
    float c = seed.c * chroma_scale;
    for (int attempt = 0; attempt < 12; attempt++) {
        if (ridge_theme_in_gamut(l, c, seed.h)) {
            return ridge_theme_from_oklch(l, c, seed.h);
        }
        c *= 0.8f;
    }
    return ridge_theme_from_oklch(l, 0.0f, seed.h);
}

static inline ridge_theme_t
ridge_theme_from_rgb(uint32_t rgb) {
    const ridge_oklch_t seed = ridge_theme_to_oklch(rgb);
    const uint32_t background = ridge_theme_role(seed, 0.55f, 0.70f);
    const uint32_t sky_top = ridge_theme_role(seed, 0.55f, 1.0f);
    const uint32_t sky_bottom = ridge_theme_role(seed, 0.64f, 0.55f);
    const uint32_t back0 = ridge_theme_role(seed, 0.49f, 0.95f);
    const uint32_t back1 = ridge_theme_role(seed, 0.42f, 0.85f);
    return (ridge_theme_t){rgb,           background,          sky_top,          sky_bottom,          back0,
                           back1,         GFX_RGB(background), GFX_RGB(sky_top), GFX_RGB(sky_bottom), GFX_RGB(back0),
                           GFX_RGB(back1)};
}
