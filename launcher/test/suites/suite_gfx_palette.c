/* Host-only: exercises the host-side palette generator, which firmware never links. */

#include "suites.h"

#ifndef DEVICE_BUILD

#include "unity.h"

#include "gfx/gfx_indexed.h"
#include "gfx/gfx_palette_standard.h"
#include "gfx_palette_gen.h"

static void
test_cga16_has_16_entries_black_first_white_last(void) {
    TEST_ASSERT_EQUAL_INT(16, gfx_palette_cga16.count);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), gfx_palette_cga16.entries[0]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFFFFF), gfx_palette_cga16.entries[15]);
}

static void
test_ega16_is_bit_identical_to_cga16(void) {
    TEST_ASSERT_EQUAL_INT(gfx_palette_cga16.count, gfx_palette_ega16.count);
    for (int i = 0; i < gfx_palette_cga16.count; i++) {
        TEST_ASSERT_EQUAL_HEX16(gfx_palette_cga16.entries[i], gfx_palette_ega16.entries[i]);
    }
}

static void
test_pico8_16_first_and_last_entries(void) {
    TEST_ASSERT_EQUAL_INT(16, gfx_palette_pico8_16.count);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), gfx_palette_pico8_16.entries[0]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFCCAA), gfx_palette_pico8_16.entries[15]);
}

static void
test_db16_and_db32_sizes_and_known_entries(void) {
    TEST_ASSERT_EQUAL_INT(16, gfx_palette_db16.count);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x140C1C), gfx_palette_db16.entries[0]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xDEEED6), gfx_palette_db16.entries[15]);

    TEST_ASSERT_EQUAL_INT(32, gfx_palette_db32.count);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), gfx_palette_db32.entries[0]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x8A6F30), gfx_palette_db32.entries[31]);
}

static void
test_grayscale16_is_evenly_spaced(void) {
    TEST_ASSERT_EQUAL_INT(16, gfx_palette_grayscale16.count);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), gfx_palette_grayscale16.entries[0]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x888888), gfx_palette_grayscale16.entries[8]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFFFFF), gfx_palette_grayscale16.entries[15]);
}

static void
test_vga256_and_grayscale256_have_256_unique_entries(void) {
    TEST_ASSERT_EQUAL_INT(256, gfx_palette_vga256.count);
    TEST_ASSERT_EQUAL_INT(256, gfx_palette_grayscale256.count);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), gfx_palette_grayscale256.entries[0]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFFFFF), gfx_palette_grayscale256.entries[255]);

    for (int i = 0; i < gfx_palette_vga256.count; i++) {
        for (int j = i + 1; j < gfx_palette_vga256.count; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(gfx_palette_vga256.entries[i], gfx_palette_vga256.entries[j],
                                          "vga256 has a duplicate entry");
        }
    }
}

static void
test_standard_registry_finds_every_palette_by_name(void) {
    TEST_ASSERT_EQUAL_PTR(&gfx_palette_cga16, gfx_palette_standard_find("cga16"));
    TEST_ASSERT_EQUAL_PTR(&gfx_palette_db32, gfx_palette_standard_find("db32"));
    TEST_ASSERT_NULL(gfx_palette_standard_find("not-a-real-palette"));

    TEST_ASSERT_GREATER_OR_EQUAL_INT(8, gfx_palette_standard_count());
    bool saw_grayscale256 = false;
    for (int i = 0; i < gfx_palette_standard_count(); i++) {
        if (gfx_palette_standard_at(i) == &gfx_palette_grayscale256) {
            saw_grayscale256 = true;
        }
    }
    TEST_ASSERT_TRUE(saw_grayscale256);
}

/* Every one of a small palette's own entries maps back to itself exactly -
 * the round trip a nearest-in-OKLab search must get right when the target
 * IS one of the palette's own colours, not merely close to one. */
static void
test_index_map_round_trips_every_entry_of_a_small_palette(void) {
    static uint8_t map[65536];
    gfx_palette_gen_build_index_map(&gfx_palette_cga16, 0, map);

    for (int i = 0; i < gfx_palette_cga16.count; i++) {
        const int idx = gfx_palette_index_of(gfx_palette_cga16.entries[i], map);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i, idx, "a palette's own entry did not map back to its own index");
    }
}

/* first_index lets a caller reserve a UI block ahead of its own colour
 * entries - the reserved ones must never come back from an ordinary
 * lookup. */
