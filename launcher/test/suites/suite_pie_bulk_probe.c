/*
 * Device-only probe: does a PIE 128-bit store or copy beat the C library on
 * the long contiguous passes? memset vs PIE fill and memcpy vs PIE copy, into
 * PSRAM and into internal RAM, at the r3d depth size and the panel strip
 * size. Every rep ends with a cache write-back, so a store left dirty in the
 * cache is paid inside the rep that made it.
 */
#include "suites.h"

#ifdef DEVICE_BUILD

#include "perf_stats.h"

#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "core/memory.h"
#include "core/timing.h"
#include "gfx/gfx.h"

static const char* TAG = "pie_bulk";

#define PROBE_REPS 41

static void
pie_fill_zero(void* destination, size_t blocks) {
    __asm__ volatile("ee.zero.q q0\n"
                     "loopgtz %[blocks], 0f\n"
                     "ee.vst.128.ip q0, %[destination], 16\n"
                     "0:\n"
                     : [destination] "+r"(destination)
                     : [blocks] "r"(blocks)
                     : "memory");
}

static void
pie_fill_zero_x4(void* destination, size_t blocks) {
    __asm__ volatile("ee.zero.q q0\n"
                     "loopgtz %[blocks], 0f\n"
                     "ee.vst.128.ip q0, %[destination], 16\n"
                     "ee.vst.128.ip q0, %[destination], 16\n"
                     "ee.vst.128.ip q0, %[destination], 16\n"
                     "ee.vst.128.ip q0, %[destination], 16\n"
                     "0:\n"
                     : [destination] "+r"(destination)
                     : [blocks] "r"(blocks / 4)
                     : "memory");
}

static void
pie_copy(void* destination, const void* source, size_t blocks) {
    __asm__ volatile("loopgtz %[blocks], 0f\n"
                     "ee.vld.128.ip q0, %[source], 16\n"
                     "ee.vst.128.ip q0, %[destination], 16\n"
                     "0:\n"
                     : [destination] "+r"(destination), [source] "+r"(source)
                     : [blocks] "r"(blocks)
                     : "memory");
}

static void
pie_copy_x2(void* destination, const void* source, size_t blocks) {
    __asm__ volatile("loopgtz %[blocks], 0f\n"
                     "ee.vld.128.ip q0, %[source], 16\n"
                     "ee.vld.128.ip q1, %[source], 16\n"
                     "ee.vst.128.ip q0, %[destination], 16\n"
                     "ee.vst.128.ip q1, %[destination], 16\n"
                     "0:\n"
                     : [destination] "+r"(destination), [source] "+r"(source)
                     : [blocks] "r"(blocks / 2)
                     : "memory");
}

