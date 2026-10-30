/*
 * simd board timing: cycles of each call against its C reference over
 * internal RAM, at lengths around each minimum and at screen-row lengths.
 * Rows print as "SIMD_BENCH <call> n=<elements> ref=<cycles> simd=<cycles>",
 * the median of BENCH_CYCLE_WARM_REPS warm runs. Board only: QEMU has no PIE.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "testkit/bench.h"
#include "testkit/suite.h"
#include "unity.h"

#include "simd/copy.h"
#include "simd/fill.h"
#include "simd/repeat.h"
#include "simd/simd.h"

/* Lengths timed, in elements: from below every minimum to a screen row and past it. */
static const size_t lengths[] = {16, 32, 48, 64, 96, 128, 184, 368, 1024};

#define LONGEST    1024
#define FILL_VALUE 0x1234U

typedef void (*call_fn)(void* dst, const void* src, size_t count);

static void
fill_ref(void* dst, const void* src, size_t count) {
    (void)src;
    simd_fill_u16_ref(dst, FILL_VALUE, count);
}

static void
fill_simd(void* dst, const void* src, size_t count) {
    (void)src;
    simd_fill_u16(dst, FILL_VALUE, count);
}

static void
repeat2_ref(void* dst, const void* src, size_t count) {
    simd_repeat2_u16_ref(dst, src, count);
}

static void
repeat2_simd(void* dst, const void* src, size_t count) {
    simd_repeat2_u16(dst, src, count);
}

/* Copy lengths count 16-bit elements, so every row compares the same data size. */
static void
copy_ref(void* dst, const void* src, size_t count) {
    simd_copy_ref(dst, src, count * sizeof(uint16_t));
}

static void
copy_simd(void* dst, const void* src, size_t count) {
    simd_copy(dst, src, count * sizeof(uint16_t));
}

static int64_t
median_cycles(call_fn fn, void* dst, const void* src, size_t count) {
    int32_t cycles[BENCH_CYCLE_WARM_REPS];
    fn(dst, src, count);
    for (int r = 0; r < BENCH_CYCLE_WARM_REPS; r++) {
        const uint32_t began = bench_cycles();
        fn(dst, src, count);
        cycles[r] = (int32_t)(bench_cycles() - began);
    }
    return bench_stats_compute(cycles, BENCH_CYCLE_WARM_REPS).med;
}

static void
bench_call(const char* name, call_fn ref, call_fn simd, void* dst, const void* src) {
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; i++) {
        const int64_t ref_cycles = median_cycles(ref, dst, src, lengths[i]);
        const int64_t simd_cycles = median_cycles(simd, dst, src, lengths[i]);
        printf("SIMD_BENCH %s n=%u ref=%lld simd=%lld\n", name, (unsigned)lengths[i], (long long)ref_cycles,
               (long long)simd_cycles);
    }
}

static void
test_simd_board_rows(void) {
    if (bench_is_qemu()) {
        TEST_IGNORE_MESSAGE("PIE runs on the board only");
    }
    /* Twice the longest run for repeat's output; one element past alignment
     * so the unaligned source rows run too. */
    uint16_t* dst = bench_alloc_internal((2 * LONGEST + SIMD_LANES_U16) * sizeof(uint16_t), "simd bench dst");
    uint16_t* src = bench_alloc_internal((LONGEST + SIMD_LANES_U16) * sizeof(uint16_t), "simd bench src");
    /* Any data: the copies cost the same whatever they carry. */
    for (size_t i = 0; i < LONGEST + SIMD_LANES_U16; i++) {
        src[i] = (uint16_t)i;
    }
    uint16_t* aligned_dst = dst + (simd_head_bytes(dst) / sizeof *dst);
    uint16_t* aligned_src = src + (simd_head_bytes(src) / sizeof *src);
    bench_call("fill_u16", fill_ref, fill_simd, aligned_dst, NULL);
    bench_call("repeat2_u16", repeat2_ref, repeat2_simd, aligned_dst, aligned_src);
    bench_call("repeat2_u16_unaligned_src", repeat2_ref, repeat2_simd, aligned_dst, aligned_src + 1);
    bench_call("copy", copy_ref, copy_simd, aligned_dst, aligned_src);
    bench_call("copy_unaligned_src", copy_ref, copy_simd, aligned_dst, aligned_src + 1);
    free(src);
    free(dst);
}

void
run_simd_perf_suite(void) {
    RUN_TEST(test_simd_board_rows);
}

SUITE_REGISTER_ON_REQUEST(run_simd_perf_suite);
