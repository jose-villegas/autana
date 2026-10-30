/* bench: target clocks, internal allocations and instruction counters for test workloads. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "testkit/bench.h"
#include "math/stats/statsi.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include <xtensa/config/core-isa.h>
#include "xtensa/xt_perf_consts.h"
#include "xtensa_perfmon_access.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp_cache.h"
extern char _rodata_reserved_start[], _rodata_reserved_end[];
extern char _instruction_reserved_start[], _instruction_reserved_end[];
#endif
#elif defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define BENCH_BUILD_HASH_CHARS 12
#define BENCH_NS_PER_US        1000
#define BENCH_NS_PER_SECOND    1000000000

#ifndef ESP_PLATFORM
static int64_t
monotonic_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return count.QuadPart / frequency.QuadPart * BENCH_NS_PER_SECOND
           + count.QuadPart % frequency.QuadPart * BENCH_NS_PER_SECOND / frequency.QuadPart;
#else
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * BENCH_NS_PER_SECOND + time.tv_nsec;
#endif
}

uint32_t
bench_cycles(void) {
    return (uint32_t)monotonic_ns();
}
#endif

void
bench_cache_code_cold(void) {
#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32S3
    const uintptr_t line = CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE;
    const uintptr_t start = (uintptr_t)_instruction_reserved_start & ~(line - 1);
    const uintptr_t end = ((uintptr_t)_instruction_reserved_end + line - 1) & ~(line - 1);
    TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync((void*)start, end - start,
                                              ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_DIR_M2C));
#endif
}

bool
bench_cache_cold(void) {
#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32S3
    /* Flash is read-only; invalidating PSRAM could discard the other core's writes. */
    const uintptr_t line = CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE;
    const uintptr_t start = (uintptr_t)_rodata_reserved_start & ~(line - 1);
    const uintptr_t end = ((uintptr_t)_rodata_reserved_end + line - 1) & ~(line - 1);
    TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync((void*)start, end - start,
                                              ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_DIR_M2C));
    bench_cache_code_cold();
    return true;
#else
    return false;
#endif
}

int64_t
bench_now_us(void) {
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return monotonic_ns() / BENCH_NS_PER_US;
#endif
}

void
bench_yield(void) {
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
}

void*
bench_alloc_internal(size_t size, const char* what) {
#ifdef ESP_PLATFORM
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
#else
    void* ptr = malloc(size);
#endif
    TEST_ASSERT_NOT_NULL_MESSAGE(ptr, what);
    return ptr;
}

/* A runner overrides these defaults by linking its own definitions. */
__attribute__((weak)) const char*
bench_build_id(void) {
#ifdef ESP_PLATFORM
    static char id[BENCH_BUILD_HASH_CHARS + 1];
    esp_app_get_elf_sha256(id, sizeof id);
    return id;
#else
    return "host";
#endif
}

__attribute__((weak)) bool
bench_counters_idle(void) {
    return true;
}

__attribute__((weak)) bool
bench_insn_begin(void) {
#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ARCH_XTENSA
    TEST_ASSERT_EQUAL_INT(0, esp_cpu_get_core_id());
    TEST_ASSERT_TRUE(bench_counters_idle());
    xtensa_perfmon_stop();
    xtensa_perfmon_init(0, XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL, 0, -1);
    xtensa_perfmon_reset(0);
    xtensa_perfmon_start();
    return true;
#else
    return false;
#endif
}

__attribute__((weak)) bool
bench_insn_end(uint32_t* insn) {
#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ARCH_XTENSA
    xtensa_perfmon_stop();
    *insn = xtensa_perfmon_value(0);
    return xtensa_perfmon_overflow(0) == ESP_OK;
#else
    (void)insn;
    return false;
#endif
}

void
bench_target_report(const char* prefix) {
#ifdef ESP_PLATFORM
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    const char* arch = "Xtensa";
#else
    const char* arch = "RISC-V";
#endif
    printf("BUILD_ID=%s\n", bench_build_id());
    printf("%s target=%s arch=%s", prefix, CONFIG_IDF_TARGET, arch);
#ifdef XCHAL_CORE_ID
    printf(" core=%s", XCHAL_CORE_ID);
#endif
    printf(" execution=%s\n", bench_is_qemu() ? "qemu" : "hardware");
#else
    printf("%s target=host execution=host unit=ns\n", prefix);
#endif
}

bool
bench_is_qemu(void) {
#if defined(ESP_PLATFORM) && CONFIG_TESTKIT_QEMU
    return true;
#else
    return false;
#endif
}

bench_stats_t
bench_stats_compute(int32_t* values, int n) {
    bench_stats_t s = {.min = INT64_MAX, .max = 0, .avg = 0, .med = 0, .p95 = 0};
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        if (values[i] < s.min) {
            s.min = values[i];
        }
        if (values[i] > s.max) {
            s.max = values[i];
        }
        sum += values[i];
    }
    s.avg = sum / n;
    s.med = statsi_median(values, (size_t)n);
    s.p95 = statsi_percentile(values, (size_t)n, 95);
    return s;
}
