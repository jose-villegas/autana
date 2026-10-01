#include "util/frame_cost.h"

#if FRAME_COST_ENABLED

#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa/xt_perf_consts.h"
#include "xtensa_perfmon_access.h"

/* select/mask pairs from xtensa/xt_perf_consts.h. */
#define FRAME_COST_EVENTS(X)                                                                                           \
    X("insn", XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL)                                                                   \
    X("window", XTPERF_CNT_EXR, XTPERF_MASK_EXR_WINDOW)                                                                \
    X("level1_int", XTPERF_CNT_EXR, XTPERF_MASK_EXR_LEVEL1_INT)                                                        \
    X("replays", XTPERF_CNT_EXR, XTPERF_MASK_EXR_REPLAYS)                                                              \
    X("icache_miss_stall", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_CACHE_MISS)                                         \
    X("iterative_mul", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_MUL)                                          \
    X("iterative_div", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_DIV)                                          \
    X("d_stall_all", XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL)                                                      \
    X("bubbles_cti", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_CTI)                                                      \
    X("bubbles_all", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_ALL)                                                      \
    X("branch_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_TAKEN)                                                  \
    X("branch_not_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_NOT_TAKEN)                                          \
    X("call", XTPERF_CNT_INSN, (uint16_t)(XTPERF_MASK_INSN_CALL | XTPERF_MASK_INSN_CALLX))                             \
    X("icache_miss_fetch", XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_CACHE_MISSES)                                           \
    X("iram_fetch", XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_IRAM)

#define FRAME_COST_EVENT_ROW(name, select, mask)                                                                       \
    _Static_assert(sizeof(name) - 1 <= FRAME_COST_EVENT_NAME_MAX, name " is too long an event name");
FRAME_COST_EVENTS(FRAME_COST_EVENT_ROW)
#undef FRAME_COST_EVENT_ROW

#define FRAME_COST_EVENT_ROW(name, select, mask) {name, select, mask},
static const frame_cost_event_t events[] = {FRAME_COST_EVENTS(FRAME_COST_EVENT_ROW)};
#undef FRAME_COST_EVENT_ROW

const frame_cost_event_t*
frame_cost_event_find(const char* name) {
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
        if (strcmp(events[i].name, name) == 0) {
            return &events[i];
        }
    }
    return NULL;
}

int
frame_cost_event_count(void) {
    return (int)(sizeof events / sizeof events[0]);
}

const frame_cost_event_t*
frame_cost_event_at(int index) {
    return index >= 0 && index < frame_cost_event_count() ? &events[index] : NULL;
}

static frame_cost_t shared;
static TaskHandle_t owner_task;
static int foreign_task_calls;
static int arm_refused;

/* The request from the console, written name and event first and `pending`
 * last; the frame task consumes it. */
static char pending_name[FRAME_COST_NAME_MAX + 1];
static char pending_event[FRAME_COST_EVENT_NAME_MAX + 1];
static volatile bool pending;

bool
frame_cost_request_arm(const char* name, const char* event) {
    if (pending || strlen(name) > FRAME_COST_NAME_MAX || frame_cost_event_find(event) == NULL) {
        return false;
    }
    strcpy(pending_name, name);
    strcpy(pending_event, event);
    __sync_synchronize();
    pending = true;
    return true;
}

bool
frame_cost_name_known(const char* name) {
    return frame_cost_name_seen(&shared, name);
}

const char*
frame_cost_name_at(int index) {
    return index >= 0 && index < shared.name_count ? shared.names[index] : NULL;
}

int
frame_cost_names_dropped(void) {
    return shared.names_dropped;
}

/* Counters live on the core that reads them, so the frame task configures
 * them itself. Applied between outermost brackets only: a bracket must
 * begin and end under one configuration. */
static void
apply_pending_arm(void) {
    xtensa_perfmon_stop();
    const frame_cost_event_t* const event = frame_cost_event_find(pending_event);
    if (pending_name[0] == '\0' || event == NULL) {
        (void)frame_cost_arm(&shared, "", FRAME_COST_DEFAULT_EVENT);
    } else if (frame_cost_arm(&shared, pending_name, pending_event)) {
        xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, 0xffff, 0, -1);
        xtensa_perfmon_init(1, event->select, event->mask, 0, -1);
        xtensa_perfmon_reset(0);
        xtensa_perfmon_reset(1);
        xtensa_perfmon_start();
    } else {
        arm_refused++;
    }
    pending = false;
}

/* The frame loop's own task claims ownership on its first bracket; a begin
 * from any other task is refused rather than corrupt the shared instance. */
int
frame_cost_begin(void) {
    const TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (owner_task == NULL) {
        owner_task = caller;
    }
    if (caller != owner_task) {
        foreign_task_calls++;
        return FRAME_COST_IGNORE_MARK;
    }
    if (pending && shared.depth == 0) {
        apply_pending_arm();
    }
    const bool counting = shared.armed[0] != '\0';
    return frame_cost_enter_counted(&shared, esp_timer_get_time(), counting ? xtensa_perfmon_value(0) : 0,
                                    counting ? xtensa_perfmon_value(1) : 0);
}

void
frame_cost_end(int mark, const char* name) {
    const bool counting = shared.armed[0] != '\0';
    frame_cost_leave_counted(&shared, mark, name, esp_timer_get_time(), counting ? xtensa_perfmon_value(0) : 0,
                             counting ? xtensa_perfmon_value(1) : 0);
}

static int
append_counter(char* out, size_t out_size, int length, const char* what, int* count) {
    if (*count == 0) {
        return length;
    }
    const int wrote = snprintf(out + length, out_size - (size_t)length, " +%d %s", *count, what);
    *count = 0;
    if (wrote < 0 || (size_t)(length + wrote) >= out_size) {
        out[length] = '\0';
        return length;
    }
    return length + wrote;
}

int
frame_cost_take_report(uint32_t frames, char* out, size_t out_size) {
    int length = frame_cost_report(&shared, frames, out, out_size);
    if (frames == 0 || out_size == 0) {
        return length;
    }
    length = append_counter(out, out_size, length, "arm refused", &arm_refused);
    return append_counter(out, out_size, length, "foreign", &foreign_task_calls);
}

#endif
