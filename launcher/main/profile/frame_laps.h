/*
 * frame_laps, the phases inside a frame_cost bracket that are too short or
 * too many to bracket (one triangle's set-up and fill): a lap reads the
 * core's cycle counter, one instruction, where a bracket reads the
 * microsecond clock, about a microsecond a read.
 *
 * Each core runs one lap at a time: starting a lap closes the one running
 * and charges it its cycles, so laps tile the time between a core's first
 * and last lap. A lap also counts its starts, so what the laps themselves
 * cost, a few cycles each, can be taken back out: a lap around a vertex or a
 * triangle is not free next to the work it times. Once a window the laps
 * join the console as ms per frame per core, on a line of their own under
 * frame_cost's.
 *
 * Pure here, on a frame_laps_core_t a test can own; the shared laps exist
 * only in a lapped perf-scope build on the chip
 * (`autana flash diag --perf-scope --frame-laps`), elsewhere every
 * FRAME_LAP*() is nothing: their cost would move the perf scope's budgets.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "collections/strbuf.h"

/* Distinct lap names, across every core. */
#define FRAME_LAPS_NAMES      16
#define FRAME_LAPS_CORES      2
#define FRAME_LAPS_NAME_MAX   24

/* The report line at its widest: one lap's cost, then per core its label
 * and every name at its longest with its two figures at their widest. */
#define FRAME_LAPS_REPORT_MAX (FRAME_LAPS_CORES * (8 + (FRAME_LAPS_NAMES * (FRAME_LAPS_NAME_MAX + 24))) + 32)

/* A lap's place in the name table plus one; 0 is no lap. */
typedef uint8_t frame_laps_slot_t;

typedef struct {
    uint64_t cycles[FRAME_LAPS_NAMES];
    uint32_t starts[FRAME_LAPS_NAMES];
    uint32_t began; /* the cycle count at the last lap boundary */
    frame_laps_slot_t running;
} frame_laps_core_t;

/* Closes the running lap at `now`, charging it, and runs `next` from there;
 * a `next` of 0 runs none. */
static inline __attribute__((always_inline)) void
frame_laps_switch(frame_laps_core_t* core, uint32_t now, frame_laps_slot_t next) {
    if (core->running != 0) {
        core->cycles[core->running - 1] += now - core->began;
    }
    if (next != 0) {
        core->starts[next - 1]++;
    }
    core->began = now;
    core->running = next;
}

/* Charges `slot` everything since the core's last boundary, whatever ran,
 * and runs none: for a wait that began where the last lap ended. Not a
 * start: it does not run at the rate a lap does. */
static inline __attribute__((always_inline)) void
frame_laps_charge_since(frame_laps_core_t* core, uint32_t now, frame_laps_slot_t slot) {
    core->running = slot;
    frame_laps_switch(core, now, 0);
}

/* "lap N ns", one lap's own cost, then "cpuN name ms/starts ..." for each
 * core: its laps that ran, in ms per frame to the microsecond and starts per
 * frame, cores apart by " | "; then forgets the window. A lap still running
 * keeps running. Returns the line's length, 0 for no laps. */
static inline int
frame_laps_report(frame_laps_core_t cores[], int core_count, const char* const names[], uint32_t frames,
                  uint32_t cycles_per_us, uint32_t lap_cycles, char* out, size_t out_size) {
    strbuf_t line = strbuf_make(out, out_size);
    bool any = false;
    for (int c = 0; c < core_count; c++) {
        bool labelled = false;
        for (int i = 0; i < FRAME_LAPS_NAMES; i++) {
            if (cores[c].cycles[i] == 0) {
                continue;
            }
            if (!any) {
                strbuf_appendf(&line, "lap %u ns", (unsigned)(lap_cycles * 1000U / cycles_per_us));
                any = true;
            }
            if (!labelled) {
                strbuf_appendf(&line, " | cpu%d", c);
                labelled = true;
            }
            const uint64_t us = frames == 0 ? 0 : cores[c].cycles[i] / cycles_per_us / frames;
            const uint32_t starts = frames == 0 ? 0 : cores[c].starts[i] / frames;
            strbuf_appendf(&line, " %s %u.%03u/%u", names[i], (unsigned)(us / 1000), (unsigned)(us % 1000),
                           (unsigned)starts);
            cores[c].cycles[i] = 0;
            cores[c].starts[i] = 0;
        }
    }
    return (int)line.length;
}

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF_LAPS
#define FRAME_LAPS_ENABLED 1
#else
#define FRAME_LAPS_ENABLED 0
#endif

#if FRAME_LAPS_ENABLED
#include "esp_cpu.h"

extern frame_laps_core_t frame_laps_cores[FRAME_LAPS_CORES];

/* The slot of `name`, adding it on first sight; 0 once the table is full. */
frame_laps_slot_t frame_laps_slot(const char* name);

/* The window's laps as ms per frame, from the frame task between frames,
 * while no other core runs a lap. Returns the line's length. */
int frame_laps_take_report(uint32_t frames, char* out, size_t out_size);

static inline __attribute__((always_inline)) void
frame_laps_to(frame_laps_slot_t next) {
    frame_laps_switch(&frame_laps_cores[esp_cpu_get_core_id()], esp_cpu_get_cycle_count(), next);
}

static inline __attribute__((always_inline)) void
frame_laps_since(frame_laps_slot_t slot) {
    frame_laps_charge_since(&frame_laps_cores[esp_cpu_get_core_id()], esp_cpu_get_cycle_count(), slot);
}

/* Each call site finds its name's slot once. */
#define FRAME_LAPS_SLOT_OF(name)                                                                                       \
    ({                                                                                                                 \
        _Static_assert(sizeof(name) - 1 <= FRAME_LAPS_NAME_MAX, name " is too long a lap name");                       \
        static frame_laps_slot_t lap_slot;                                                                             \
        if (lap_slot == 0) {                                                                                           \
            lap_slot = frame_laps_slot(name);                                                                          \
        }                                                                                                              \
        lap_slot;                                                                                                      \
    })

#define FRAME_LAP(name)       frame_laps_to(FRAME_LAPS_SLOT_OF(name))
#define FRAME_LAP_END()       frame_laps_to(0)
#define FRAME_LAP_SINCE(name) frame_laps_since(FRAME_LAPS_SLOT_OF(name))
#else
#define FRAME_LAP(name)       ((void)0)
#define FRAME_LAP_END()       ((void)0)
#define FRAME_LAP_SINCE(name) ((void)0)
#endif