static void
write_back(void* p, size_t bytes, bool psram) {
    if (psram) {
        TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync(p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
    }
}

typedef enum { KIND_MEMSET, KIND_PIE_FILL, KIND_PIE_FILL_X4, KIND_MEMCPY, KIND_PIE_COPY, KIND_PIE_COPY_X2 } kind_t;

static const char* const kind_names[] = {"memset", "pie_fill", "pie_fill_x4", "memcpy", "pie_copy", "pie_copy_x2"};

static void
run_one(kind_t kind, void* destination, const void* source, size_t bytes, bool destination_psram) {
    switch (kind) {
        case KIND_MEMSET: memset(destination, 0, bytes); break;
        case KIND_PIE_FILL: pie_fill_zero(destination, bytes / 16); break;
        case KIND_PIE_FILL_X4: pie_fill_zero_x4(destination, bytes / 16); break;
        case KIND_MEMCPY: memcpy(destination, source, bytes); break;
        case KIND_PIE_COPY: pie_copy(destination, source, bytes / 16); break;
        case KIND_PIE_COPY_X2: pie_copy_x2(destination, source, bytes / 16); break;
    }
    write_back(destination, bytes, destination_psram);
}

static void
measure_from(const char* label, kind_t kind, void* destination, const void* source, size_t bytes,
             bool destination_psram, bool cold_source) {
    int32_t us[PROBE_REPS];
    for (int i = 0; i < PROBE_REPS; i++) {
        if (cold_source) {
            TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync((void*)source, bytes,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
        }
        const int64_t began = timing_now_us();
        run_one(kind, destination, source, bytes, destination_psram);
        us[i] = (int32_t)(timing_now_us() - began);
    }
    const perf_stats_t s = perf_stats_compute(us, PROBE_REPS);
    ESP_LOGI(TAG, "PIE_PROBE %s %s bytes=%u med=%lld min=%lld max=%lld us MB/s=%lld", label, kind_names[kind],
             (unsigned)bytes, (long long)s.med, (long long)s.min, (long long)s.max,
             s.med > 0 ? (long long)bytes / s.med : 0LL);
}

static void
measure(const char* label, kind_t kind, void* destination, const void* source, size_t bytes, bool destination_psram) {
    measure_from(label, kind, destination, source, bytes, destination_psram, false);
}

static void
test_pie_bulk_probe(void) {
    const size_t depth_bytes = (size_t)(GFX_WIDTH / 2) * (GFX_HEIGHT / 2) * sizeof(uint16_t);
    const size_t frame_bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(uint16_t);
    const size_t strip_bytes = (size_t)GFX_WIDTH * 16 * sizeof(uint16_t);
    const size_t internal_bytes = 16 * 1024;

    ESP_LOGI(TAG, "free psram %u internal %u", (unsigned)memory_free_bytes(MEMORY_PSRAM),
             (unsigned)memory_free_bytes(MEMORY_INTERNAL));
    uint8_t* psram = heap_caps_aligned_alloc(64, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t* psram_source = heap_caps_aligned_alloc(64, depth_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t* internal = heap_caps_aligned_alloc(16, internal_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t* internal_source = heap_caps_aligned_alloc(16, internal_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(psram);
    TEST_ASSERT_NOT_NULL(psram_source);
    TEST_ASSERT_NOT_NULL(internal);
    TEST_ASSERT_NOT_NULL(internal_source);
    TEST_ASSERT_EQUAL(0, (uintptr_t)psram & 15);
    TEST_ASSERT_EQUAL(0, (uintptr_t)psram_source & 15);
    for (size_t i = 0; i < depth_bytes; i++) {
        psram_source[i] = (uint8_t)(i * 7);
    }

    for (int k = KIND_MEMSET; k <= KIND_PIE_FILL_X4; k++) {
        measure("depth_psram", (kind_t)k, psram, NULL, depth_bytes, true);
        measure("frame_psram", (kind_t)k, psram, NULL, frame_bytes, true);
        measure("internal", (kind_t)k, internal, NULL, internal_bytes, false);
    }
    for (int k = KIND_MEMCPY; k <= KIND_PIE_COPY_X2; k++) {
        measure("strip_psram_to_internal", (kind_t)k, internal, psram_source, strip_bytes, false);
        measure_from("strip_cold_psram_to_internal", (kind_t)k, internal, psram_source, strip_bytes, false, true);
        measure("depth_psram_to_psram", (kind_t)k, psram, psram_source, depth_bytes, true);
        measure("internal_to_internal", (kind_t)k, internal, internal_source, internal_bytes, false);
    }

    /* The kernels must agree byte for byte with the library. */
    memset(internal, 0xA5, internal_bytes);
    pie_fill_zero(internal, internal_bytes / 16);
    for (size_t i = 0; i < internal_bytes; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, internal[i]);
    }
    pie_copy_x2(internal, psram_source, internal_bytes / 16);
    TEST_ASSERT_EQUAL_MEMORY(psram_source, internal, internal_bytes);

    heap_caps_free(internal_source);
    heap_caps_free(internal);
    heap_caps_free(psram_source);
    heap_caps_free(psram);
}

static void
run_pie_bulk_probe(void) {
    RUN_TEST(test_pie_bulk_probe);
}

#else /* !DEVICE_BUILD */

static void
run_pie_bulk_probe(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_pie_bulk_probe);
