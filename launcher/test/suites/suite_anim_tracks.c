/*
 * Portable suite: anim/anim_tracks, the TRCK pack entry. On a host the clip
 * tools/tests/anim_probe.py packs (AUTANA_ANIM_PROBE) samples as the Python
 * sampler does; on both, every clip in the shipped pack opens, and the boot
 * clip holds the same floats as the tracks compiled into the firmware.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "anim/anim_tracks.h"
#include "asset/asset_store.h"
#include "boot/boot_anim_tracks_generated.h"
#include "test_alloc.h"

#ifndef DEVICE_BUILD
#include "asset/asset_file.h"
#endif

/* Two tracks: "a/translation", linear, 3 wide, 2 keys; "lens/perspective/yfov",
 * step, 1 wide, 3 keys. Each array starts where the last ends. */
enum {
    ROW0 = 8,
    ROW1 = 56,
    A_TIMES = 104,
    A_VALUES = 112,
    B_TIMES = 136,
    B_VALUES = 148,
    ENTRY_BYTES = 160,
    ROW_SIZE = 48,
};

#define BOOT_CLIP "boot_anim_motion"

static void
put32(uint8_t* at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        at[i] = (uint8_t)(value >> (8 * i));
    }
}

static void
put16(uint8_t* at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void
put_floats(uint8_t* at, const float* values, int count) {
    memcpy(at, values, (size_t)count * sizeof(float));
}

static void
put_row(uint8_t* row, const char* name, uint32_t times, uint32_t values, uint32_t keys, uint8_t width,
        anim_interp_t interp) {
    strncpy((char*)row, name, ANIM_TRACK_NAME_MAX);
    put32(row + 32, times);
    put32(row + 36, values);
    put16(row + 40, keys);
    row[42] = width;
    row[43] = (uint8_t)interp;
}

typedef struct {
    uint8_t* entry;
    void* raw;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f;
    f.entry = test_alloc_aligned(ENTRY_BYTES, 16, &f.raw);
    TEST_ASSERT_NOT_NULL(f.entry);
    memset(f.entry, 0, ENTRY_BYTES);
    put16(f.entry, ANIM_TRACKS_VERSION);
    put16(f.entry + 2, 2);
    put32(f.entry + 4, 2000);
    put_row(f.entry + ROW0, "a/translation", A_TIMES, A_VALUES, 2, 3, ANIM_LINEAR);
    put_row(f.entry + ROW1, "lens/perspective/yfov", B_TIMES, B_VALUES, 3, 1, ANIM_STEP);
    const float a_times[] = {0.0F, 2.0F};
    const float a_values[] = {0.0F, 0.0F, 0.0F, 4.0F, -2.0F, 8.0F};
    const float b_times[] = {0.0F, 1.0F, 1.5F};
    const float b_values[] = {0.5F, 0.7F, 0.9F};
    put_floats(f.entry + A_TIMES, a_times, 2);
    put_floats(f.entry + A_VALUES, a_values, 6);
    put_floats(f.entry + B_TIMES, b_times, 3);
    put_floats(f.entry + B_VALUES, b_values, 3);
    return f;
}

static asset_status_t
open_fixture(const fixture_t* f, uint32_t size, anim_tracks_t* tracks) {
    return anim_tracks_open((asset_view_t){f->entry, size}, tracks);
}

static void
test_an_entry_opens_and_a_found_track_points_into_it(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_fixture(&f, ENTRY_BYTES, &tracks));
    TEST_ASSERT_EQUAL_UINT16(2, tracks.count);
    TEST_ASSERT_EQUAL_UINT32(2000, tracks.clip.duration_ms);
    anim_track_t track;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_find(&tracks, "a/translation", &track));
    TEST_ASSERT_EQUAL_PTR(f.entry + A_TIMES, track.times);
    TEST_ASSERT_EQUAL_PTR(f.entry + A_VALUES, track.values);
    TEST_ASSERT_EQUAL_UINT16(2, track.count);
    TEST_ASSERT_EQUAL_UINT8(3, track.width);
    float out[ANIM_WIDTH_MAX];
    anim_track_sample(&track, anim_clip_seconds(&tracks.clip, 1000, ANIM_LOOP), out);
    TEST_ASSERT_EQUAL_FLOAT(2.0F, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, out[1]);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_find(&tracks, "lens/perspective/yfov", &track));
    TEST_ASSERT_EQUAL_UINT8(ANIM_STEP, track.interp);
    anim_track_sample(&track, 1.2F, out);
    TEST_ASSERT_EQUAL_FLOAT(0.7F, out[0]);
    test_free_aligned(f.raw);
}

