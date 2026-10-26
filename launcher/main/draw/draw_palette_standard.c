#include "draw/draw_palette_standard.h"

#include <string.h>

#include "draw/draw_palette_standard_generated.h"
#include "image/palette/palette.h"
#include "image/pixel/rgb565.h"

#define PALETTE_ROW(a, b, c, d) RGB565BE(a), RGB565BE(b), RGB565BE(c), RGB565BE(d)

/* IBM CGA/EGA 16-colour palette. Index 6 (brown, not the sequential dark
 * yellow) is deliberate CGA hardware behaviour, not a typo; see the
 * sources cited in draw_palette_standard.h. */
static const gfx_color_t cga_ega_16[16] = {
    PALETTE_ROW(0x000000, 0x0000AA, 0x00AA00, 0x00AAAA),
    PALETTE_ROW(0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA),
    PALETTE_ROW(0x555555, 0x5555FF, 0x55FF55, 0x55FFFF),
    PALETTE_ROW(0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF),
};

static const gfx_color_t pico8_16[16] = {
    PALETTE_ROW(0x000000, 0x1D2B53, 0x7E2553, 0x008751),
    PALETTE_ROW(0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8),
    PALETTE_ROW(0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436),
    PALETTE_ROW(0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA),
};

static const gfx_color_t db16[16] = {
    PALETTE_ROW(0x140C1C, 0x442434, 0x30346D, 0x4E4A4E),
    PALETTE_ROW(0x854C30, 0x346524, 0xD04648, 0x757161),
    PALETTE_ROW(0x597DCE, 0xD27D2C, 0x8595A1, 0x6DAA2C),
    PALETTE_ROW(0xD2AA99, 0x6DC2CA, 0xDAD45E, 0xDEEED6),
};

static const gfx_color_t db32[32] = {
    PALETTE_ROW(0x000000, 0x222034, 0x45283C, 0x663931), PALETTE_ROW(0x8F563B, 0xDF7126, 0xD9A066, 0xEEC39A),
    PALETTE_ROW(0xFBF236, 0x99E550, 0x6ABE30, 0x37946E), PALETTE_ROW(0x4B692F, 0x524B24, 0x323C39, 0x3F3F74),
    PALETTE_ROW(0x306082, 0x5B6EE1, 0x639BFF, 0x5FCDE4), PALETTE_ROW(0xCBDBFC, 0xFFFFFF, 0x9BADB7, 0x847E87),
    PALETTE_ROW(0x696A6A, 0x595652, 0x76428A, 0xAC3232), PALETTE_ROW(0xD95763, 0xD77BBA, 0x8F974A, 0x8A6F30),
};

/* i*17 for i in 0..15: exact, since 15*17 == 255. */
static const gfx_color_t grayscale_16[16] = {
    PALETTE_ROW(0x000000, 0x111111, 0x222222, 0x333333),
    PALETTE_ROW(0x444444, 0x555555, 0x666666, 0x777777),
    PALETTE_ROW(0x888888, 0x999999, 0xAAAAAA, 0xBBBBBB),
    PALETTE_ROW(0xCCCCCC, 0xDDDDDD, 0xEEEEEE, 0xFFFFFF),
};

const palette_t draw_palette_cga16 = {"cga16", cga_ega_16, 16};
const palette_t draw_palette_ega16 = {"ega16", cga_ega_16, 16};
const palette_t draw_palette_pico8_16 = {"pico8_16", pico8_16, 16};
const palette_t draw_palette_db16 = {"db16", db16, 16};
const palette_t draw_palette_db32 = {"db32", db32, 32};
const palette_t draw_palette_vga256 = {"vga256", draw_palette_vga256_entries, 256};
const palette_t draw_palette_grayscale16 = {"grayscale16", grayscale_16, 16};
const palette_t draw_palette_grayscale256 = {"grayscale256", draw_palette_grayscale256_entries, 256};

static const palette_t* const registry[] = {
    &draw_palette_cga16, &draw_palette_ega16,  &draw_palette_pico8_16,    &draw_palette_db16,
    &draw_palette_db32,  &draw_palette_vga256, &draw_palette_grayscale16, &draw_palette_grayscale256,
};

#define REGISTRY_COUNT ((int)(sizeof registry / sizeof registry[0]))

const palette_t*
draw_palette_standard_find(const char* name) {
    for (int i = 0; i < REGISTRY_COUNT; i++) {
        if (strcmp(registry[i]->name, name) == 0) {
            return registry[i];
        }
    }
    return NULL;
}

int
draw_palette_standard_count(void) {
    return REGISTRY_COUNT;
}

const palette_t*
draw_palette_standard_at(int index) {
    return registry[index];
}
