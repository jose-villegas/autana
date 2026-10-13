/* Shared independent bitmap reads, run counts and bounded icon collection for suites. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/draw/icon.h"
#include "unity.h"

static inline bool
icon_test_bit(const gfx_image_t* icon, int x, int y) {
    const unsigned bit = (unsigned)y * icon->stride + (unsigned)x;
    return (icon->bits[bit / 8] & (0x80 >> (bit % 8))) != 0;
}

static inline int
icon_test_runs(const gfx_image_t* icon) {
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
    icon_rect_t* blocks;
    int count;
    int cap;
} icon_test_collect_t;

static inline void
icon_test_collect(void* ctx, int x, int y, int w, int h) {
    icon_test_collect_t* cc = ctx;
    TEST_ASSERT_TRUE_MESSAGE(cc->count < cc->cap,
                             "icon_walk_blocks emitted more runs than the test's own buffer expects");
    cc->blocks[cc->count] = (icon_rect_t){x, y, w, h};
    cc->count++;
}