static void
test_a_missing_name_is_not_found(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    anim_track_t track;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_fixture(&f, ENTRY_BYTES, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, anim_tracks_find(&tracks, "a/rotation", &track));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND,
                          anim_tracks_find(&tracks, "a/translation/and/more/than/32/chars", &track));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, anim_tracks_find(&tracks, "a/", &track));
    test_free_aligned(f.raw);
}

static void
test_a_truncated_header_or_table_is_out_of_bounds(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, 6, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ROW1 + 20, &tracks));
    TEST_ASSERT_EQUAL_UINT16(0, tracks.count);
    /* Cut inside row 0, whose padding past the cut is in use: the table is
     * refused before any row is read. */
    f.entry[ROW0 + 45] = 1;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ROW0 + 20, &tracks));
    test_free_aligned(f.raw);
}

/* A count past 255 is read whole: 300 keys, linear, one wide. */
static void
test_a_track_of_more_than_255_keys_reports_them_all(void) {
    enum { KEYS = 300, TIMES_AT = 56, VALUES_AT = TIMES_AT + (KEYS * 4), BYTES = VALUES_AT + (KEYS * 4) };

    void* raw;
    uint8_t* entry = test_alloc_aligned(BYTES, 16, &raw);
    TEST_ASSERT_NOT_NULL(entry);
    memset(entry, 0, BYTES);
    put16(entry, ANIM_TRACKS_VERSION);
    put16(entry + 2, 1);
    put_row(entry + ROW0, "long/scale", TIMES_AT, VALUES_AT, KEYS, 1, ANIM_LINEAR);
    anim_tracks_t tracks;
    anim_track_t track;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){entry, BYTES}, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_find(&tracks, "long/scale", &track));
    TEST_ASSERT_EQUAL_UINT16(KEYS, track.count);
    test_free_aligned(raw);
}

static void
test_an_array_past_the_end_is_out_of_bounds(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES - 4, &tracks));
    put16(f.entry + ROW0 + 40, 0xFFFF); /* keys enough to run far past the entry */
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

/* Track B's 3 keys fit 1 wide but not 4: the values check counts the width. */
static void
test_values_that_leave_the_entry_only_by_their_width_are_out_of_bounds(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    f.entry[ROW1 + 42] = 4;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

/* Track A's two times start 4 bytes before the end while its values fit. */
static void
test_times_that_alone_leave_the_entry_are_out_of_bounds(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    put32(f.entry + ROW0 + 32, ENTRY_BYTES - 4);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

/* An offset whose end wraps past 2^32 back into the entry is still out of it. */
static void
test_an_offset_that_wraps_round_is_out_of_bounds(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    put32(f.entry + ROW0 + 32, 0xFFFFFFFCU);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

static void
test_misaligned_floats_or_floats_inside_the_table_are_out_of_bounds(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    put32(f.entry + ROW1 + 32, B_TIMES + 2);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
    put32(f.entry + ROW1 + 32, ROW1);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

static void
test_an_entry_off_a_4_byte_boundary_is_out_of_bounds(void) {
    fixture_t f = fixture();
    void* raw;
    uint8_t* shifted = test_alloc_aligned(ENTRY_BYTES + 4, 16, &raw);
    TEST_ASSERT_NOT_NULL(shifted);
    memcpy(shifted + 2, f.entry, ENTRY_BYTES);
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, anim_tracks_open((asset_view_t){shifted + 2, ENTRY_BYTES}, &tracks));
    test_free_aligned(raw);
    test_free_aligned(f.raw);
}

/* Field `offset` of row 0 set to `value`: the status the entry then opens with. */
static asset_status_t
open_with_row_byte(int offset, uint8_t value) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    f.entry[ROW0 + offset] = value;
    const asset_status_t status = open_fixture(&f, ENTRY_BYTES, &tracks);
    test_free_aligned(f.raw);
    return status;
}

static void
test_a_bad_width_interpolation_or_flag_is_a_format_error(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(42, 0));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(42, ANIM_WIDTH_MAX + 1));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(43, ANIM_CUBIC + 1));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(44, 2));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(44, 1)); /* a quaternion 3 wide */
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(45, 1)); /* padding in use, each byte */
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(46, 1));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(47, 1));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_with_row_byte(40, 0)); /* no keys (count's low byte; high is 0) */
}

