/* core-1 job dispatch shared by every engine client. */
#include "util/job.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "build_variant.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "job";

#define JOB_CORE1_STACK_BYTES 3072
/* Gfx presents at priority 5; rendering must preempt engine work. */
#define JOB_CORE1_PRIORITY    3
#define JOB_CORE1             1

static TaskHandle_t job_task_handle;
static StaticTask_t job_task_tcb;
static SemaphoreHandle_t job_done_sem;
static StackType_t* job_stack;
static bool job_ready;
static bool job_unavailable;
static bool job_active;
static bool job_waits_for_core;
static job_fn_t volatile job_fn;
static uint8_t job_ctx[JOB_CTX_MAX] __attribute__((aligned(8)));

#if CONFIG_LAUNCHER_DEVELOPMENT
static bool job_timeout_logged;
#endif

static void
job_call_inline(job_fn_t fn, const void* ctx, size_t ctx_size) {
    uint8_t copy[JOB_CTX_MAX] __attribute__((aligned(8)));

    if (ctx_size != 0) {
        memcpy(copy, ctx, ctx_size);
    }
    fn(copy);
}

static void
job_task_fn(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        job_fn(job_ctx);
        xSemaphoreGive(job_done_sem);
    }
}

static bool
job_bring_up(void) {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    return false;
#else
    if (job_unavailable) {
        return false;
    }
    if (job_ready) {
        return true;
    }

    job_done_sem = xSemaphoreCreateBinary();
    if (job_done_sem == NULL) {
        job_unavailable = true;
        return false;
    }

    job_stack = heap_caps_malloc(JOB_CORE1_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (job_stack == NULL) {
        job_unavailable = true;
        return false;
    }

    job_task_handle =
        xTaskCreateStaticPinnedToCore(job_task_fn, "core1_job", JOB_CORE1_STACK_BYTES / sizeof(StackType_t), NULL,
                                      JOB_CORE1_PRIORITY, job_stack, &job_task_tcb, JOB_CORE1);
    job_ready = (job_task_handle != NULL);
    job_unavailable = !job_ready;
    return job_ready;
#endif
}

static bool
job_reap_finished(void) {
    if (!job_active) {
        return true;
    }
    if (xSemaphoreTake(job_done_sem, 0) != pdTRUE) {
        return false;
    }
    job_active = false;
    return true;
}

static TickType_t
job_timeout_ticks(unsigned timeout_ms) {
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);

    return timeout_ms != 0 && ticks == 0 ? 1 : ticks;
}

bool
job_run_core1(job_fn_t fn, const void* ctx, size_t ctx_size) {
    assert(fn != NULL);
    assert(ctx != NULL || ctx_size == 0);
    if (ctx_size > JOB_CTX_MAX) {
        return false;
    }

    if (!job_bring_up() || job_waits_for_core || !job_reap_finished()) {
        job_call_inline(fn, ctx, ctx_size);
        return true;
    }

    if (ctx_size != 0) {
        memcpy(job_ctx, ctx, ctx_size);
    }
    job_fn = fn;
    job_active = true;
    job_waits_for_core = true;
    xTaskNotifyGive(job_task_handle);
    return true;
}

bool
job_wait(unsigned timeout_ms) {
    if (!job_waits_for_core) {
        return true;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t start_us = esp_timer_get_time();
#endif
    if (xSemaphoreTake(job_done_sem, job_timeout_ticks(timeout_ms)) == pdTRUE) {
        job_active = false;
        job_waits_for_core = false;
        return true;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    if (!job_timeout_logged) {
        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        ESP_LOGE(TAG, "core-1 job %p timed out after %lld us", (void*)job_fn, (long long)elapsed_us);
        job_timeout_logged = true;
    }
#endif
    return false;
}

#else

bool
job_run_core1(job_fn_t fn, const void* ctx, size_t ctx_size) {
    assert(fn != NULL);
    assert(ctx != NULL || ctx_size == 0);
    if (ctx_size > JOB_CTX_MAX) {
        return false;
    }

    uint8_t copy[JOB_CTX_MAX] __attribute__((aligned(8)));
    if (ctx_size != 0) {
        memcpy(copy, ctx, ctx_size);
    }
    fn(copy);
    return true;
}

bool
job_wait(unsigned timeout_ms) {
    (void)timeout_ms;
    return true;
}

#endif
