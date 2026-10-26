#include "draw/draw_palette_standard.h"

#include <string.h>

#include "draw/draw_palette_standard_generated.h"
#include "image/palette/palette.h"
#include "image/pixel/rgb565.h"

/* IBM CGA/EGA 16-colour palette. Index 6 (brown, not the sequential dark
 * yellow) is deliberate CGA hardware behaviour, not a typo; see the
 * sources cited in draw_palette_standard.h. */
static const gfx_color_t cga_ega_16[16] = {
    RGB565BE(0x000000), RGB565BE(0x0000AA), RGB565BE(0x00AA00), RGB565BE(0x00AAAA),
    RGB565BE(0xAA0000), RGB565BE(0xAA00AA), RGB565BE(0xAA5500), RGB565BE(0xAAAAAA),
    RGB565BE(0x555555), RGB565BE(0x5555FF), RGB565BE(0x55FF55), RGB565BE(0x55FFFF),
    RGB565BE(0xFF5555), RGB565BE(0xFF55FF), RGB565BE(0xFFFF55), RGB565BE(0xFFFFFF),
};

static const gfx_color_t pico8_16[16] = {
    RGB565BE(0x000000), RGB565BE(0x1D2B53), RGB565BE(0x7E2553), RGB565BE(0x008751),
    RGB565BE(0xAB5236), RGB565BE(0x5F574F), RGB565BE(0xC2C3C7), RGB565BE(0xFFF1E8),
    RGB565BE(0xFF004D), RGB565BE(0xFFA300), RGB565BE(0xFFEC27), RGB565BE(0x00E436),
    RGB565BE(0x29ADFF), RGB565BE(0x83769C), RGB565BE(0xFF77A8), RGB565BE(0xFFCCAA),
};

static const gfx_color_t db16[16] = {
    RGB565BE(0x140C1C), RGB565BE(0x442434), RGB565BE(0x30346D), RGB565BE(0x4E4A4E),
    RGB565BE(0x854C30), RGB565BE(0x346524), RGB565BE(0xD04648), RGB565BE(0x757161),
    RGB565BE(0x597DCE), RGB565BE(0xD27D2C), RGB565BE(0x8595A1), RGB565BE(0x6DAA2C),
    RGB565BE(0xD2AA99), RGB565BE(0x6DC2CA), RGB565BE(0xDAD45E), RGB565BE(0xDEEED6),
};

static const gfx_color_t db32[32] = {
    RGB565BE(0x000000), RGB565BE(0x222034), RGB565BE(0x45283C), RGB565BE(0x663931), RGB565BE(0x8F563B),
    RGB565BE(0xDF7126), RGB565BE(0xD9A066), RGB565BE(0xEEC39A), RGB565BE(0xFBF236), RGB565BE(0x99E550),
    RGB565BE(0x6ABE30), RGB565BE(0x37946E), RGB565BE(0x4B692F), RGB565BE(0x524B24), RGB565BE(0x323C39),
    RGB565BE(0x3F3F74), RGB565BE(0x306082), RGB565BE(0x5B6EE1), RGB565BE(0x639BFF), RGB565BE(0x5FCDE4),
    RGB565BE(0xCBDBFC), RGB565BE(0xFFFFFF), RGB565BE(0x9BADB7), RGB565BE(0x847E87), RGB565BE(0x696A6A),
    RGB565BE(0x595652), RGB565BE(0x76428A), RGB565BE(0xAC3232), RGB565BE(0xD95763), RGB565BE(0xD77BBA),
    RGB565BE(0x8F974A), RGB565BE(0x8A6F30),
};

/* i*17 for i in 0..15: exact, since 15*17 == 255. */
static const gfx_color_t grayscale_16[16] = {
    RGB565BE(0x000000), RGB565BE(0x111111), RGB565BE(0x222222), RGB565BE(0x333333),
    RGB565BE(0x444444), RGB565BE(0x555555), RGB565BE(0x666666), RGB565BE(0x777777),
    RGB565BE(0x888888), RGB565BE(0x999999), RGB565BE(0xAAAAAA), RGB565BE(0xBBBBBB),
    RGB565BE(0xCCCCCC), RGB565BE(0xDDDDDD), RGB565BE(0xEEEEEE), RGB565BE(0xFFFFFF),
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
