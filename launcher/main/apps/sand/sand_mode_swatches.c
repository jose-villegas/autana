#include "sand_mode_swatches.h"

#include <stdbool.h>

static int
luma(uint32_t rgb) {
    return (int)(((rgb >> 16) & 0xFF) * 299 + ((rgb >> 8) & 0xFF) * 587 + (rgb & 0xFF) * 114);
}

static bool
contains(const uint32_t* colors, int n, uint32_t rgb) {
    for (int i = 0; i < n; i++) {
        if (colors[i] == rgb) {
            return true;
        }
    }
    return false;
}

/* Dark to light, so the grid reads as the mode's own ramp. */
static void
sort_by_luma(uint32_t* colors, int n) {
    for (int i = 1; i < n; i++) {
        const uint32_t c = colors[i];
        int j = i - 1;
        while (j >= 0 && luma(colors[j]) > luma(c)) {
            colors[j + 1] = colors[j];
            j--;
        }
        colors[j + 1] = c;
    }
}

static void
build_sixteen(const gfx_color_t* none_lut, int lut_size, sand_mode_swatch_t* out) {
    const int want = SAND_SWATCH_16_COLS * SAND_SWATCH_ROWS;
    int n = 0;
    for (int i = 0; i < lut_size && n < want; i++) {
        const uint32_t rgb = gfx_color_rgb888(none_lut[i]);
        if (!contains(out->rgb, n, rgb)) {
            out->rgb[n++] = rgb;
        }
    }
    sort_by_luma(out->rgb, n);
    for (; n < want; n++) {
        out->rgb[n] = out->rgb[n - 1];
    }
    out->cols = SAND_SWATCH_16_COLS;
    out->rows = SAND_SWATCH_ROWS;
}

static void
build_256(const gfx_color_t* lut256, int lut_size, int ui_entries, sand_mode_swatch_t* out) {
    const int want = SAND_SWATCH_256_COLS * SAND_SWATCH_ROWS;
    const int span = lut_size - ui_entries;
    for (int i = 0; i < want; i++) {
        out->rgb[i] = gfx_color_rgb888(lut256[ui_entries + i * span / want]);
    }
    out->cols = SAND_SWATCH_256_COLS;
    out->rows = SAND_SWATCH_ROWS;
}

/* Fully saturated, full value: hue 0-1535 in six 256-step sextants. */
static uint32_t
hue_rgb(int hue) {
    const int sextant = hue / 256;
    const uint32_t up = (uint32_t)(hue % 256);
    const uint32_t down = 255 - up;
    switch (sextant) {
        case 0: return 0xFF0000u | (up << 8);
        case 1: return (down << 16) | 0x00FF00u;
        case 2: return 0x00FF00u | up;
        case 3: return (down << 8) | 0x0000FFu;
        case 4: return (up << 16) | 0x0000FFu;
        default: return 0xFF0000u | down;
    }
}

static void
build_full(sand_mode_swatch_t* out) {
    for (int i = 0; i < SAND_SWATCH_FULL_BANDS; i++) {
        out->rgb[i] = hue_rgb(i * 1536 / SAND_SWATCH_FULL_BANDS);
    }
    out->cols = SAND_SWATCH_FULL_BANDS;
    out->rows = 1;
}

void
sand_mode_swatches(const gfx_color_t* none_lut, const gfx_color_t* lut256, int lut_size, int ui_entries,
                   sand_mode_swatch_t out[SAND_COLOUR_MODE_COUNT]) {
    build_full(&out[SAND_COLOUR_FULL]);
    build_256(lut256, lut_size, ui_entries, &out[SAND_COLOUR_256]);
    build_sixteen(none_lut, lut_size, &out[SAND_COLOUR_16]);
}
