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

#include <assert.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "sand_core1";

#define CORE1_STACK_BYTES     3072

/* BELOW gfx's present task (priority 5, gfx.c) on purpose: sand_step() can
 * run while a previous frame is still presenting (main.c's step_app()), so
 * this task shares core 1 with a task that must never lose the CPU to it.
 * A lower priority means FreeRTOS preempts this one the instant present
 * has work again, so present's own timing is unaffected - this task only
 * fills the gaps present's own waits on the strip-sent semaphore leave
 * behind, never competes with it. */
#define CORE1_PRIORITY        3
#define CORE1_CORE            1

/* No dispatch this file makes is ever more than a full-grid tile scan, and
 * the slowest of those measures in the tens of microseconds (see
 * Sand-Simulation.md's performance discipline table for a whole settled
 * screen). A hundred milliseconds is not a budget, it is the line between
 * "still running" and "never coming back" - see core1_join_or_disable()'s
 * own comment for what crossing it means. */
#define CORE1_JOIN_TIMEOUT_MS 100

static TaskHandle_t core1_task_handle;
static StaticTask_t core1_task_tcb;
static SemaphoreHandle_t core1_done_sem;
static bool core1_ready;

/* Latched true the first time a join times out. core1_bring_up() refuses
 * to hand out the task again after that - see core1_join_or_disable() -
 * so every later step pays only the one branch this adds, forever, the
 * same degraded mode an allocation failure already falls back to. */
static bool core1_disabled;

static void (*volatile core1_fn)(void*);

/* The dispatched context, COPIED here by sand_core1_run() rather than
 * merely pointed at: a join that times out cannot also un-arm a
 * straggler task still mid-callback, so a caller's stack-allocated
 * context would dangle the moment it returns. This buffer outlives every
 * caller, so a late finish reads stale-but-valid bytes, never freed
 * memory. */
static uint8_t core1_ctx_storage[SAND_CORE1_CTX_MAX] __attribute__((aligned(8)));

static void
core1_task_fn(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        core1_fn(core1_ctx_storage);
        xSemaphoreGive(core1_done_sem);
    }
}

/* Lazy: sand may never run on a given boot, and a task nobody uses should
 * not cost a permanent stack. Idempotent and tolerant of failure - a board
 * that cannot spare the stack still steps, just without core 1's help. */
static bool
core1_bring_up(void) {
    if (core1_disabled) {
        return false;
    }
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
sand_core1_run(void (*fn)(void*), const void* ctx, size_t ctx_size) {
    assert(ctx_size <= sizeof core1_ctx_storage);
    if (!sand_two_core_step_enabled() || !core1_bring_up()) {
        /* Runs synchronously on the caller's own stack, so the original
         * ctx is still valid for the whole call - no copy needed. */
        fn((void*)(uintptr_t)ctx);
        core1_fn = NULL;
        return;
    }
    memcpy(core1_ctx_storage, ctx, ctx_size);
    core1_fn = fn;
    xTaskNotifyGive(core1_task_handle);
}

/* A timeout means the dispatched work never came back - a wedged core 1,
 * a starved task, a priority inversion against whatever else is pinned
 * there. Nothing here can "cancel" the straggler, so the only sound
 * recovery is to stop ever notifying it again: core1_disabled latches
 * true for the rest of this boot and every later sand_core1_run() falls
 * back to running inline, the same fallback a bring-up failure already
 * takes. */
static void
core1_join_or_disable(void) {
    if (xSemaphoreTake(core1_done_sem, pdMS_TO_TICKS(CORE1_JOIN_TIMEOUT_MS)) == pdTRUE) {
        core1_fn = NULL;
        return;
    }

    core1_disabled = true;
#if CONFIG_LAUNCHER_DEVELOPMENT
    ESP_LOGE(TAG,
             "core 1 did not answer within %d ms - two-core stepping is "
             "OFF for the rest of this boot",
             CORE1_JOIN_TIMEOUT_MS);
#endif
    /* core1_fn and core1_ctx_storage are left as they are on purpose: a
     * straggler that finishes late still reads a valid, if stale, copy -
     * see that buffer's own comment above - and nothing here knows
     * whether it has read them yet. */
}

void
sand_core1_join(void) {
    if (core1_fn == NULL) {
        return;
    }
    core1_join_or_disable();
}

#else /* !ESP_PLATFORM */

/* A host build has no second core to hand work to and no FreeRTOS to hand
 * it with, so the split path still exists here - inline, in whatever order
 * the caller issues it - purely so a host test can exercise the same
 * range split real hardware runs and check it against the plain serial
 * call. */
void
sand_core1_run(void (*fn)(void*), const void* ctx, size_t ctx_size) {
    (void)ctx_size;
    fn((void*)(uintptr_t)ctx);
}

void
sand_core1_join(void) {}

#endif
