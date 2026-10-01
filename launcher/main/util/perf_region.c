#include "util/perf_region.h"

#if PERF_REGION_ENABLED

#include <stdio.h>

#include "esp_cpu.h"
#include "esp_err.h"
#include "xtensa/xt_perf_consts.h"
#include "xtensa_perfmon_access.h"

static perf_region_registry_t shared;

static const perf_region_event_t events[] = {
    {"insn", XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL},
    {"window", XTPERF_CNT_EXR, XTPERF_MASK_EXR_WINDOW},
    {"level1_int", XTPERF_CNT_EXR, XTPERF_MASK_EXR_LEVEL1_INT},
    {"replays", XTPERF_CNT_EXR, XTPERF_MASK_EXR_REPLAYS},
    {"icache_miss_stall", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_CACHE_MISS},
    {"iterative_mul", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_MUL},
    {"iterative_div", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_DIV},
    {"d_stall_all", XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL},
    {"bubbles_cti", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_CTI},
    {"bubbles_all", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_ALL},
    {"branch_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_TAKEN},
    {"branch_not_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_NOT_TAKEN},
    {"call", XTPERF_CNT_INSN, (uint16_t)(XTPERF_MASK_INSN_CALL | XTPERF_MASK_INSN_CALLX)},
    {"icache_miss_fetch", XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_CACHE_MISSES},
    {"iram_fetch", XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_IRAM},
};

static volatile const perf_region_entry_t* armed_region;
static volatile const perf_region_event_t* armed_event;
static perf_region_mark_t* active_mark;

perf_region_registry_t*
perf_region_shared(void) {
    return &shared;
}

const perf_region_event_t*
perf_region_event_find(const char* name) {
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
        if (strcmp(events[i].name, name) == 0) {
            return &events[i];
        }
    }
    return NULL;
}

int
perf_region_event_count(void) {
    return (int)(sizeof events / sizeof events[0]);
}

const perf_region_event_t*
perf_region_event_at(int index) {
    return index >= 0 && index < perf_region_event_count() ? &events[index] : NULL;
}

bool
perf_region_begin(perf_region_mark_t* mark, const perf_region_entry_t* region, const perf_region_event_t* event) {
    *mark = (perf_region_mark_t){0};
    if (active_mark != NULL) {
        return false;
    }
    if (event == NULL) {
        if (armed_region != region) {
            return false;
        }
        event = armed_event;
    }
    if (event == NULL) {
        return false;
    }

    xtensa_perfmon_stop();
    xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, 0xffff, 0, -1);
    xtensa_perfmon_init(1, event->select, event->mask, 0, -1);
    xtensa_perfmon_reset(0);
    xtensa_perfmon_reset(1);
    xtensa_perfmon_start();
    *mark = (perf_region_mark_t){.region = region, .event = event, .core = esp_cpu_get_core_id(), .active = true};
    active_mark = mark;
    return true;
}

bool
perf_region_end(perf_region_mark_t* mark, perf_region_counts_t* counts) {
    if (!mark->active || active_mark != mark || esp_cpu_get_core_id() != mark->core) {
        return false;
    }
    xtensa_perfmon_stop();
    const perf_region_counts_t result = {
        .cycles = xtensa_perfmon_value(0),
        .event = xtensa_perfmon_value(1),
        .cycles_overflowed = xtensa_perfmon_overflow(0) != ESP_OK,
        .event_overflowed = xtensa_perfmon_overflow(1) != ESP_OK,
    };
    mark->active = false;
    active_mark = NULL;
    if (counts != NULL) {
        *counts = result;
    } else {
        printf("PERF %s cycles=%u %s=%u%s%s\n", mark->region->name, (unsigned)result.cycles, mark->event->name,
               (unsigned)result.event, result.cycles_overflowed ? " overflow=cycles" : "",
               result.event_overflowed ? " overflow=event" : "");
        fflush(stdout);
    }
    return true;
}

static void
reply_error(perf_region_reply_fn reply, const char* reason, const char* about) {
    char line[PERF_REGION_NAME_MAX + PERF_REGION_EVENT_NAME_MAX + 24];
    snprintf(line, sizeof line, "PERF_ERR %s %s", reason, about);
    reply(line);
}

bool
perf_region_handle_line(const char* line, perf_region_reply_fn reply) {
    if (strcmp(line, "off") == 0) {
        armed_event = NULL;
        armed_region = NULL;
        reply("PERF_OK off");
        return true;
    }

    char region_name[PERF_REGION_NAME_MAX + 1];
    char event_name[PERF_REGION_EVENT_NAME_MAX + 1] = "insn";
    char extra[2];
    const int words = sscanf(line, "%31s %31s %1s", region_name, event_name, extra);
    if (words < 1 || words > 2) {
        reply("PERF_ERR usage PERF <region|off> [event]");
        return false;
    }
    perf_region_entry_t* const region = perf_region_find(&shared, region_name);
    if (region == NULL) {
        reply_error(reply, "unknown", region_name);
        return false;
    }
    const perf_region_event_t* const event = perf_region_event_find(event_name);
    if (event == NULL) {
        reply_error(reply, "event", event_name);
        return false;
    }
    armed_event = event;
    armed_region = region;
    char response[PERF_REGION_NAME_MAX + PERF_REGION_EVENT_NAME_MAX + 16];
    snprintf(response, sizeof response, "PERF_OK %s event=%s", region->name, event->name);
    reply(response);
    return true;
}

#endif
