/* core-1 job dispatch shared by every engine client. */
#include "core/job.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "build/build_variant.h"
#include "core/memory.h"
#include "core/timing.h"
#include "math/scalar/mathi.h"

/* Moves halfway from the current share to the balanced one. */
#define JOB_SHARE_DAMPING_DIVISOR 2.0F

int
job_share_update(int core1_permille, int64_t core1_us, int64_t caller_us) {
    const int share = core1_permille;
    assert(share >= JOB_SHARE_MIN && share <= JOB_SHARE_MAX);
    if (core1_us <= 0 || caller_us <= 0) {
        return share;
    }
    const float rate_core1 = (float)core1_us / (float)share;
    const float rate_caller = (float)caller_us / (float)(JOB_SHARE_PERMILLE - share);
    const float balanced = (float)JOB_SHARE_PERMILLE * rate_caller / (rate_core1 + rate_caller);
    return mathi_clamp((int)(((float)share + balanced) / JOB_SHARE_DAMPING_DIVISOR), JOB_SHARE_MIN, JOB_SHARE_MAX);
}

bool
job_run_steered(int16_t* core1_offset, job_fn_t fn, void* first_half, const void* second_half, size_t ctx_size) {
    const int64_t began_us = timing_now_us();
    const bool dispatched = job_try_core1(fn, second_half, ctx_size);
    if (!dispatched) {
        fn((void*)second_half);
    }
    const int64_t caller_began_us = timing_now_us();
    fn(first_half);
    const int64_t caller_us = timing_now_us() - caller_began_us;
    const bool done = job_wait(JOB_SPLIT_WAIT_MS);
    assert(done); /* the next stage reads what core 1 wrote */
    (void)done;
    if (core1_offset != NULL) {
        const int64_t core1_us = dispatched ? job_core1_elapsed_us() : caller_began_us - began_us;
        *core1_offset =
            (int16_t)(job_share_update(job_share_permille(core1_offset), core1_us, caller_us) - JOB_SHARE_EVEN);
    }
    return dispatched;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define JOB_CORE1_STACK_BYTES 3072
/* Gfx presents at priority 5; rendering must preempt engine work. */
#define JOB_CORE1_PRIORITY    3
#define JOB_CORE1             1

static TaskHandle_t job_task_handle;
static void (*job_started_fn)(void* task);
static StaticTask_t job_task_tcb;
static SemaphoreHandle_t job_done_sem;
static StackType_t* job_stack;
static bool job_ready;
static bool job_unavailable;
static bool job_active;
static bool job_waits_for_core;
static job_fn_t volatile job_fn;
static int64_t job_dispatched_us;
static int64_t volatile job_finished_us;
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
        job_finished_us = timing_now_us();
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

    job_stack = memory_alloc(JOB_CORE1_STACK_BYTES, MEMORY_INTERNAL);
    if (job_stack == NULL) {
        job_unavailable = true;
        return false;
    }

    job_task_handle =
        xTaskCreateStaticPinnedToCore(job_task_fn, "core1_job", JOB_CORE1_STACK_BYTES / sizeof(StackType_t), NULL,
                                      JOB_CORE1_PRIORITY, job_stack, &job_task_tcb, JOB_CORE1);
    job_ready = (job_task_handle != NULL);
    job_unavailable = !job_ready;
    if (job_ready && job_started_fn != NULL) {
        job_started_fn(job_task_handle);
    }
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

static bool
job_dispatch(job_fn_t fn, const void* ctx, size_t ctx_size) {
    if (!job_bring_up() || job_waits_for_core || !job_reap_finished()) {
        return false;
    }

    if (ctx_size != 0) {
        memcpy(job_ctx, ctx, ctx_size);
    }
    job_fn = fn;
    job_active = true;
    job_waits_for_core = true;
    job_dispatched_us = timing_now_us();
    xTaskNotifyGive(job_task_handle);
    return true;
}

void
job_on_worker_started(void (*fn)(void* task)) {
    job_started_fn = fn;
    if (job_ready && fn != NULL) {
        fn(job_task_handle);
    }
}

bool
job_run_core1(job_fn_t fn, const void* ctx, size_t ctx_size) {
    assert(fn != NULL);
    assert(ctx != NULL || ctx_size == 0);
    if (ctx_size > JOB_CTX_MAX) {
        return false;
    }

    if (!job_dispatch(fn, ctx, ctx_size)) {
        job_call_inline(fn, ctx, ctx_size);
    }
    return true;
}

bool
job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size) {
    assert(fn != NULL);
    assert(ctx != NULL || ctx_size == 0);
    return ctx_size <= JOB_CTX_MAX && job_dispatch(fn, ctx, ctx_size);
}

int64_t
job_core1_elapsed_us(void) {
    return job_finished_us - job_dispatched_us;
}

bool
job_wait(unsigned timeout_ms) {
    if (!job_waits_for_core) {
        return true;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t start_us = timing_now_us();
#endif
    if (xSemaphoreTake(job_done_sem, job_timeout_ticks(timeout_ms)) == pdTRUE) {
        job_active = false;
        job_waits_for_core = false;
        return true;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    if (!job_timeout_logged) {
        const int64_t elapsed_us = timing_now_us() - start_us;
        ESP_LOGE("job", "core-1 job %p timed out after %lld us", (void*)job_fn, (long long)elapsed_us);
        job_timeout_logged = true;
    }
#endif
    return false;
}

#else

void
job_on_worker_started(void (*fn)(void* task)) {
    (void)fn;
}

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
job_try_core1(job_fn_t fn, const void* ctx, size_t ctx_size) {
    (void)fn;
    (void)ctx;
    (void)ctx_size;
    return false;
}

int64_t
job_core1_elapsed_us(void) {
    return 0;
}

bool
job_wait(unsigned timeout_ms) {
    (void)timeout_ms;
    return true;
}

#endif
