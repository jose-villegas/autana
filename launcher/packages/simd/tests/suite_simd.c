/*
 * Every simd call against its C reference, bit for bit: each start offset
 * within a vector, lengths through the minimum and past several vectors,
 * every element distinct and extreme values, and guard elements around the output that no
 * call may touch. On the board this compares the PIE path; elsewhere both
 * sides are the reference, so the suite runs everywhere.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "testkit/suite.h"
#include "unity.h"

#include "simd/copy.h"
#include "simd/fill.h"
#include "simd/repeat.h"
#include "simd/simd.h"

#define GUARD          SIMD_ALIGN
#define GUARD_U16      0xA5A5U
#define GUARD_U8       0xA5U
/* Test data: element i is i * PATTERN_STEP + PATTERN_OFFSET, truncated. The
 * step is odd, so neighbours differ in every byte and 16-bit lane, and a
 * lane moved to the wrong place shows. libc rand() is not used: its state is
 * thread-local and would cost every task stack. */
#define PATTERN_STEP   0x9E37U
#define PATTERN_OFFSET 0x5A5BU

/* Long runs: a screen row, the other screen axis, and one past any row. */
static const size_t long_lengths[] = {368, 448, 1000};

/* Copy lengths around the minimum and one and two steps of two vectors past it. */
static const size_t copy_lengths[] = {
    SIMD_COPY_MIN - 1,
    SIMD_COPY_MIN,
    SIMD_COPY_MIN + 1,
    SIMD_COPY_MIN + (2 * SIMD_ALIGN) - 1,
    SIMD_COPY_MIN + (2 * SIMD_ALIGN),
    SIMD_COPY_MIN + (4 * SIMD_ALIGN) + 1,
    368 * sizeof(uint16_t),
    1000,
};

static uint32_t
pattern(size_t i) {
    return ((uint32_t)i * PATTERN_STEP) + PATTERN_OFFSET;
}

static uint16_t*
alloc_u16(size_t count) {
    uint16_t* p = malloc(count * sizeof *p);
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

static void
fill_pattern_u16(uint16_t* p, size_t count) {
    for (size_t i = 0; i < count; i++) {
        p[i] = (uint16_t)pattern(i);
    }
}

/* A buffer of guard elements with room for `count` at element `offset` past
 * GUARD of them; returns where the run starts. */
static uint16_t*
guarded_u16(uint16_t* block, size_t count, size_t offset) {
    const size_t total = (2 * GUARD) + SIMD_LANES_U16 + count;
    for (size_t i = 0; i < total; i++) {
        block[i] = GUARD_U16;
    }
    return block + GUARD + offset;
}

static size_t
guarded_count(size_t count) {
    return (2 * GUARD) + SIMD_LANES_U16 + count;
}

static void
check_fill_at(size_t count, size_t offset, uint16_t value) {
    uint16_t* want_block = alloc_u16(guarded_count(count));
    uint16_t* got_block = alloc_u16(guarded_count(count));
    simd_fill_u16_ref(guarded_u16(want_block, count, offset), value, count);
    simd_fill_u16(guarded_u16(got_block, count, offset), value, count);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(want_block, got_block, guarded_count(count));
    free(got_block);
    free(want_block);
}

static void
test_simd_fill_u16_matches_reference(void) {
    static const uint16_t extremes[] = {0x0000U, 0xFFFFU, 0x8000U, 0x00FFU};
    for (size_t offset = 0; offset < SIMD_LANES_U16; offset++) {
        for (size_t count = 0; count <= 2 * SIMD_FILL_U16_MIN; count++) {
            check_fill_at(count, offset, (uint16_t)pattern(count));
        }
        for (size_t i = 0; i < sizeof long_lengths / sizeof *long_lengths; i++) {
            for (size_t e = 0; e < sizeof extremes / sizeof *extremes; e++) {
                check_fill_at(long_lengths[i], offset, extremes[e]);
            }
        }
    }
}

static void
check_repeat2_at(size_t count, size_t dst_offset, size_t src_offset) {
    uint16_t* src_block = alloc_u16(count + SIMD_LANES_U16);
    fill_pattern_u16(src_block, count + SIMD_LANES_U16);
    const uint16_t* src = src_block + src_offset;
    uint16_t* want_block = alloc_u16(guarded_count(2 * count));
    uint16_t* got_block = alloc_u16(guarded_count(2 * count));
    simd_repeat2_u16_ref(guarded_u16(want_block, 2 * count, dst_offset), src, count);
    simd_repeat2_u16(guarded_u16(got_block, 2 * count, dst_offset), src, count);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(want_block, got_block, guarded_count(2 * count));
    free(got_block);
    free(want_block);
    free(src_block);
}

static void
test_simd_repeat2_u16_matches_reference(void) {
    for (size_t dst_offset = 0; dst_offset < SIMD_LANES_U16; dst_offset++) {
        for (size_t src_offset = 0; src_offset < SIMD_LANES_U16; src_offset++) {
            for (size_t count = 0; count <= 2 * SIMD_REPEAT2_U16_MIN; count++) {
                check_repeat2_at(count, dst_offset, src_offset);
            }
            for (size_t i = 0; i < sizeof long_lengths / sizeof *long_lengths; i++) {
                check_repeat2_at(long_lengths[i], dst_offset, src_offset);
            }
        }
    }
}

static void
check_copy_at(size_t bytes, size_t dst_offset, size_t src_offset) {
    uint8_t* src_block = malloc(bytes + SIMD_ALIGN);
    TEST_ASSERT_NOT_NULL(src_block);
    for (size_t i = 0; i < bytes + SIMD_ALIGN; i++) {
        src_block[i] = (uint8_t)pattern(i);
    }
    const size_t total = (2 * GUARD) + SIMD_ALIGN + bytes;
    uint8_t* want = malloc(total);
    uint8_t* got = malloc(total);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_NOT_NULL(got);
    memset(want, GUARD_U8, total);
    memset(got, GUARD_U8, total);
    simd_copy_ref(want + GUARD + dst_offset, src_block + src_offset, bytes);
    simd_copy(got + GUARD + dst_offset, src_block + src_offset, bytes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, total);
    free(got);
    free(want);
    free(src_block);
}

static void
test_simd_copy_matches_reference(void) {
    for (size_t dst_offset = 0; dst_offset < SIMD_ALIGN; dst_offset++) {
        for (size_t src_offset = 0; src_offset < SIMD_ALIGN; src_offset++) {
            for (size_t i = 0; i < sizeof copy_lengths / sizeof *copy_lengths; i++) {
                check_copy_at(copy_lengths[i], dst_offset, src_offset);
            }
        }
    }
    check_copy_at(0, 0, 0);
    check_copy_at(1, 1, 3);
}

void
run_simd_suite(void) {
    RUN_TEST(test_simd_fill_u16_matches_reference);
    RUN_TEST(test_simd_repeat2_u16_matches_reference);
    RUN_TEST(test_simd_copy_matches_reference);
}

SUITE_REGISTER(run_simd_suite);