/* Row 0 made 4 wide, which its values still fit: flag 1 opens, flag 2 is a
 * format error whatever the width. */
static void
test_a_quaternion_flag_past_1_is_a_format_error_even_4_wide(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    f.entry[ROW0 + 42] = 4;
    f.entry[ROW0 + 44] = 1;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_fixture(&f, ENTRY_BYTES, &tracks));
    f.entry[ROW0 + 44] = 2;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

static void
test_an_unterminated_name_is_a_format_error(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    memset(f.entry + ROW1, 'x', ANIM_TRACK_NAME_MAX);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

static void
test_an_unknown_version_is_refused(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    put16(f.entry, ANIM_TRACKS_VERSION + 1U);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

static void
test_a_cubic_track_needs_three_runs_of_values(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    f.entry[ROW1 + 43] = ANIM_CUBIC; /* 3 keys x 3 runs: 36 bytes from 148 leave the entry */
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
    test_free_aligned(f.raw);
}

/* Every TRCK entry of `pack` opens and finds each of its tracks by name. */
static int
open_every_clip(const asset_pack_t* pack) {
    int clips = 0;
    for (uint32_t i = 0; i < pack->count; i++) {
        asset_entry_t entry;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_entry(pack, i, &entry));
        if (entry.type != ANIM_TRACKS_ASSET) {
            continue;
        }
        anim_tracks_t tracks;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, anim_tracks_from_pack(pack, entry.name, &tracks), entry.name);
        TEST_ASSERT_GREATER_THAN_UINT16_MESSAGE(0, tracks.count, entry.name);
        TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, tracks.clip.duration_ms, entry.name);
        for (int t = 0; t < tracks.count; t++) {
            anim_track_t track;
            const char* name = (const char*)(tracks.base + ROW0 + ((size_t)t * ROW_SIZE));
            TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, anim_tracks_find(&tracks, name, &track), name);
        }
        clips++;
    }
    return clips;
}

static void
test_every_clip_in_the_shipped_pack_opens(void) {
    const asset_pack_t* pack = asset_store_pack();
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "the asset pack did not open: see the log above");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, open_every_clip(pack), "the pack holds no clip");
}

/* The boot clip, baked into the pack and into C from one .glb by one set of
 * checks, holds the same floats either way. */
static void
test_the_shipped_boot_clip_equals_the_compiled_one(void) {
    const asset_pack_t* pack = asset_store_pack();
    TEST_ASSERT_NOT_NULL(pack);
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_from_pack(pack, BOOT_CLIP, &tracks));
    TEST_ASSERT_EQUAL_UINT32(boot_anim_clip.duration_ms, tracks.clip.duration_ms);
    TEST_ASSERT_EQUAL_INT(boot_anim_track_count, tracks.count);
    for (int i = 0; i < boot_anim_track_count; i++) {
        const anim_track_t* want = boot_anim_tracks[i];
        anim_track_t got;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, anim_tracks_find(&tracks, boot_anim_track_names[i], &got),
                                      boot_anim_track_names[i]);
        TEST_ASSERT_EQUAL_UINT16(want->count, got.count);
        TEST_ASSERT_EQUAL_UINT8(want->width, got.width);
        TEST_ASSERT_EQUAL_UINT8(want->interp, got.interp);
        TEST_ASSERT_EQUAL_UINT8(want->quaternion, got.quaternion);
        const size_t runs = want->interp == ANIM_CUBIC ? 3U : 1U;
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want->times, got.times, sizeof(float) * want->count, boot_anim_track_names[i]);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want->values, got.values, sizeof(float) * runs * want->count * want->width,
                                         boot_anim_track_names[i]);
    }
}

#ifndef DEVICE_BUILD
/* The pack run_tests.sh has anim_probe.py write, opened. */
static void
open_probe(asset_pack_t* pack, void** buffer) {
    const char* path = getenv("AUTANA_ANIM_PROBE");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "AUTANA_ANIM_PROBE names the pack anim_probe.py wrote");
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(path, pack, buffer));
}

#define REFERENCE_TYPE ASSET_TYPE('T', 'R', 'E', 'F')
#define REFERENCE_ROW  24U

