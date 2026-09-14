/*
 * sand_core1 - a task pinned to core 1 for the handful of per-step
 * bookkeeping scans that are provably order-independent, so they can run
 * genuinely alongside the caller's own core instead of merely being called
 * from it.
 *
 * Everything that decides how a grain moves draws from sand_t's own
 * sequential PRNG a data-dependent number of times, so it cannot be split
 * this way - see sand_two_core_step_enabled() (sand.h). What lands here is
 * narrower: a scan whose per-block outcome depends only on state nothing
 * else in the same pass writes, and which writes only its own block. See
 * finalize_settling() (sand.c) and mark_liquid_neighbourhoods()
 * (sand_liquid.c).
 */
#include "sand_priv.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "sand_core1";

#define CORE1_STACK_BYTES 2048

/* BELOW gfx's present task (priority 5, gfx.c) on purpose: sand_step() can
 * run while a previous frame is still presenting (main.c's step_app()), so
 * this task shares core 1 with a task that must never lose the CPU to it.
 * A lower priority means FreeRTOS preempts this one the instant present
 * has work again, so present's own timing is unaffected - this task only
 * fills the gaps present's own waits on the strip-sent semaphore leave
 * behind, never competes with it. */
#define CORE1_PRIORITY    3
#define CORE1_CORE        1

static TaskHandle_t core1_task_handle;
static StaticTask_t core1_task_tcb;
static SemaphoreHandle_t core1_done_sem;
static bool core1_ready;

/* Set by sand_core1_run() before waking the task, read back by
 * sand_core1_join(): NULL means the last dispatch ran inline on the
 * caller's own core (two-core stepping off, or bring-up failed), so there
 * is nothing to wait for. */
static void (*volatile core1_fn)(void*);
static void* volatile core1_ctx;

static void
core1_task_fn(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        core1_fn(core1_ctx);
        xSemaphoreGive(core1_done_sem);
    }
}

/* Lazy: sand may never run on a given boot, and a task nobody uses should
 * not cost a permanent stack. Idempotent and tolerant of failure - a board
 * that cannot spare the stack still steps, just without core 1's help. */
static bool
core1_bring_up(void) {
    if (core1_ready) {
        return true;
    }

    core1_done_sem = xSemaphoreCreateBinary();
    if (core1_done_sem == NULL) {
        ESP_LOGW(TAG, "could not create the done semaphore - staying serial");
        return false;
    }

    StackType_t* const stack = heap_caps_malloc(CORE1_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stack == NULL) {
        ESP_LOGW(TAG, "could not allocate the %u byte stack - staying serial", (unsigned)CORE1_STACK_BYTES);
        return false;
    }

    core1_task_handle =
        xTaskCreateStaticPinnedToCore(core1_task_fn, "sand_core1", CORE1_STACK_BYTES / sizeof(StackType_t), NULL,
                                      CORE1_PRIORITY, stack, &core1_task_tcb, CORE1_CORE);
    core1_ready = (core1_task_handle != NULL);
    if (!core1_ready) {
        ESP_LOGW(TAG, "could not create the task - staying serial");
    }
    return core1_ready;
}

void
sand_core1_run(void (*fn)(void*), void* ctx) {
    if (!sand_two_core_step_enabled() || !core1_bring_up()) {
        fn(ctx);
        core1_fn = NULL;
        return;
    }
    core1_fn = fn;
    core1_ctx = ctx;
    xTaskNotifyGive(core1_task_handle);
}

void
sand_core1_join(void) {
    if (core1_fn == NULL) {
        return;
    }
    xSemaphoreTake(core1_done_sem, portMAX_DELAY);
    core1_fn = NULL;
}

#else /* !ESP_PLATFORM */

/* A host build has no second core to hand work to and no FreeRTOS to hand
 * it with, so the split path still exists here - inline, in whatever order
 * the caller issues it - purely so a host test can exercise the same
 * range split real hardware runs and check it against the plain serial
 * call. */
void
sand_core1_run(void (*fn)(void*), void* ctx) {
    fn(ctx);
}

void
sand_core1_join(void) {}

#endif