static void
test_index_map_never_returns_a_reserved_entry(void) {
    static uint8_t map[65536];
    gfx_palette_gen_build_index_map(&gfx_palette_vga256, GFX_PALETTE_UI_ENTRIES, map);

    for (int i = 0; i < GFX_PALETTE_UI_ENTRIES; i++) {
        const int idx = gfx_palette_index_of(gfx_palette_vga256.entries[i], map);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(GFX_PALETTE_UI_ENTRIES, idx,
                                                 "a reserved UI entry's own colour mapped back into the "
                                                 "reserved block instead of past it");
    }
}

/* A 256-colour entry that IS exactly one of the 16-colour palette's own
 * entries dithers to that same colour at every phase - alpha 0, no
 * dithering needed, the trivial case any build must get exactly right. */
static void
test_dither_table_reproduces_an_exact_16_colour_match_at_every_phase(void) {
    static gfx_color_t table[GFX_PALETTE_MAX_ENTRIES * 16];
    gfx_palette_gen_build_dither16(&gfx_palette_vga256, &gfx_palette_cga16, table);

    /* gfx_palette_vga256's own first 16 entries are EGA16, bit-identical to
     * CGA16 (see test_ega16_is_bit_identical_to_cga16) - index 0 is black,
     * present in both palettes. */
    for (int phase = 0; phase < GFX_INDEXED_DITHER16_PHASES; phase++) {
        TEST_ASSERT_EQUAL_HEX16(gfx_palette_cga16.entries[0], table[0 * GFX_INDEXED_DITHER16_PHASES + phase]);
    }
}

static void
test_dither_table_is_deterministic(void) {
    static gfx_color_t a[GFX_PALETTE_MAX_ENTRIES * 16];
    static gfx_color_t b[GFX_PALETTE_MAX_ENTRIES * 16];
    gfx_palette_gen_build_dither16(&gfx_palette_db32, &gfx_palette_pico8_16, a);
    gfx_palette_gen_build_dither16(&gfx_palette_db32, &gfx_palette_pico8_16, b);

    for (int i = 0; i < gfx_palette_db32.count * GFX_INDEXED_DITHER16_PHASES; i++) {
        TEST_ASSERT_EQUAL_HEX16(a[i], b[i]);
    }
}

/* GFX_DITHER_NONE's own table: vga256's index 0 IS cga16's own black (see
 * test_ega16_is_bit_identical_to_cga16), so the nearest search must land
 * on it exactly, not merely close. */
static void
test_lut_nearest_reproduces_an_exact_match(void) {
    static gfx_color_t lut[GFX_PALETTE_MAX_ENTRIES];
    gfx_palette_gen_build_lut_nearest(&gfx_palette_vga256, &gfx_palette_cga16, lut);
    TEST_ASSERT_EQUAL_HEX16(gfx_palette_cga16.entries[0], lut[0]);
}

static void
test_lut_nearest_is_deterministic(void) {
    static gfx_color_t a[GFX_PALETTE_MAX_ENTRIES], b[GFX_PALETTE_MAX_ENTRIES];
    gfx_palette_gen_build_lut_nearest(&gfx_palette_db32, &gfx_palette_pico8_16, a);
    gfx_palette_gen_build_lut_nearest(&gfx_palette_db32, &gfx_palette_pico8_16, b);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(a, b, gfx_palette_db32.count);
}

/* GFX_DITHER_CELL_CHECKER's own table: an exact match is solid at both of
 * its 2 phases, the same trivial case the pixel modes must also get
 * right. */
static void
test_dither_cell_checker_reproduces_an_exact_match_at_both_phases(void) {
    static gfx_color_t table[GFX_PALETTE_MAX_ENTRIES * GFX_INDEXED_CELL_CHECKER_PHASES];
    gfx_palette_gen_build_dither_cell(&gfx_palette_vga256, &gfx_palette_cga16, false, table);
    for (int phase = 0; phase < GFX_INDEXED_CELL_CHECKER_PHASES; phase++) {
        TEST_ASSERT_EQUAL_HEX16(gfx_palette_cga16.entries[0], table[0 * GFX_INDEXED_CELL_CHECKER_PHASES + phase]);
    }
}

