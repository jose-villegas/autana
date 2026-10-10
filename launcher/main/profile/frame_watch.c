#include "profile/frame_watch.h"

#if FRAME_WATCH_ENABLED

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/job.h"
#include "core/timing.h"

#if !CONFIG_HEAP_USE_HOOKS
#error "a development build watches the heap through CONFIG_HEAP_USE_HOOKS - see sdkconfig.defaults.dev"
#endif

static const char* TAG = "frame_watch";

/* The frame loop, the panel's sender and the second core's worker. */
#define WATCHED_TASKS_MAX   4

/* Events waiting for the frame's end. The hooks run on any watched task on
 * either core; the frame task alone consumes. */
#define PENDING_MAX         128

/* How far up the stack the caller of an allocation is looked for. The
 * allocator's own frames sit in IRAM, so the first frame outside it is the
 * call site. */
#define CALLER_DEPTH_MAX    6

/* What __builtin_return_address() gives past the outermost frame on Xtensa;
 * ESP-IDF's heap tracing stops at the same value. */
#define XTENSA_STACK_END_PC ((void*)0x40000000)

/* `sequence` is the event's index plus one once its fields are written: the
 * consumer reads a slot only when that matches, so it never sees a write in
 * progress, and a producer only claims an index the consumer has freed. */
typedef struct {
    uintptr_t site;
    uint8_t kind;
    uint32_t sequence;
} pending_event_t;

static volatile bool armed;
static volatile bool warning;
static TaskHandle_t watched[WATCHED_TASKS_MAX];
static volatile int watched_count;
static pending_event_t pending[PENDING_MAX];
static uint32_t pending_head;
static uint32_t pending_tail;
static uint32_t pending_dropped;

static TaskHandle_t frame_task;
static frame_watch_t watch;
static vprintf_like_t forward_vprintf;

static IRAM_ATTR bool
watching_this_task(void) {
    if (!armed || xPortInIsrContext()) {
        return false;
    }
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < watched_count; i++) {
        if (watched[i] == self) {
            return true;
        }
    }
    return false;
}

