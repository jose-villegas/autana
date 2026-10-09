#include "gfx_palette_gen.h"

#include <math.h>
#include <stdbool.h>

#include "gfx/present/gfx_indexed.h"

static double
srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

gfx_lin_t
gfx_rgb_to_lin(uint32_t rgb888) {
    return (gfx_lin_t){
        srgb_to_linear(((rgb888 >> 16) & 0xFFu) / 255.0),
        srgb_to_linear(((rgb888 >> 8) & 0xFFu) / 255.0),
        srgb_to_linear((rgb888 & 0xFFu) / 255.0),
    };
}

gfx_lab_t
gfx_lin_to_lab(gfx_lin_t c) {
    const double l = cbrt(0.4122214708 * c.r + 0.5363325602 * c.g + 0.0514459929 * c.b);
    const double m = cbrt(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    const double s = cbrt(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    return (gfx_lab_t){
        100.0 * (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s),
        100.0 * (1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
        100.0 * (0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s),
    };
}

static gfx_lab_t
rgb565_native_to_lab(uint16_t native) {
    return gfx_lin_to_lab(gfx_rgb_to_lin(gfx_color_rgb888(gfx_color_swap(native))));
}

double
gfx_lab_dist2(gfx_lab_t p, gfx_lab_t q) {
    const double dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return dl * dl + da * da + db * db;
}

void
gfx_palette_gen_build_index_map(const gfx_palette_t* palette, int first_index, uint8_t out_map[65536]) {
    gfx_lab_t entry_lab[GFX_PALETTE_MAX_ENTRIES];
    for (int i = first_index; i < palette->count; i++) {
        entry_lab[i] = rgb565_native_to_lab(gfx_color_swap(palette->entries[i]));
    }

    for (int k = 0; k < 65536; k++) {
        const gfx_lab_t target = rgb565_native_to_lab((uint16_t)k);
        int best = first_index;
        double best_d = 1e30;
        for (int i = first_index; i < palette->count; i++) {
            const double d = gfx_lab_dist2(target, entry_lab[i]);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        out_map[k] = (uint8_t)best;
    }
}

/* One 16-colour palette entry's dither choice for a target colour: the
 * single nearest entry, or the finest-matching point along the segment
 * between the two entries whose linear-light blend comes closest, the
 * same two-stage search main/apps/sand/tools/shading_palette.c's own
 * ega_choose() used before this was generalised out of it. */
typedef struct {
    uint8_t lo, hi, level; /* level: 0-16, sixteenths of the way to `hi` */
} dither_choice_t;

/* Stage one of choose_dither(), and GFX_DITHER_NONE's own whole answer:
 * the single nearest entry, never a blend. */
static dither_choice_t
nearest_choice(const gfx_lab_t* lab, int count16, gfx_lab_t target, double* out_cost) {
    dither_choice_t best = {0, 0, 0};
    double best_cost = 1e30;
    for (int i = 0; i < count16; i++) {
        const double e = sqrt(gfx_lab_dist2(target, lab[i]));
        if (e < best_cost) {
            best_cost = e;
            best = (dither_choice_t){(uint8_t)i, (uint8_t)i, 0};
        }
    }
    *out_cost = best_cost;
    return best;
}

/* Every achievable dither level between entries i and j (the level nearest
 * the target's least-squares projection onto that segment, plus one on
 * each side), keeping best/best_cost if one of them wins. */
static void
try_dither_pair(const gfx_lin_t* lin, int i, int j, gfx_lab_t target, gfx_lin_t target_lin, double* best_cost,
                dither_choice_t* best) {
    const gfx_lin_t d = {lin[j].r - lin[i].r, lin[j].g - lin[i].g, lin[j].b - lin[i].b};
    const double den = d.r * d.r + d.g * d.g + d.b * d.b;
    if (den <= 0.0) {
        return;
    }
    const double t =
        ((target_lin.r - lin[i].r) * d.r + (target_lin.g - lin[i].g) * d.g + (target_lin.b - lin[i].b) * d.b) / den;
    const int centre = (int)lround(t * 16.0);
    for (int level = centre - 1; level <= centre + 1; level++) {
        if (level < 1 || level >= 16) {
            continue;
        }
        const double f = (double)level / 16.0;
        const gfx_lin_t mix = {lin[i].r + d.r * f, lin[i].g + d.g * f, lin[i].b + d.b * f};
        const double e = sqrt(gfx_lab_dist2(target, gfx_lin_to_lab(mix)));
        if (e < *best_cost) {
            *best_cost = e;
            *best = (dither_choice_t){(uint8_t)i, (uint8_t)j, (uint8_t)level};
        }
    }
}

static dither_choice_t
choose_dither(const gfx_lin_t* lin, const gfx_lab_t* lab, int count16, gfx_lab_t target, gfx_lin_t target_lin) {
    double best_cost;
    dither_choice_t best = nearest_choice(lab, count16, target, &best_cost);

    for (int i = 0; i < count16; i++) {
        for (int j = i + 1; j < count16; j++) {
            try_dither_pair(lin, i, j, target, target_lin, &best_cost, &best);
        }
    }
    return best;
}

static void
lin_lab_of_palette16(const gfx_palette_t* palette16, gfx_lin_t out_lin[16], gfx_lab_t out_lab[16]) {
    for (int i = 0; i < palette16->count; i++) {
        const uint32_t rgb888 = gfx_color_rgb888(palette16->entries[i]);
        out_lin[i] = gfx_rgb_to_lin(rgb888);
        out_lab[i] = gfx_lin_to_lab(out_lin[i]);
    }
}

/* palette256 entry `i`'s own dither choice against palette16, the one
 * step every build function below shares; only how each lays lo/hi/level
 * onto pixels or cells differs. */
static dither_choice_t
dither_choice_of_entry(const gfx_palette_t* palette256, int i, const gfx_lin_t* lin16, const gfx_lab_t* lab16,
                       int count16) {
    const uint32_t rgb888 = gfx_color_rgb888(palette256->entries[i]);
    const gfx_lin_t target_lin = gfx_rgb_to_lin(rgb888);
    const gfx_lab_t target_lab = gfx_lin_to_lab(target_lin);
    return choose_dither(lin16, lab16, count16, target_lab, target_lin);
}

/* palette256 entry `i`'s own NEAREST palette16 entry, never a blend:
 * GFX_DITHER_NONE's own choice. */
static uint8_t
nearest_index_of_entry(const gfx_palette_t* palette256, int i, const gfx_lab_t* lab16, int count16) {
    const uint32_t rgb888 = gfx_color_rgb888(palette256->entries[i]);
    const gfx_lab_t target_lab = gfx_lin_to_lab(gfx_rgb_to_lin(rgb888));
    double cost;
    return nearest_choice(lab16, count16, target_lab, &cost).lo;
}

void
gfx_palette_gen_build_dither16(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                               gfx_color_t out_table[GFX_PALETTE_MAX_ENTRIES * 16]) {
    gfx_lin_t lin16[16];
    gfx_lab_t lab16[16];
    lin_lab_of_palette16(palette16, lin16, lab16);

    for (int i = 0; i < palette256->count; i++) {
        const dither_choice_t ch = dither_choice_of_entry(palette256, i, lin16, lab16, palette16->count);
        const uint8_t alpha = ch.level == 0 ? 0u : (uint8_t)(ch.level * 16u);

        for (int py = 0; py < 4; py++) {
            for (int px = 0; px < 4; px++) {
                const bool hi = gfx_dither_covers(px, py, alpha);
                out_table[i * GFX_INDEXED_DITHER16_PHASES + py * 4 + px] =
                    hi ? palette16->entries[ch.hi] : palette16->entries[ch.lo];
            }
        }
    }
}

void
gfx_palette_gen_build_lut_nearest(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                  gfx_color_t out_lut[GFX_PALETTE_MAX_ENTRIES]) {
    gfx_lab_t lab16[16];
    for (int i = 0; i < palette16->count; i++) {
        lab16[i] = gfx_lin_to_lab(gfx_rgb_to_lin(gfx_color_rgb888(palette16->entries[i])));
    }

    for (int i = 0; i < palette256->count; i++) {
        out_lut[i] = palette16->entries[nearest_index_of_entry(palette256, i, lab16, palette16->count)];
    }
}

void
gfx_palette_gen_build_dither_cell(const gfx_palette_t* palette256, const gfx_palette_t* palette16, bool bayer2,
                                  gfx_color_t* out_table) {
    gfx_lin_t lin16[16];
    gfx_lab_t lab16[16];
    lin_lab_of_palette16(palette16, lin16, lab16);
    /* gfx_dither4x4 (gfx_color.h) at order 2: the same recursive Bayer
     * construction, its four taps spread over `level`'s own 0-16 domain. */
    static const uint8_t bayer2x2[4] = {0, 8, 12, 4};

    for (int i = 0; i < palette256->count; i++) {
        const dither_choice_t ch = dither_choice_of_entry(palette256, i, lin16, lab16, palette16->count);
        if (!bayer2) {
            /* checker: solid when the search found no worthwhile blend,
             * else alternating; matches
             * gfx_indexed_expand_row_dither_cell()'s own (gx + cy) & 1 phase
             * order. */
            out_table[i * 2 + 0] = palette16->entries[ch.lo];
            out_table[i * 2 + 1] = palette16->entries[ch.level == 0 ? ch.lo : ch.hi];
            continue;
        }
        for (int p = 0; p < 4; p++) {
            const bool hi = ch.level > bayer2x2[p];
            out_table[i * 4 + p] = palette16->entries[hi ? ch.hi : ch.lo];
        }
    }
}

void
gfx_palette_gen_build_dither_checker2(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                      gfx_color_t out_table[GFX_PALETTE_MAX_ENTRIES * 2 * 2]) {
    gfx_lin_t lin16[16];
    gfx_lab_t lab16[16];
    lin_lab_of_palette16(palette16, lin16, lab16);

    for (int i = 0; i < palette256->count; i++) {
        const dither_choice_t ch = dither_choice_of_entry(palette256, i, lin16, lab16, palette16->count);
        for (int py = 0; py < 2; py++) {
            for (int px = 0; px < 2; px++) {
                const bool hi = ch.level != 0 && ((px + py) & 1) != 0;
                out_table[(i * 2 + py) * 2 + px] = palette16->entries[hi ? ch.hi : ch.lo];
            }
        }
    }
}
