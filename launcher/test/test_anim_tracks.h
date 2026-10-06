/* Writes a TRCK entry's header and track rows, the layout
 * docs/Animation-Tracks.md gives, into zeroed bytes; the arrays a row points
 * at are the test's to fill. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "anim/anim_tracks.h"
#include "test_pack.h"

enum { TEST_TRACKS_HEADER_SIZE = 8, TEST_TRACK_ROW_SIZE = 48 };

typedef struct {
    const char* name;
    uint32_t times, values;
    int keys, width;
    anim_interp_t interp;
    bool quaternion;
} test_track_t;

static inline void
test_tracks_header(uint8_t* entry, int count, uint32_t duration_ms) {
    test_pack_put16(entry, ANIM_TRACKS_VERSION);
    test_pack_put16(entry + 2, count);
    test_pack_put32(entry + 4, duration_ms);
}

static inline void
test_track_row(uint8_t* entry, int index, const test_track_t* track) {
    uint8_t* row = entry + TEST_TRACKS_HEADER_SIZE + (index * TEST_TRACK_ROW_SIZE);
    strncpy((char*)row, track->name, ANIM_TRACK_NAME_MAX);
    test_pack_put32(row + 32, track->times);
    test_pack_put32(row + 36, track->values);
    test_pack_put16(row + 40, track->keys);
    row[42] = (uint8_t)track->width;
    row[43] = (uint8_t)track->interp;
    row[44] = track->quaternion ? 1U : 0U;
}