static IRAM_ATTR void
record(frame_watch_kind_t kind, uintptr_t site) {
    uint32_t index = __atomic_load_n(&pending_head, __ATOMIC_RELAXED);
    do {
        if (index - __atomic_load_n(&pending_tail, __ATOMIC_ACQUIRE) >= PENDING_MAX) {
            __atomic_fetch_add(&pending_dropped, 1, __ATOMIC_RELAXED);
            return;
        }
    } while (!__atomic_compare_exchange_n(&pending_head, &index, index + 1, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    pending_event_t* slot = &pending[index % PENDING_MAX];
    slot->site = site;
    slot->kind = (uint8_t)kind;
    __atomic_store_n(&slot->sequence, index + 1, __ATOMIC_RELEASE);
}

#define TRY_CALLER(N)                                                                                                  \
    do {                                                                                                               \
        void* const address = __builtin_return_address(N);                                                             \
        if (address == XTENSA_STACK_END_PC || !esp_ptr_executable(address)) {                                          \
            return 0;                                                                                                  \
        }                                                                                                              \
        if (!esp_ptr_in_iram(address)) {                                                                               \
            return (uintptr_t)address;                                                                                 \
        }                                                                                                              \
    } while (0)

/* __builtin_return_address() takes a constant, hence the unrolled walk.
 * A nonzero one is safe on this chip, where the heap's own tracing walks
 * the same way. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"

static IRAM_ATTR __attribute__((noinline)) uintptr_t
heap_call_site(void) {
    _Static_assert(CALLER_DEPTH_MAX == 6, "the walk below is unrolled to CALLER_DEPTH_MAX");
    TRY_CALLER(1);
    TRY_CALLER(2);
    TRY_CALLER(3);
    TRY_CALLER(4);
    TRY_CALLER(5);
    TRY_CALLER(6);
    return 0;
}

#pragma GCC diagnostic pop

IRAM_ATTR void
esp_heap_trace_alloc_hook(void* ptr, size_t size, uint32_t caps) {
    (void)ptr;
    (void)size;
    (void)caps;
    if (watching_this_task()) {
        record(FRAME_WATCH_ALLOC, heap_call_site());
    }
}

IRAM_ATTR void
esp_heap_trace_free_hook(void* ptr) {
    (void)ptr;
    if (watching_this_task()) {
        record(FRAME_WATCH_FREE, heap_call_site());
    }
}

/* The format string is the site: each ESP_LOG* call passes its own literal. */
static int
watched_vprintf(const char* format, va_list args) {
    if (!warning && watching_this_task()) {
        record(FRAME_WATCH_CONSOLE, (uintptr_t)format);
    }
    return forward_vprintf(format, args);
}

static void
add_task(TaskHandle_t task) {
    for (int i = 0; i < watched_count; i++) {
        if (watched[i] == task) {
            return;
        }
    }
    if (watched_count >= WATCHED_TASKS_MAX) {
        ESP_LOGE(TAG, "no room to watch task %s: raise WATCHED_TASKS_MAX", pcTaskGetName(task));
        return;
    }
    watched[watched_count] = task;
    watched_count++;
}

void
frame_watch_add_task(void* task) {
    add_task((TaskHandle_t)task);
}

void
frame_watch_start(void) {
    frame_task = xTaskGetCurrentTaskHandle();
    add_task(frame_task);
    job_on_worker_started(frame_watch_add_task);
    if (forward_vprintf == NULL) {
        frame_watch_reset(&watch);
        forward_vprintf = esp_log_set_vprintf(watched_vprintf);
    }
    armed = true;
}

/* Events stay pending from a slot whose producer has not finished writing;
 * the next drain picks them up. Overflow is counted only once judged. */
static void
drain(void) {
    const uint32_t head = __atomic_load_n(&pending_head, __ATOMIC_ACQUIRE);
    uint32_t tail = pending_tail;
    while (tail != head) {
        const pending_event_t* slot = &pending[tail % PENDING_MAX];
        if (__atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE) != tail + 1) {
            break;
        }
        frame_watch_note(&watch, (frame_watch_kind_t)slot->kind, slot->site);
        tail++;
    }
    __atomic_store_n(&pending_tail, tail, __ATOMIC_RELEASE);
    const uint32_t overflowed = __atomic_exchange_n(&pending_dropped, 0, __ATOMIC_RELAXED);
    if (watch.warmup_left == 0) {
        watch.dropped += overflowed;
    }
}

/* A log line's site is its format; one kept in flash is shown up to its
 * first newline, which names the line without a symbol lookup. */
static void
warn(const frame_watch_site_t* s) {
    const char* kind = frame_watch_kind_name((frame_watch_kind_t)s->kind);
    const int seen = frame_watch_frames_seen(s);
    warning = true;
    if (s->kind == FRAME_WATCH_CONSOLE && esp_ptr_in_drom((const void*)s->site)) {
        const char* format = (const char*)s->site;
        ESP_LOGW(TAG, "FRAME_WATCH %s in %d of %d frames at 0x%08lx: %.*s", kind, seen, FRAME_WATCH_WINDOW,
                 (unsigned long)s->site, (int)strcspn(format, "\n"), format);
    } else {
        ESP_LOGW(TAG, "FRAME_WATCH %s in %d of %d frames at 0x%08lx", kind, seen, FRAME_WATCH_WINDOW,
                 (unsigned long)s->site);
    }
    warning = false;
}

void
frame_watch_presented(void) {
    if (!armed || xTaskGetCurrentTaskHandle() != frame_task) {
        return;
    }
    drain();
    frame_watch_close_frame(&watch);
    const int64_t now_us = timing_now_us();
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        if (frame_watch_take_due(&watch.sites[i], now_us)) {
            warn(&watch.sites[i]);
        }
    }
}

void
frame_watch_restart(void) {
    frame_watch_settle(&watch);
    drain();
}

int
frame_watch_json(char* out, size_t out_size) {
    return frame_watch_format_json(&watch, out, out_size);
}

void
frame_watch_test_begin(void) {
    frame_watch_start();
    drain();
    frame_watch_reset(&watch);
}

frame_watch_verdict_t
frame_watch_test_end(void) {
    drain();
    const frame_watch_verdict_t verdict = frame_watch_verdict(&watch);
    frame_watch_settle(&watch);
    return verdict;
}

#endif