/* GFX_DITHER_CELL_BAYER2's own table: solid at all 4 of its phases. */
static void
test_dither_cell_bayer2_reproduces_an_exact_match_at_every_phase(void) {
    static gfx_color_t table[GFX_PALETTE_MAX_ENTRIES * GFX_INDEXED_CELL_BAYER2_PHASES];
    gfx_palette_gen_build_dither_cell(&gfx_palette_vga256, &gfx_palette_cga16, true, table);
    for (int phase = 0; phase < GFX_INDEXED_CELL_BAYER2_PHASES; phase++) {
        TEST_ASSERT_EQUAL_HEX16(gfx_palette_cga16.entries[0], table[0 * GFX_INDEXED_CELL_BAYER2_PHASES + phase]);
    }
}

static void
test_dither_cell_bayer2_is_deterministic(void) {
    static gfx_color_t a[GFX_PALETTE_MAX_ENTRIES * GFX_INDEXED_CELL_BAYER2_PHASES];
    static gfx_color_t b[GFX_PALETTE_MAX_ENTRIES * GFX_INDEXED_CELL_BAYER2_PHASES];
    gfx_palette_gen_build_dither_cell(&gfx_palette_db32, &gfx_palette_pico8_16, true, a);
    gfx_palette_gen_build_dither_cell(&gfx_palette_db32, &gfx_palette_pico8_16, true, b);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(a, b, gfx_palette_db32.count * GFX_INDEXED_CELL_BAYER2_PHASES);
}

/* GFX_DITHER_PIXEL_CHECKER2's own table: solid across all 4 (row, column)
 * phase combinations - the same exact-match case
 * gfx_palette_gen_build_dither16()'s own test covers for the 4x4 pattern. */
static void
test_dither_checker2_reproduces_an_exact_match_at_every_phase(void) {
    static gfx_color_t table[GFX_PALETTE_MAX_ENTRIES * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX];
    gfx_palette_gen_build_dither_checker2(&gfx_palette_vga256, &gfx_palette_cga16, table);
    const int stride = GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX;
    for (int i = 0; i < stride; i++) {
        TEST_ASSERT_EQUAL_HEX16(gfx_palette_cga16.entries[0], table[i]);
    }
}

static void
test_dither_checker2_is_deterministic(void) {
    static gfx_color_t a[GFX_PALETTE_MAX_ENTRIES * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX];
    static gfx_color_t b[GFX_PALETTE_MAX_ENTRIES * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX];
    gfx_palette_gen_build_dither_checker2(&gfx_palette_db32, &gfx_palette_pico8_16, a);
    gfx_palette_gen_build_dither_checker2(&gfx_palette_db32, &gfx_palette_pico8_16, b);
    const int n = gfx_palette_db32.count * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX;
    TEST_ASSERT_EQUAL_HEX16_ARRAY(a, b, n);
}

void
run_gfx_palette_suite(void) {
    RUN_TEST(test_cga16_has_16_entries_black_first_white_last);
    RUN_TEST(test_ega16_is_bit_identical_to_cga16);
    RUN_TEST(test_pico8_16_first_and_last_entries);
    RUN_TEST(test_db16_and_db32_sizes_and_known_entries);
    RUN_TEST(test_grayscale16_is_evenly_spaced);
    RUN_TEST(test_vga256_and_grayscale256_have_256_unique_entries);
    RUN_TEST(test_standard_registry_finds_every_palette_by_name);
    RUN_TEST(test_index_map_round_trips_every_entry_of_a_small_palette);
    RUN_TEST(test_index_map_never_returns_a_reserved_entry);
    RUN_TEST(test_dither_table_reproduces_an_exact_16_colour_match_at_every_phase);
    RUN_TEST(test_dither_table_is_deterministic);
    RUN_TEST(test_lut_nearest_reproduces_an_exact_match);
    RUN_TEST(test_lut_nearest_is_deterministic);
    RUN_TEST(test_dither_cell_checker_reproduces_an_exact_match_at_both_phases);
    RUN_TEST(test_dither_cell_bayer2_reproduces_an_exact_match_at_every_phase);
    RUN_TEST(test_dither_cell_bayer2_is_deterministic);
    RUN_TEST(test_dither_checker2_reproduces_an_exact_match_at_every_phase);
    RUN_TEST(test_dither_checker2_is_deterministic);
}

SUITE_REGISTER(run_gfx_palette_suite);

#else

void
run_gfx_palette_suite(void) {}

SUITE_REGISTER(run_gfx_palette_suite);

#endif
