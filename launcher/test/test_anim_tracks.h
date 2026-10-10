/* TRCK test fixture writer; curve arrays belong to each test. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "anim/anim_tracks.h"
#include "test_pack.h"

enum {
    TEST_TRACKS_HEADER_SIZE = ANIM_TRACKS_HEADER_SIZE,
    TEST_TRACK_ROW_SIZE = ANIM_TRACKS_ROW_SIZE,
    TEST_TRACK_STRING_SLOT = 64,
    TEST_TRACK_FIELD_OFFSET = 48,
};

typedef struct {
    const char* path;
    const char* field;
    uint32_t component;
    anim_value_t type;
    uint32_t times, values;
    int keys;
    anim_interp_t interp;
} test_track_t;

static inline void
test_tracks_header(uint8_t* entry, int count, uint32_t duration_ms) {
    test_pack_put16(entry + ANIM_TRACKS_AT_VERSION, ANIM_TRACKS_VERSION);
    test_pack_put16(entry + ANIM_TRACKS_AT_COUNT, count);
    test_pack_put32(entry + ANIM_TRACKS_AT_DURATION, duration_ms);
    test_pack_put32(entry + ANIM_TRACKS_AT_STRINGS, TEST_TRACKS_HEADER_SIZE + (count * TEST_TRACK_ROW_SIZE));
    test_pack_put32(entry + ANIM_TRACKS_AT_STRINGS_SIZE, count * TEST_TRACK_STRING_SLOT);
}

static inline void
test_track_row(uint8_t* entry, int index, const test_track_t* track) {
    uint8_t* row = entry + TEST_TRACKS_HEADER_SIZE + (index * TEST_TRACK_ROW_SIZE);
    const int count = entry[ANIM_TRACKS_AT_COUNT] | (entry[ANIM_TRACKS_AT_COUNT + 1] << 8);
    const uint32_t strings = TEST_TRACKS_HEADER_SIZE + (count * TEST_TRACK_ROW_SIZE);
    const uint16_t path = index * TEST_TRACK_STRING_SLOT;
    const uint16_t field = path + TEST_TRACK_FIELD_OFFSET;
    strcpy((char*)entry + strings + path, track->path);
    strcpy((char*)entry + strings + field, track->field);
    test_pack_put16(row + ANIM_TRACKS_ROW_PATH, path);
    test_pack_put16(row + ANIM_TRACKS_ROW_FIELD, field);
    test_pack_put32(row + ANIM_TRACKS_ROW_COMPONENT, track->component);
    test_pack_put32(row + ANIM_TRACKS_ROW_TIMES, track->times);
    test_pack_put32(row + ANIM_TRACKS_ROW_VALUES, track->values);
    test_pack_put16(row + ANIM_TRACKS_ROW_KEYS, track->keys);
    row[ANIM_TRACKS_ROW_TYPE] = (uint8_t)track->type;
    row[ANIM_TRACKS_ROW_INTERP] = (uint8_t)track->interp;
}
