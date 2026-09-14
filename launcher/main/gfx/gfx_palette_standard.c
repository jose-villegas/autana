#include "gfx/gfx_palette_standard.h"

#include <string.h>

#include "gfx/gfx_palette_standard_generated.h"

/* IBM CGA/EGA 16-colour palette. Index 6 (brown, not the sequential dark
 * yellow) is deliberate CGA hardware behaviour, not a typo - see the
 * sources cited in gfx_palette_standard.h. */
static const gfx_color_t cga_ega_16[16] = {
    GFX_RGB(0x000000), GFX_RGB(0x0000AA), GFX_RGB(0x00AA00), GFX_RGB(0x00AAAA), GFX_RGB(0xAA0000), GFX_RGB(0xAA00AA),
    GFX_RGB(0xAA5500), GFX_RGB(0xAAAAAA), GFX_RGB(0x555555), GFX_RGB(0x5555FF), GFX_RGB(0x55FF55), GFX_RGB(0x55FFFF),
    GFX_RGB(0xFF5555), GFX_RGB(0xFF55FF), GFX_RGB(0xFFFF55), GFX_RGB(0xFFFFFF),
};

static const gfx_color_t pico8_16[16] = {
    GFX_RGB(0x000000), GFX_RGB(0x1D2B53), GFX_RGB(0x7E2553), GFX_RGB(0x008751), GFX_RGB(0xAB5236), GFX_RGB(0x5F574F),
    GFX_RGB(0xC2C3C7), GFX_RGB(0xFFF1E8), GFX_RGB(0xFF004D), GFX_RGB(0xFFA300), GFX_RGB(0xFFEC27), GFX_RGB(0x00E436),
    GFX_RGB(0x29ADFF), GFX_RGB(0x83769C), GFX_RGB(0xFF77A8), GFX_RGB(0xFFCCAA),
};

static const gfx_color_t db16[16] = {
    GFX_RGB(0x140C1C), GFX_RGB(0x442434), GFX_RGB(0x30346D), GFX_RGB(0x4E4A4E), GFX_RGB(0x854C30), GFX_RGB(0x346524),
    GFX_RGB(0xD04648), GFX_RGB(0x757161), GFX_RGB(0x597DCE), GFX_RGB(0xD27D2C), GFX_RGB(0x8595A1), GFX_RGB(0x6DAA2C),
    GFX_RGB(0xD2AA99), GFX_RGB(0x6DC2CA), GFX_RGB(0xDAD45E), GFX_RGB(0xDEEED6),
};

static const gfx_color_t db32[32] = {
    GFX_RGB(0x000000), GFX_RGB(0x222034), GFX_RGB(0x45283C), GFX_RGB(0x663931), GFX_RGB(0x8F563B), GFX_RGB(0xDF7126),
    GFX_RGB(0xD9A066), GFX_RGB(0xEEC39A), GFX_RGB(0xFBF236), GFX_RGB(0x99E550), GFX_RGB(0x6ABE30), GFX_RGB(0x37946E),
    GFX_RGB(0x4B692F), GFX_RGB(0x524B24), GFX_RGB(0x323C39), GFX_RGB(0x3F3F74), GFX_RGB(0x306082), GFX_RGB(0x5B6EE1),
    GFX_RGB(0x639BFF), GFX_RGB(0x5FCDE4), GFX_RGB(0xCBDBFC), GFX_RGB(0xFFFFFF), GFX_RGB(0x9BADB7), GFX_RGB(0x847E87),
    GFX_RGB(0x696A6A), GFX_RGB(0x595652), GFX_RGB(0x76428A), GFX_RGB(0xAC3232), GFX_RGB(0xD95763), GFX_RGB(0xD77BBA),
    GFX_RGB(0x8F974A), GFX_RGB(0x8A6F30),
};

/* i*17 for i in 0..15 - exact, since 15*17 == 255. */
static const gfx_color_t grayscale_16[16] = {
    GFX_RGB(0x000000), GFX_RGB(0x111111), GFX_RGB(0x222222), GFX_RGB(0x333333), GFX_RGB(0x444444), GFX_RGB(0x555555),
    GFX_RGB(0x666666), GFX_RGB(0x777777), GFX_RGB(0x888888), GFX_RGB(0x999999), GFX_RGB(0xAAAAAA), GFX_RGB(0xBBBBBB),
    GFX_RGB(0xCCCCCC), GFX_RGB(0xDDDDDD), GFX_RGB(0xEEEEEE), GFX_RGB(0xFFFFFF),
};

const gfx_palette_t gfx_palette_cga16 = {"cga16", cga_ega_16, 16};
const gfx_palette_t gfx_palette_ega16 = {"ega16", cga_ega_16, 16};
const gfx_palette_t gfx_palette_pico8_16 = {"pico8_16", pico8_16, 16};
const gfx_palette_t gfx_palette_db16 = {"db16", db16, 16};
const gfx_palette_t gfx_palette_db32 = {"db32", db32, 32};
const gfx_palette_t gfx_palette_vga256 = {"vga256", gfx_palette_vga256_entries, 256};
const gfx_palette_t gfx_palette_grayscale16 = {"grayscale16", grayscale_16, 16};
const gfx_palette_t gfx_palette_grayscale256 = {"grayscale256", gfx_palette_grayscale256_entries, 256};

static const gfx_palette_t* const registry[] = {
    &gfx_palette_cga16, &gfx_palette_ega16,  &gfx_palette_pico8_16,    &gfx_palette_db16,
    &gfx_palette_db32,  &gfx_palette_vga256, &gfx_palette_grayscale16, &gfx_palette_grayscale256,
};

#define REGISTRY_COUNT ((int)(sizeof registry / sizeof registry[0]))

const gfx_palette_t*
gfx_palette_standard_find(const char* name) {
    for (int i = 0; i < REGISTRY_COUNT; i++) {
        if (strcmp(registry[i]->name, name) == 0) {
            return registry[i];
        }
    }
    return NULL;
}

int
gfx_palette_standard_count(void) {
    return REGISTRY_COUNT;
}

const gfx_palette_t*
gfx_palette_standard_at(int index) {
    return registry[index];
}
