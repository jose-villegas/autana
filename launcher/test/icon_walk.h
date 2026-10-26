/* Shared independent bitmap reads, run counts and bounded icon collection for suites. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "draw/icon.h"
#include "unity.h"

static inline bool
icon_test_bit(const image_t* icon, int x, int y) {
    const uint8_t* row = image_row(icon, y);
    return image_mono1_ink(row, x);
}

static inline int
icon_test_runs(const image_t* icon) {
    int total = 0;
    for (int y = 0; y < icon->height; y++) {
        bool in_run = false;
        for (int x = 0; x < icon->width; x++) {
            const bool on = icon_test_bit(icon, x, y);
            if (on && !in_run) {
                total++;
            }
            in_run = on;
        }
    }
    return total;
}

typedef struct {
    recti_t* blocks;
    int count;
    int cap;
} icon_test_collect_t;

static inline void
icon_test_collect(void* ctx, recti_t rect) {
    icon_test_collect_t* cc = ctx;
    TEST_ASSERT_TRUE_MESSAGE(cc->count < cc->cap,
                             "icon_walk_blocks emitted more runs than the test's own buffer expects");
    cc->blocks[cc->count] = rect;
    cc->count++;
}
