/*
 * gfx_palette_standard - a handful of well-known fixed palettes, for any
 * app that wants GFX_PIXFMT_INDEXED8 without building its own study.
 *
 * Selected by name or position, not a Kconfig choice - an app calls
 * gfx_palette_standard_find() (or iterates gfx_palette_standard_count()/
 * _at()) at runtime and installs the result through gfx_indexed_set_lut()
 * or gfx_indexed_set_lut16() (tools/gen/gfx_palette_gen.h builds the reverse
 * index map and dither table either one needs first).
 */
#pragma once

#include "gfx/gfx_palette.h"

/* CGA/EGA share one array: EGA's default 16-colour text-mode palette is
 * bit-identical to CGA's (both IBM, early 1980s) - see
 * https://en.wikipedia.org/wiki/List_of_software_palettes and
 * https://moddingwiki.shikadi.net/wiki/EGA_Palette. */
extern const gfx_palette_t gfx_palette_cga16;
extern const gfx_palette_t gfx_palette_ega16;

/* Lexaloffle Games' PICO-8 fantasy console default palette -
 * https://pico-8.fandom.com/wiki/Palette. */
extern const gfx_palette_t gfx_palette_pico8_16;

/* DawnBringer (Richard "DawnBringer" Fhager)'s 16- and 32-colour pixel-art
 * palettes, published via PixelJoint - https://lospec.com/palette-list/
 * dawnbringer-16 and .../dawnbringer-32. */
extern const gfx_palette_t gfx_palette_db16;
extern const gfx_palette_t gfx_palette_db32;

/* 16 EGA + a 216-colour "web-safe" 6x6x6 RGB lattice + enough grays to
 * reach 256 - see tools/gen/gen_gfx_palette_standard.py for the construction. */
extern const gfx_palette_t gfx_palette_vga256;

extern const gfx_palette_t gfx_palette_grayscale16;
extern const gfx_palette_t gfx_palette_grayscale256;

/* Name lookup, exact match, or NULL. */
const gfx_palette_t* gfx_palette_standard_find(const char* name);

int gfx_palette_standard_count(void);

/* 0 <= index < gfx_palette_standard_count(), for listing rather than
 * naming one. */
const gfx_palette_t* gfx_palette_standard_at(int index);
