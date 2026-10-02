#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdint.h>

#include "unity.h"

#include "esp_cpu.h"
#include "esp_log.h"

#include "gfx/gfx.h"
#include "render/upscale.h"
#include "util/job.h"
#include "util/memory.h"
#include "util/timing.h"

#define SAMPLES 100

typedef struct {
    const char* label;
    int numerator, denominator;
} scale_case_t;

typedef struct {
    const upscale_t* scale;
    const uint16_t* source;
    uint16_t* destination;
    int first_row, row_count;
} upscale_job_t;

static const scale_case_t cases[] = {
    {"1", 1, 1}, {"1.25", 5, 4}, {"1.33", 4, 3}, {"1.5", 3, 2}, {"2", 2, 1}, {"2.5", 5, 2}, {"4", 4, 1},
};

static void
upscale_job(void* context) {
    const upscale_job_t* job = context;
    upscale_rows(job->scale, job->source, NULL, 0, job->destination, job->first_row, job->row_count);
}

static int
scaled_size(int full, const scale_case_t* scale) {
    return (full * scale->denominator + scale->numerator / 2) / scale->numerator;
}

static void
report_case(const scale_case_t* test_case) {
    const int source_width = scaled_size(GFX_WIDTH, test_case);
    const int source_height = scaled_size(GFX_HEIGHT, test_case);
    const size_t source_pixels = (size_t)source_width * source_height;
    const size_t destination_pixels = (size_t)GFX_WIDTH * GFX_HEIGHT;
    uint16_t* source = memory_alloc(source_pixels * sizeof(*source), MEMORY_PSRAM);
    uint16_t* destination = memory_alloc(destination_pixels * sizeof(*destination), MEMORY_PSRAM);
    uint16_t* columns = memory_alloc(GFX_WIDTH * sizeof(*columns), MEMORY_INTERNAL);
    uint16_t* rows = memory_alloc(GFX_HEIGHT * sizeof(*rows), MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(columns);
    TEST_ASSERT_NOT_NULL(rows);
    for (size_t i = 0; i < source_pixels; i++) {
        source[i] = (uint16_t)i;
    }
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, source_width, source_height, GFX_WIDTH, GFX_HEIGHT, columns, rows));
    upscale_rows(&scale, source, NULL, 0, destination, 0, GFX_HEIGHT);

    int64_t start_us = timing_now_us();
    uint32_t start_cycles = esp_cpu_get_cycle_count();
    for (int i = 0; i < SAMPLES; i++) {
        upscale_rows(&scale, source, NULL, 0, destination, 0, GFX_HEIGHT);
    }
    const uint32_t one_cycles = esp_cpu_get_cycle_count() - start_cycles;
    const int64_t one_us = timing_now_us() - start_us;
    ESP_LOGI("upscale_perf", "scale %s source %dx%d one core: %lld us/frame %.2f cycles/destination pixel",
             test_case->label, source_width, source_height, (long long)(one_us / SAMPLES),
             (double)one_cycles / SAMPLES / destination_pixels);

    const int middle = GFX_HEIGHT / 2;
    const upscale_job_t top = {&scale, source, destination, 0, middle};
    start_us = timing_now_us();
    start_cycles = esp_cpu_get_cycle_count();
    for (int i = 0; i < SAMPLES; i++) {
        const upscale_job_t bottom = {&scale, source, destination, middle, GFX_HEIGHT - middle};
        TEST_ASSERT_TRUE(job_run_core1(upscale_job, &top, sizeof top));
        upscale_job(&bottom);
        TEST_ASSERT_TRUE(job_wait(1000));
    }
    const uint32_t two_cycles = esp_cpu_get_cycle_count() - start_cycles;
    const int64_t two_us = timing_now_us() - start_us;
    ESP_LOGI("upscale_perf", "scale %s source %dx%d two cores: %lld us/frame %.2f cycles/destination pixel",
             test_case->label, source_width, source_height, (long long)(two_us / SAMPLES),
             (double)two_cycles / SAMPLES / destination_pixels);
    memory_free(rows);
    memory_free(columns);
    memory_free(destination);
    memory_free(source);
}

static void
test_upscale_cost_to_the_panel(void) {
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        report_case(&cases[i]);
    }
}

void
run_upscale_perf_suite(void) {
    RUN_TEST(test_upscale_cost_to_the_panel);
}

#else

void
run_upscale_perf_suite(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_upscale_perf_suite);
