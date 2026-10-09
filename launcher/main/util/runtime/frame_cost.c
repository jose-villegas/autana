#include "util/runtime/frame_cost.h"

#if FRAME_COST_ENABLED

#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa/xt_perf_consts.h"
#include "xtensa_perfmon_access.h"

#include "util/runtime/timing.h"

/* select/mask pairs from xtensa/xt_perf_consts.h. */
#define FRAME_COST_EVENTS(X)                                                                                           \
    X("insn", XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL)                                                                   \
    X("window", XTPERF_CNT_EXR, XTPERF_MASK_EXR_WINDOW)                                                                \
    X("level1_int", XTPERF_CNT_EXR, XTPERF_MASK_EXR_LEVEL1_INT)                                                        \
    X("replays", XTPERF_CNT_EXR, XTPERF_MASK_EXR_REPLAYS)                                                              \
    X("iterative_mul", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_MUL)                                          \
    X("iterative_div", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_DIV)                                          \
    X("d_stall_all", XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL)                                                      \
    X("bubbles_cti", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_CTI)                                                      \
    X("bubbles_all", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_ALL)                                                      \
    X("branch_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_TAKEN)                                                  \
    X("branch_not_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_NOT_TAKEN)                                          \
    X("call", XTPERF_CNT_INSN, (uint16_t)(XTPERF_MASK_INSN_CALL | XTPERF_MASK_INSN_CALLX))                             \
    X("iram_fetch", XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_IRAM)

#define FRAME_COST_EVENT_CHECK(name, select, mask)                                                                     \
    _Static_assert(sizeof(name) - 1 <= FRAME_COST_EVENT_NAME_MAX, name " is too long an event name");
FRAME_COST_EVENTS(FRAME_COST_EVENT_CHECK)

#define FRAME_COST_EVENT_ROW(name, select, mask) {name, select, mask},
static const frame_cost_event_t events[] = {FRAME_COST_EVENTS(FRAME_COST_EVENT_ROW)};

int
frame_cost_event_index(const char* name) {
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
        if (strcmp(events[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
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

int
frame_cost_shared_name_index(const char* name) {
    return frame_cost_name_index(&shared, name);
}

void
frame_cost_shared_post_arm(int name_index, int event_index) {
    frame_cost_post_arm(&shared, name_index, event_index);
}

bool
frame_cost_counters_idle(void) {
    return shared.armed_name == 0 && shared.pending_arm == 0;
}

const char*
frame_cost_name_at(int index) {
    return index >= 0 && index < shared.name_count ? shared.names[index] : NULL;
}

int
frame_cost_names_dropped(void) {
    return shared.names_dropped;
}

/* Counters live on the core that reads them, so the frame task programs
 * them itself, when frame_cost_apply_pending() lets it. */
static void
program_counters(int event_index) {
    xtensa_perfmon_stop();
    if (event_index < 0) {
        return;
    }
    const frame_cost_event_t* const event = &events[event_index];
    xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, 0xffff, 0, -1);
    xtensa_perfmon_init(1, event->select, event->mask, 0, -1);
    xtensa_perfmon_reset(0);
    xtensa_perfmon_reset(1);
    xtensa_perfmon_start();
}

/* The frame loop's own task claims ownership on its first bracket; a begin
 * from any other task is refused rather than corrupt the shared instance. */
int
frame_cost_begin(void) {
    const TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (owner_task == NULL) {
        owner_task = caller;
    }
    int event_index = -1;
    if (shared.pending_arm != 0 && frame_cost_apply_pending(&shared, caller == owner_task, &event_index)) {
        program_counters(event_index);
    }
    if (caller != owner_task) {
        foreign_task_calls++;
        return FRAME_COST_IGNORE_MARK;
    }
    const bool counting = shared.armed_name > 0;
    return frame_cost_enter_counted(&shared, timing_now_us(), counting ? xtensa_perfmon_value(0) : 0,
                                    counting ? xtensa_perfmon_value(1) : 0);
}

int64_t
frame_cost_end(int mark, const char* name) {
    const bool counting = shared.armed_name > 0;
    return frame_cost_leave_counted(&shared, mark, name, timing_now_us(), counting ? xtensa_perfmon_value(0) : 0,
                                    counting ? xtensa_perfmon_value(1) : 0);
}

int
frame_cost_take_counts(char* out, size_t out_size) {
    return frame_cost_counts_line(&shared, events[shared.armed_event].name, out, out_size);
}

int
frame_cost_take_report(uint32_t frames, char* out, size_t out_size) {
    int length = frame_cost_report(&shared, frames, out, out_size);
    if (frames == 0 || out_size == 0 || foreign_task_calls == 0) {
        return length;
    }
    const int wrote = snprintf(out + length, out_size - (size_t)length, " +%d foreign", foreign_task_calls);
    foreign_task_calls = 0;
    if (wrote < 0 || (size_t)(length + wrote) >= out_size) {
        out[length] = '\0';
        return length;
    }
    return length + wrote;
}

#endif
