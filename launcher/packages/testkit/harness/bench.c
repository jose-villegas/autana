/* bench: target clocks, internal allocations and instruction counters for test workloads. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "testkit/bench.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#define BENCH_BUILD_HASH_CHARS        12
#define BENCH_ALLOCATION_MESSAGE_SIZE 160
#include "esp_app_desc.h"
#include "esp_cpu.h"
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
extern char _rodata_reserved_start[];
extern char _rodata_reserved_end[];
extern char _instruction_reserved_start[];
extern char _instruction_reserved_end[];

/* Cold calls refill instructions and flash rodata; PSRAM stays cached and inputs stay in internal RAM.
 * Instructions and flash rodata are read-only, so dropping them loses nothing. Writing back then
 * invalidating PSRAM would drop whatever the other core writes between those two steps. */
static inline void
bench_cache_rodata_cold(void) {
    const uintptr_t data_line = CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE;
    const uintptr_t data_start = (uintptr_t)_rodata_reserved_start & ~(data_line - 1);
    const uintptr_t data_end = ((uintptr_t)_rodata_reserved_end + data_line - 1) & ~(data_line - 1);
    TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync((void*)data_start, data_end - data_start,
                                              ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_DIR_M2C));
}

void
bench_cache_code_cold(void) {
    const uintptr_t line = CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE;
    const uintptr_t start = (uintptr_t)_instruction_reserved_start & ~(line - 1);
    const uintptr_t end = ((uintptr_t)_instruction_reserved_end + line - 1) & ~(line - 1);
    TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync((void*)start, end - start,
                                              ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_DIR_M2C));
}

bool
bench_cache_cold(void) {
    bench_cache_rodata_cold();
    bench_cache_code_cold();
    return true;
}

#else
void
bench_cache_code_cold(void) {}

bool
bench_cache_cold(void) {
    return false;
}
#endif
const char*
bench_unit(void) {
    return "cycles";
}

int64_t
bench_now_us(void) {
    return esp_timer_get_time();
}

void
bench_yield(void) {
    vTaskDelay(1);
}

void*
bench_alloc_internal(size_t size, const char* what) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    char message[BENCH_ALLOCATION_MESSAGE_SIZE];
    snprintf(message, sizeof message, "%s needs %zu B contiguous internal RAM; largest free block %zu B", what, size,
             largest);
    TEST_ASSERT_TRUE_MESSAGE(largest >= size, message);
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
    TEST_ASSERT_NOT_NULL_MESSAGE(ptr, message);
    return ptr;
}

__attribute__((weak)) const char*
bench_build_id(void) {
    static char id[BENCH_BUILD_HASH_CHARS + 1];
    esp_app_get_elf_sha256(id, sizeof id);
    return id;
}

bool
bench_insn_begin(void) {
    TEST_ASSERT_EQUAL_INT(0, esp_cpu_get_core_id());
    TEST_ASSERT_TRUE(bench_counters_idle());
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    xtensa_perfmon_stop();
    xtensa_perfmon_init(0, XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL, 0, -1);
    xtensa_perfmon_reset(0);
    xtensa_perfmon_start();
    return true;
#else
    return false;
#endif
}

bool
bench_insn_end(uint32_t* insn) {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
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
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    const char* arch = "Xtensa";
#elif CONFIG_IDF_TARGET_ARCH_RISCV
    const char* arch = "RISC-V";
#else
    const char* arch = "unknown";
#endif
    const char* execution = bench_is_qemu() ? "qemu" : "hardware";
    printf("BUILD_ID=%s\n", bench_build_id());
#ifdef XCHAL_CORE_ID
    printf("%s target=%s arch=%s core=%s execution=%s\n", prefix, CONFIG_IDF_TARGET, arch, XCHAL_CORE_ID, execution);
#else
    printf("%s target=%s arch=%s execution=%s\n", prefix, CONFIG_IDF_TARGET, arch, execution);
#endif
}
#else
#define BENCH_NS_PER_US     1000
#define BENCH_NS_PER_SECOND 1000000000
#ifdef _WIN32
#include <windows.h>

static int64_t
monotonic_ns(void) {
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return count.QuadPart / frequency.QuadPart * BENCH_NS_PER_SECOND
           + count.QuadPart % frequency.QuadPart * BENCH_NS_PER_SECOND / frequency.QuadPart;
}
#else
#include <time.h>

static int64_t
monotonic_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * BENCH_NS_PER_SECOND + time.tv_nsec;
}
#endif
uint32_t
bench_cycles(void) {
    return (uint32_t)monotonic_ns();
}

const char*
bench_unit(void) {
    return "ns";
}

void
bench_cache_code_cold(void) {}

bool
bench_cache_cold(void) {
    return false;
}

bool
bench_insn_begin(void) {
    return false;
}

bool
bench_insn_end(uint32_t* insn) {
    (void)insn;
    return false;
}

int64_t
bench_now_us(void) {
    return monotonic_ns() / BENCH_NS_PER_US;
}

void
bench_yield(void) {}

const char*
bench_build_id(void) {
    return "host";
}

void*
bench_alloc_internal(size_t size, const char* what) {
    void* ptr = malloc(size);
    TEST_ASSERT_NOT_NULL_MESSAGE(ptr, what);
    return ptr;
}

void
bench_target_report(const char* prefix) {
    printf("%s target=host execution=host\n", prefix);
}
#endif
__attribute__((weak)) bool
bench_counters_idle(void) {
    return true;
}

void
bench_free(void* ptr) {
    free(ptr);
}

void
bench_icount_report_row(const char* prefix, const char* name, int64_t us, int reps, int batches) {
    printf("%s %s us=%" PRId64 " reps=%d\n", prefix, name, us, reps);
    TEST_ASSERT_TRUE(bench_icount_long_enough(us, batches));
}

void
bench_icount_report(const char* name, int64_t us, int reps, int batches) {
    bench_icount_report_row(BENCH_ICOUNT_PREFIX, name, us, reps, batches);
}

bool
bench_is_qemu(void) {
#if defined(ESP_PLATFORM) && CONFIG_TESTKIT_QEMU
    return true;
#else
    return false;
#endif
}
