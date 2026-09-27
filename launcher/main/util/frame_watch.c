#include "util/frame_watch.h"

#if FRAME_WATCH_ENABLED

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !CONFIG_HEAP_USE_HOOKS
#error "a development build watches the heap through CONFIG_HEAP_USE_HOOKS - see sdkconfig.defaults.dev"
#endif

static const char* TAG = "frame_watch";

/* The frame loop, the panel's sender and the second core's worker. */
#define WATCHED_TASKS_MAX 4

/* Events from one frame, waiting for its end: the hooks run on any task and
 * either core, so they only append here, and the frame task alone reads. */
#define PENDING_MAX       64

/* How far up the stack the caller of an allocation is looked for. The
 * allocator's own frames sit in IRAM, so the first frame outside it is the
 * call site. */
#define CALLER_DEPTH_MAX  6

typedef struct {
    uintptr_t site;
    uint8_t kind;
} pending_event_t;

static volatile bool inside;
static TaskHandle_t watched[WATCHED_TASKS_MAX];
static volatile int watched_count;
static pending_event_t pending[PENDING_MAX];
static uint32_t pending_count;

static TaskHandle_t frame_task;
static frame_watch_t shell_watch;
static frame_watch_t test_watch;
static bool testing;
static bool shell_inside;

static vprintf_like_t forward_vprintf;

static IRAM_ATTR bool
watching_this_task(void) {
    if (!inside || xPortInIsrContext()) {
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
    const uint32_t slot = __atomic_fetch_add(&pending_count, 1, __ATOMIC_RELAXED);
    if (slot < PENDING_MAX) {
        pending[slot] = (pending_event_t){.site = site, .kind = (uint8_t)kind};
    }
}

static IRAM_ATTR bool
is_call_site(void* address) {
    return esp_ptr_executable(address) && !esp_ptr_in_iram(address) && address != (void*)0x40000000;
}

#define TRY_CALLER(N)                                                                                                  \
    do {                                                                                                               \
        void* const address = __builtin_return_address(N);                                                             \
        if (!esp_ptr_executable(address)) {                                                                            \
            return 0;                                                                                                  \
        }                                                                                                              \
        if (is_call_site(address)) {                                                                                   \
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
    if (watching_this_task()) {
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
    if (watched_count < WATCHED_TASKS_MAX) {
        watched[watched_count] = task;
        watched_count++;
    }
}

void
frame_watch_add_task(void* task) {
    add_task((TaskHandle_t)task);
}

void
frame_watch_start(void) {
    frame_task = xTaskGetCurrentTaskHandle();
    add_task(frame_task);
    if (forward_vprintf == NULL) {
        frame_watch_reset(&shell_watch);
        forward_vprintf = esp_log_set_vprintf(watched_vprintf);
    }
}

static void
drain(frame_watch_t* w) {
    uint32_t count = __atomic_exchange_n(&pending_count, 0, __ATOMIC_RELAXED);
    if (count > PENDING_MAX) {
        w->dropped += count - PENDING_MAX;
        count = PENDING_MAX;
    }
    for (uint32_t i = 0; i < count; i++) {
        frame_watch_note(w, (frame_watch_kind_t)pending[i].kind, pending[i].site);
    }
}

/* A log line's site is its format; one kept in flash is shown up to its
 * first newline, which names the line without a symbol lookup. */
static void
warn(const frame_watch_site_t* s) {
    const char* kind = frame_watch_kind_name((frame_watch_kind_t)s->kind);
    const int seen = frame_watch_frames_seen(s);
    if (s->kind == FRAME_WATCH_CONSOLE && esp_ptr_in_drom((const void*)s->site)) {
        const char* format = (const char*)s->site;
        ESP_LOGW(TAG, "FRAME_WATCH %s in %d of %d frames at 0x%08lx: %.*s", kind, seen, FRAME_WATCH_WINDOW,
                 (unsigned long)s->site, (int)strcspn(format, "\n"), format);
        return;
    }
    ESP_LOGW(TAG, "FRAME_WATCH %s in %d of %d frames at 0x%08lx", kind, seen, FRAME_WATCH_WINDOW,
             (unsigned long)s->site);
}

static void
close_and_warn(frame_watch_t* w) {
    drain(w);
    frame_watch_close_frame(w);
    const int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        if (frame_watch_take_due(&w->sites[i], now_us)) {
            warn(&w->sites[i]);
        }
    }
}

void
frame_watch_frame_begin(void) {
    if (testing) {
        return;
    }
    shell_inside = true;
    inside = true;
}

void
frame_watch_frame_end(void) {
    if (testing) {
        return;
    }
    inside = false;
    shell_inside = false;
    close_and_warn(&shell_watch);
}

void
frame_watch_settle(void) {
    shell_watch.warmup_left = FRAME_WATCH_WARMUP;
}

int
frame_watch_json(char* out, size_t out_size) {
    return frame_watch_format_json(&shell_watch, out, out_size);
}

void
frame_watch_test_begin(void) {
    frame_watch_start();
    inside = false;
    drain(&shell_watch);
    frame_watch_reset(&test_watch);
    testing = true;
    inside = true;
}

int
frame_watch_test_end(void) {
    if (!testing) {
        return 0;
    }
    inside = false;
    close_and_warn(&test_watch);
    testing = false;
    inside = shell_inside;
    return test_watch.ever_repeating;
}

void
frame_watch_presented(void) {
    if (!testing || xTaskGetCurrentTaskHandle() != frame_task) {
        return;
    }
    inside = false;
    close_and_warn(&test_watch);
    inside = true;
}

#endif
