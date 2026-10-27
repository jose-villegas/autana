/*
 * draw_palette_standard: a handful of well-known fixed palettes, for any
 * app that wants GFX_LAYOUT_INDEXED without building its own study.
 *
 * Selected by name or position, not a Kconfig choice; an app calls
 * draw_palette_standard_find() (or iterates draw_palette_standard_count()/
 * _at()) at runtime and installs the result through gfx_indexed_set_palette()
 * or gfx_indexed_set_lut16() (tools/gen/draw_palette_gen.h builds the reverse
 * index map and dither table either one needs first).
 */
#pragma once

#include "image/palette/palette.h"

/* Indices 0-15 are reserved for UI (the shell's own chrome, drawn through
 * whichever palette an app installed) in any palette meant for
 * GFX_LAYOUT_INDEXED; a shading colour has no business landing on one. */
#define DRAW_PALETTE_UI_ENTRIES 16

/* CGA/EGA share one array: EGA's default 16-colour text-mode palette is
 * bit-identical to CGA's (both IBM, early 1980s); see
 * https://en.wikipedia.org/wiki/List_of_software_palettes and
 * https://moddingwiki.shikadi.net/wiki/EGA_Palette. */
extern const palette_t draw_palette_cga16;
extern const palette_t draw_palette_ega16;

/* Lexaloffle Games' PICO-8 fantasy console default palette:
 * https://pico-8.fandom.com/wiki/Palette. */
extern const palette_t draw_palette_pico8_16;

/* DawnBringer (Richard "DawnBringer" Fhager)'s 16- and 32-colour pixel-art
 * palettes, published via PixelJoint: https://lospec.com/palette-list/
 * dawnbringer-16 and .../dawnbringer-32. */
extern const palette_t draw_palette_db16;
extern const palette_t draw_palette_db32;

/* 16 EGA + a 216-colour "web-safe" 6x6x6 RGB lattice + enough grays to
 * reach 256; see tools/gen/gen_draw_palette_standard.py for the construction. */
extern const palette_t draw_palette_vga256;

extern const palette_t draw_palette_grayscale16;
extern const palette_t draw_palette_grayscale256;

/* Name lookup, exact match, or NULL. */
const palette_t* draw_palette_standard_find(const char* name);

int draw_palette_standard_count(void);

/* 0 <= index < draw_palette_standard_count(), for listing rather than
 * naming one. */
const palette_t* draw_palette_standard_at(int index);
