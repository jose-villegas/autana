/* Portable TRCK binding validation and sampler reference tests. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anim/anim_tracks.h"
#include "asset/asset_store.h"
#include "suites.h"
#include "test_alloc.h"
#include "test_anim_tracks.h"
#include "test_pack.h"
#include "unity.h"
#ifndef DEVICE_BUILD
#include "asset/asset_file.h"
#endif
#define BOOT_CLIP "boot_anim_motion"

enum {
    ROW0 = ANIM_TRACKS_HEADER_SIZE,
    STRINGS = ROW0 + ANIM_TRACKS_ROW_SIZE,
    STRING_BYTES = 16,
    TIMES = STRINGS + STRING_BYTES,
    VALUES = TIMES + 2 * sizeof(float),
    ENTRY_BYTES = VALUES + 2 * ANIM_WIDTH_MAX * ANIM_TRACKS_CUBIC_RUNS * sizeof(float),
};

typedef struct {
    uint8_t* entry;
    void* raw;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f;
    f.entry = test_alloc_aligned(ENTRY_BYTES, ANIM_TRACKS_ALIGNMENT, &f.raw);
    TEST_ASSERT_NOT_NULL(f.entry);
    memset(f.entry, 0, ENTRY_BYTES);
    test_tracks_header(f.entry, 1, 2000);
    test_pack_put32(f.entry + ANIM_TRACKS_AT_STRINGS_SIZE, STRING_BYTES);
    memcpy(f.entry + STRINGS, "n\0position\0", sizeof "n\0position\0");
    test_pack_put16(f.entry + ROW0 + ANIM_TRACKS_ROW_FIELD, 2);
    test_pack_put32(f.entry + ROW0 + ANIM_TRACKS_ROW_COMPONENT, ANIM_COMPONENT_TRANSFORM);
    test_pack_put32(f.entry + ROW0 + ANIM_TRACKS_ROW_TIMES, TIMES);
    test_pack_put32(f.entry + ROW0 + ANIM_TRACKS_ROW_VALUES, VALUES);
    test_pack_put16(f.entry + ROW0 + ANIM_TRACKS_ROW_KEYS, 2);
    f.entry[ROW0 + ANIM_TRACKS_ROW_TYPE] = ANIM_VALUE_VEC3;
    f.entry[ROW0 + ANIM_TRACKS_ROW_INTERP] = ANIM_LINEAR;
    const float times[] = {0, 2};
    test_pack_put_floats(f.entry + TIMES, times, 2);
    return f;
}

static asset_status_t
open_fixture(const fixture_t* f, uint32_t size, anim_tracks_t* out) {
    return anim_tracks_open((asset_view_t){f->entry, size}, out);
}

static void
test_types_and_interpolations(void) {
    for (int type = ANIM_VALUE_FLOAT; type <= ANIM_VALUE_COLOUR; type++) {
        for (int interp = ANIM_STEP; interp <= ANIM_CUBIC; interp++) {
            fixture_t f = fixture();
            f.entry[ROW0 + ANIM_TRACKS_ROW_TYPE] = type;
            f.entry[ROW0 + ANIM_TRACKS_ROW_INTERP] = interp;
            anim_tracks_t tracks;
            TEST_ASSERT_EQUAL_INT(ASSET_OK, open_fixture(&f, ENTRY_BYTES, &tracks));
            anim_binding_t b;
            TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_binding_at(&tracks, 0, &b));
            static const uint8_t WIDTHS[] = {1, 2, 3, 4, 3};
            TEST_ASSERT_EQUAL_INT(type, b.type);
            TEST_ASSERT_EQUAL_INT(WIDTHS[type], b.curve.width);
            TEST_ASSERT_EQUAL_INT(type == ANIM_VALUE_QUAT, b.curve.quaternion);
            TEST_ASSERT_EQUAL_STRING("n", b.path);
            TEST_ASSERT_EQUAL_STRING("position", b.field);
            anim_track_t curve;
            TEST_ASSERT_EQUAL_INT(ASSET_OK,
                                  anim_tracks_find(&tracks, "n", ANIM_COMPONENT_TRANSFORM, "position", &curve));
            TEST_ASSERT_EQUAL_PTR(f.entry + VALUES, curve.values);
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND,
                                  anim_tracks_find(&tracks, "n", ANIM_COMPONENT_CAMERA, "position", &curve));
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND,
                                  anim_tracks_find(&tracks, "m", ANIM_COMPONENT_TRANSFORM, "position", &curve));
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND,
                                  anim_tracks_find(&tracks, "n", ANIM_COMPONENT_TRANSFORM, "scale", &curve));
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, anim_tracks_binding_at(&tracks, -1, &b));
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, anim_tracks_binding_at(&tracks, tracks.count, &b));
            test_free_aligned(f.raw);
        }
    }
}

static void
test_invalid_fields_and_padding(void) {
    static const struct {
        int offset;
        uint8_t value;
    } CASES[] = {
        {ANIM_TRACKS_AT_ROOT, ANIM_ROOT_SKELETON + 1},
        {ANIM_TRACKS_AT_PAD, 1},
        {ANIM_TRACKS_AT_PAD + 1, 1},
        {ANIM_TRACKS_AT_PAD + 2, 1},
        {ROW0 + ANIM_TRACKS_ROW_TYPE, ANIM_VALUE_COLOUR + 1},
        {ROW0 + ANIM_TRACKS_ROW_INTERP, ANIM_CUBIC + 1},
        {ROW0 + ANIM_TRACKS_ROW_COMPONENT, 0},
        {ROW0 + ANIM_TRACKS_ROW_KEYS, 0},
        {ROW0 + ANIM_TRACKS_ROW_PAD, 1},
        {ROW0 + ANIM_TRACKS_ROW_PAD + 1, 1},
        {ROW0 + ANIM_TRACKS_ROW_PAD + 2, 1},
        {ROW0 + ANIM_TRACKS_ROW_PAD + 3, 1},
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        fixture_t f = fixture();
        anim_tracks_t tracks;
        f.entry[CASES[i].offset] = CASES[i].value;
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_fixture(&f, ENTRY_BYTES, &tracks));
        test_free_aligned(f.raw);
    }
}

static void
test_strings_times_and_values(void) {
    for (int which = 0; which < 7; which++) {
        fixture_t f = fixture();
        anim_tracks_t tracks;
        if (which == 0) {
            test_pack_put16(f.entry + ROW0 + ANIM_TRACKS_ROW_PATH, STRING_BYTES);
        }
        if (which == 1) {
            test_pack_put16(f.entry + ROW0 + ANIM_TRACKS_ROW_FIELD, STRING_BYTES);
        }
        if (which == 2) {
            memset(f.entry + STRINGS, 'x', STRING_BYTES);
        }
        if (which == 3) {
            const float v[] = {0, 0};
            test_pack_put_floats(f.entry + TIMES, v, 2);
        }
        if (which == 4) {
            const float v[] = {0, NAN};
            test_pack_put_floats(f.entry + TIMES, v, 2);
        }
        if (which == 5) {
            const float v[] = {INFINITY};
            test_pack_put_floats(f.entry + VALUES, v, 1);
        }
        if (which == 6) {
            const float v[] = {2, 1};
            test_pack_put_floats(f.entry + TIMES, v, 2);
        }
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_fixture(&f, ENTRY_BYTES, &tracks));
        test_free_aligned(f.raw);
    }
}

static void
test_version_and_bounds(void) {
    fixture_t f = fixture();
    anim_tracks_t tracks;
    test_pack_put16(f.entry, 1);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, open_fixture(&f, ENTRY_BYTES, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, open_fixture(&f, sizeof(uint16_t), &tracks));
    test_pack_put16(f.entry, ANIM_TRACKS_VERSION);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ANIM_TRACKS_HEADER_SIZE - 1, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, STRINGS - 1, &tracks));

    static const struct {
        int offset;
        uint32_t value;
    } CASES[] = {
        {ANIM_TRACKS_AT_STRINGS, STRINGS - ANIM_TRACKS_ALIGNMENT},
        {ANIM_TRACKS_AT_STRINGS_SIZE, ENTRY_BYTES},
        {ROW0 + ANIM_TRACKS_ROW_TIMES, TIMES + 1},
        {ROW0 + ANIM_TRACKS_ROW_VALUES, ENTRY_BYTES},
        {ROW0 + ANIM_TRACKS_ROW_TIMES, STRINGS},
    };

    test_free_aligned(f.raw);
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        f = fixture();
        test_pack_put32(f.entry + CASES[i].offset, CASES[i].value);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_fixture(&f, ENTRY_BYTES, &tracks));
        test_free_aligned(f.raw);
    }
}

static void
test_large_key_count_and_sized_cubic_values(void) {
    enum {
        KEYS = 300,
        VALUES_AT = TIMES + KEYS * sizeof(float),
        BYTES = VALUES_AT + KEYS * ANIM_TRACKS_CUBIC_RUNS * sizeof(float)
    };

    uint8_t* entry = calloc(1, BYTES);
    TEST_ASSERT_NOT_NULL(entry);
    fixture_t seed = fixture();
    memcpy(entry, seed.entry, VALUES);
    test_free_aligned(seed.raw);
    entry[ROW0 + ANIM_TRACKS_ROW_TYPE] = ANIM_VALUE_FLOAT;
    entry[ROW0 + ANIM_TRACKS_ROW_INTERP] = ANIM_CUBIC;
    test_pack_put16(entry + ROW0 + ANIM_TRACKS_ROW_KEYS, KEYS);
    test_pack_put32(entry + ROW0 + ANIM_TRACKS_ROW_VALUES, VALUES_AT);
    for (int i = 0; i < KEYS; i++) {
        const float time[] = {(float)i};
        test_pack_put_floats(entry + TIMES + i * sizeof(float), time, 1);
    }
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){entry, BYTES}, &tracks));
    anim_binding_t b;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_binding_at(&tracks, 0, &b));
    TEST_ASSERT_EQUAL_UINT16(KEYS, b.curve.count);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, anim_tracks_open((asset_view_t){entry, BYTES - sizeof(float)}, &tracks));
    free(entry);
}

static void
test_find_node_required_parts_and_default_scale(void) {
    enum { COUNT = 2, BYTES = 512, TIMES_AT = 256, POSITION_AT = 260, ROTATION_AT = 272 };

    uint8_t* entry = calloc(1, BYTES);
    TEST_ASSERT_NOT_NULL(entry);
    test_tracks_header(entry, COUNT, 1000);
    test_track_row(entry, 0,
                   &(test_track_t){.name = "node/translation",
                                   .times = TIMES_AT,
                                   .values = POSITION_AT,
                                   .keys = 1,
                                   .width = 3,
                                   .interp = ANIM_STEP});
    test_track_row(entry, 1,
                   &(test_track_t){.name = "node/rotation",
                                   .times = TIMES_AT,
                                   .values = ROTATION_AT,
                                   .keys = 1,
                                   .width = 4,
                                   .quaternion = true,
                                   .interp = ANIM_STEP});
    const float rotation[] = {0, 0, 0, 1};
    test_pack_put_floats(entry + ROTATION_AT, rotation, ANIM_WIDTH_MAX);
    anim_tracks_t tracks;
    anim_node_tracks_t node;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){entry, BYTES}, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_find_node(&tracks, "node", &node));
    float scale[ANIM_WIDTH_MAX];
    anim_track_sample(&node.scale, 0, scale);
    TEST_ASSERT_EQUAL_FLOAT(1, scale[0]);
    TEST_ASSERT_EQUAL_FLOAT(1, scale[1]);
    TEST_ASSERT_EQUAL_FLOAT(1, scale[2]);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, anim_tracks_find_node(&tracks, "missing", &node));
    entry[ROW0 + ANIM_TRACKS_ROW_TYPE] = ANIM_VALUE_VEC2;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){entry, BYTES}, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, anim_tracks_find_node(&tracks, "node", &node));
    entry[ROW0 + ANIM_TRACKS_ROW_TYPE] = ANIM_VALUE_VEC3;
    entry[ROW0 + ANIM_TRACKS_ROW_SIZE + ANIM_TRACKS_ROW_TYPE] = ANIM_VALUE_COLOUR;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){entry, BYTES}, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, anim_tracks_find_node(&tracks, "node", &node));
    free(entry);
}

static int
open_every_clip(const asset_pack_t* pack) {
    int clips = 0;
    for (uint32_t i = 0; i < pack->count; i++) {
        asset_entry_t e;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_entry(pack, i, &e));
        if (e.type != ANIM_TRACKS_ASSET) {
            continue;
        }
        anim_tracks_t tracks;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_from_pack(pack, e.name, &tracks));
        for (int j = 0; j < tracks.count; j++) {
            anim_binding_t b;
            anim_track_t curve;
            TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_binding_at(&tracks, j, &b));
            TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_find(&tracks, b.path, b.component, b.field, &curve));
        }
        clips++;
    }
    return clips;
}

static void
test_every_clip_in_the_boot_clip_s_pack_opens(void) {
    const asset_pack_t* pack = asset_store_pack(BOOT_CLIP);
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "the boot clip's pack did not mount: see the log above");
    const int clips = open_every_clip(pack);
    asset_store_release(BOOT_CLIP);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, clips, "the pack holds no clip");
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
    anim_binding_t binding;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_binding_at(tracks, index, &binding));
    const anim_track_t track = binding.curve;
    const char* name = binding.path;
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
    RUN_TEST(test_types_and_interpolations);
    RUN_TEST(test_large_key_count_and_sized_cubic_values);
    RUN_TEST(test_find_node_required_parts_and_default_scale);
    RUN_TEST(test_invalid_fields_and_padding);
    RUN_TEST(test_strings_times_and_values);
    RUN_TEST(test_version_and_bounds);
    RUN_TEST(test_every_clip_in_the_boot_clip_s_pack_opens);
#ifndef DEVICE_BUILD
    RUN_TEST(test_the_probe_clip_samples_as_the_python_sampler);
    RUN_TEST(test_a_missing_clip_and_an_entry_of_another_type_are_told_apart);
#endif
}

SUITE_REGISTER(suite_anim_tracks);