static uint32_t
get32(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static float
get_float(const uint8_t* at) {
    float value;
    memcpy(&value, at, sizeof value);
    return value;
}

/* One reference row: track `index` at `seconds` against the Python sampler. */
static void
check_reference_row(const anim_tracks_t* tracks, const uint8_t* row) {
    const int index = row[0] | (row[1] << 8);
    TEST_ASSERT_LESS_THAN_INT(tracks->count, index);
    const char* name = (const char*)(tracks->base + ROW0 + ((size_t)index * ROW_SIZE));
    anim_track_t track;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, anim_tracks_find(tracks, name, &track), name);
    const float seconds = get_float(row + 4);
    float got[ANIM_WIDTH_MAX];
    anim_track_sample(&track, seconds, got);
    char what[64];
    for (int k = 0; k < track.width; k++) {
        const float want = get_float(row + 8 + (4 * k));
        snprintf(what, sizeof what, "%s[%d] at %.6f s", name, k, (double)seconds);
        if (row[2] != 0) {
            TEST_ASSERT_TRUE_MESSAGE(got[k] == want, what);
        } else {
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2e-5F, want, got[k], what);
        }
    }
}

static void
test_the_probe_clip_samples_as_the_python_sampler(void) {
    asset_pack_t pack;
    void* buffer = NULL;
    open_probe(&pack, &buffer);
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_from_pack(&pack, "probe", &tracks));
    asset_view_t reference;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&pack, "probe_ref", REFERENCE_TYPE, &reference));
    TEST_ASSERT_EQUAL_UINT32(get32(reference.data), tracks.clip.duration_ms);
    const uint32_t rows = get32(reference.data + 4);
    TEST_ASSERT_EQUAL_UINT32(8U + (rows * REFERENCE_ROW), reference.size);
    TEST_ASSERT_GREATER_THAN_UINT32(tracks.count * 100U, rows);
    for (uint32_t r = 0; r < rows; r++) {
        check_reference_row(&tracks, reference.data + 8 + (r * REFERENCE_ROW));
    }
    asset_file_release(buffer);
}

static void
test_a_missing_clip_and_an_entry_of_another_type_are_told_apart(void) {
    asset_pack_t pack;
    void* buffer = NULL;
    open_probe(&pack, &buffer);
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, anim_tracks_from_pack(&pack, "nope", &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TYPE, anim_tracks_from_pack(&pack, "probe_ref", &tracks));
    TEST_ASSERT_NULL(tracks.base);
    asset_file_release(buffer);
}
#endif

void
suite_anim_tracks(void) {
    RUN_TEST(test_an_entry_opens_and_a_found_track_points_into_it);
    RUN_TEST(test_a_missing_name_is_not_found);
    RUN_TEST(test_a_truncated_header_or_table_is_out_of_bounds);
    RUN_TEST(test_an_array_past_the_end_is_out_of_bounds);
    RUN_TEST(test_a_track_of_more_than_255_keys_reports_them_all);
    RUN_TEST(test_values_that_leave_the_entry_only_by_their_width_are_out_of_bounds);
    RUN_TEST(test_times_that_alone_leave_the_entry_are_out_of_bounds);
    RUN_TEST(test_an_offset_that_wraps_round_is_out_of_bounds);
    RUN_TEST(test_misaligned_floats_or_floats_inside_the_table_are_out_of_bounds);
    RUN_TEST(test_an_entry_off_a_4_byte_boundary_is_out_of_bounds);
    RUN_TEST(test_a_bad_width_interpolation_or_flag_is_a_format_error);
    RUN_TEST(test_a_quaternion_flag_past_1_is_a_format_error_even_4_wide);
    RUN_TEST(test_an_unterminated_name_is_a_format_error);
    RUN_TEST(test_an_unknown_version_is_refused);
    RUN_TEST(test_a_cubic_track_needs_three_runs_of_values);
    RUN_TEST(test_every_clip_in_the_shipped_pack_opens);
    RUN_TEST(test_the_shipped_boot_clip_equals_the_compiled_one);
#ifndef DEVICE_BUILD
    RUN_TEST(test_the_probe_clip_samples_as_the_python_sampler);
    RUN_TEST(test_a_missing_clip_and_an_entry_of_another_type_are_told_apart);
#endif
}

SUITE_REGISTER(suite_anim_tracks);
