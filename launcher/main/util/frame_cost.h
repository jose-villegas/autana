/*
 * frame_cost, where a frame's time goes, by name: a bracket's own
 * microseconds go to a named slot, read out once a window as ms per frame.
 *
 * Pure here, on a frame_cost_t a test can own; the shared instance exists
 * only in a development build on the chip, elsewhere a bracket is nothing.
 *
 * One name can be armed for the S3's two hardware counters: while armed,
 * every bracket reads them, one name keeps samples. A level is 24 bytes
 * larger and a slot 32; an unarmed bracket adds a few loads and branches.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAME_COST_SLOTS          12

/* Distinct names remembered for the console to list and check an arm
 * against, across windows. */
#define FRAME_COST_NAMES          32

/* Longest bracket name; FRAME_COST_END() refuses a longer one at compile
 * time, since a counter arm must be able to carry it on one console line. */
#define FRAME_COST_NAME_MAX       24
#define FRAME_COST_EVENT_NAME_MAX 17

/* A buffer for the time line at its worst: every slot with a name of the
 * longest length and its two figures at their widest, the total, and a drop
 * count. A smaller one loses the tail without a word. */
#define FRAME_COST_REPORT_MAX     672
_Static_assert(FRAME_COST_REPORT_MAX >= FRAME_COST_SLOTS * (FRAME_COST_NAME_MAX + 28) + 48,
               "FRAME_COST_REPORT_MAX no longer holds every slot at its longest");

/* The counts line of the armed name: "perf: ", the name, the three cycle
 * figures, the event name, its average and n, each at its widest. */
#define FRAME_COST_COUNTS_MAX 160
_Static_assert(FRAME_COST_COUNTS_MAX
                   >= 6 + FRAME_COST_NAME_MAX + 17 + 32 + 1 + FRAME_COST_EVENT_NAME_MAX + 5 + 10 + 3 + 10 + 1,
               "FRAME_COST_COUNTS_MAX no longer holds the counts line at its longest");

/* The event an arm gets when the console names none. */
#define FRAME_COST_DEFAULT_EVENT "insn"

/* Deeper than this and a bracket is not a stage; see frame_cost_enter(). */
#define FRAME_COST_STACK_DEPTH   8

/* Never a real mark: frame_cost_enter() hands it back when it could not
 * push, and frame_cost_leave() treats it as already gone. */
#define FRAME_COST_IGNORE_MARK   (-1)

typedef struct {
    const char* name;
    int64_t total_us;
    int64_t worst_us;
    /* The armed name's counter samples this window: own cycles and own
     * event counts per bracket, summed, with the cycle extremes. */
    uint32_t n;
    uint64_t cycles_sum;
    uint64_t event_sum;
    uint32_t cycles_min;
    uint32_t cycles_max;
} frame_cost_slot_t;

typedef struct {
    int64_t began_us;
    int64_t inside_us;
    uint32_t began_cycles;
    uint32_t began_event;
    uint32_t inside_cycles;
    uint32_t inside_event;
    /* Armed when this bracket began, so a counter read starts and ends in
     * one configuration. */
    bool counted;
} frame_cost_level_t;

typedef struct {
    frame_cost_slot_t slots[FRAME_COST_SLOTS];
    int count;
    int dropped;
    /* The armed name as an index into `names` plus one, 0 for none, and
     * its event as an index into the chip's event table. */
    int armed_name;
    int armed_event;
    /* A request waiting for the frame task: FRAME_COST_ARM_PENDING, the
     * name index plus one in bits 8..15 (0 disarms) and the event index in
     * the low byte; 0 when nothing waits. The newest request wins. */
    volatile uint32_t pending_arm;
    const char* names[FRAME_COST_NAMES];
    int name_count;
    int names_dropped;
    frame_cost_level_t stack[FRAME_COST_STACK_DEPTH];
    int depth;
} frame_cost_t;

/* Remembers a name for good, so the console can list it and check an arm
 * against it; a full table counts the miss instead. */
static inline void
frame_cost_note_name(frame_cost_t* cost, const char* name) {
    for (int i = 0; i < cost->name_count; i++) {
        if (cost->names[i] == name || strcmp(cost->names[i], name) == 0) {
            return;
        }
    }
    if (cost->name_count >= FRAME_COST_NAMES) {
        cost->names_dropped++;
        return;
    }
    cost->names[cost->name_count++] = name;
}

/* Names are string literals: one is found again by its address first. With
 * every slot taken a new name is dropped rather than charged to another. */
static inline frame_cost_slot_t*
frame_cost_slot(frame_cost_t* cost, const char* name) {
    for (int i = 0; i < cost->count; i++) {
        if (cost->slots[i].name == name || strcmp(cost->slots[i].name, name) == 0) {
            return &cost->slots[i];
        }
    }
    if (cost->count >= FRAME_COST_SLOTS) {
        cost->dropped++;
        return NULL;
    }
    frame_cost_slot_t* slot = &cost->slots[cost->count++];
    *slot = (frame_cost_slot_t){.name = name};
    frame_cost_note_name(cost, name);
    return slot;
}

/* The index of a name the console may arm, or -1 for one never bracketed. */
static inline int
frame_cost_name_index(const frame_cost_t* cost, const char* name) {
    for (int i = 0; i < cost->name_count; i++) {
        if (strcmp(cost->names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

#define FRAME_COST_ARM_PENDING 0x80000000u
_Static_assert(FRAME_COST_NAMES < 255, "a name index plus one must fit the request's byte");

/* From any task or core: asks for `name_index` (-1 disarms) with the event
 * at `event_index`. Nothing changes until the frame task applies it. */
static inline void
frame_cost_post_arm(frame_cost_t* cost, int name_index, int event_index) {
    cost->pending_arm = FRAME_COST_ARM_PENDING | (uint32_t)(name_index + 1) << 8 | (uint32_t)event_index;
}

/* Applies the waiting request, only for the owning task and only between
 * outermost brackets, so no bracket sees two configurations. A change of
 * arm drops the samples of the old one, never to be labelled with the new
 * event. `*event_index` is the event now armed, or -1 for none. */
static inline bool
frame_cost_apply_pending(frame_cost_t* cost, bool owner, int* event_index) {
    const uint32_t request = cost->pending_arm;
    if (request == 0 || !owner || cost->depth != 0) {
        return false;
    }
    if (!__sync_bool_compare_and_swap(&cost->pending_arm, request, 0u)) {
        return false;
    }
    cost->armed_name = (int)(request >> 8 & 0xffu);
    cost->armed_event = (int)(request & 0xffu);
    for (int i = 0; i < FRAME_COST_SLOTS; i++) {
        cost->slots[i].n = 0;
        cost->slots[i].cycles_sum = 0;
        cost->slots[i].event_sum = 0;
    }
    *event_index = cost->armed_name > 0 ? cost->armed_event : -1;
    return true;
}

static inline bool
frame_cost_is_armed(const frame_cost_t* cost, const char* name) {
    if (cost->armed_name <= 0) {
        return false;
    }
    const char* const armed = cost->names[cost->armed_name - 1];
    return armed == name || strcmp(armed, name) == 0;
}

/* The slot the time went to, or NULL for a name dropped for want of one. */
static inline frame_cost_slot_t*
frame_cost_add(frame_cost_t* cost, const char* name, int64_t us) {
    frame_cost_slot_t* slot = frame_cost_slot(cost, name);
    if (slot != NULL) {
        slot->total_us += us;
        slot->worst_us = us > slot->worst_us ? us : slot->worst_us;
    }
    return slot;
}

/* Pushes the level a bracket just started at and returns its mark, for
 * frame_cost_leave() to read back. A stack already FRAME_COST_STACK_DEPTH
 * deep pushes nothing and returns FRAME_COST_IGNORE_MARK instead. */
static inline int
frame_cost_enter_counted(frame_cost_t* cost, int64_t now_us, uint32_t cycles, uint32_t event) {
    if (cost->depth >= FRAME_COST_STACK_DEPTH) {
        return FRAME_COST_IGNORE_MARK;
    }
    const int mark = cost->depth++;
    cost->stack[mark] = (frame_cost_level_t){
        .began_us = now_us, .began_cycles = cycles, .began_event = event, .counted = cost->armed_name > 0};
    return mark;
}

static inline int
frame_cost_enter(frame_cost_t* cost, int64_t now_us) {
    return frame_cost_enter_counted(cost, now_us, 0, 0);
}

/* Charges `name` its elapsed time minus whatever ran inside it, then folds
 * the whole elapsed time into the level below, so that one is exclusive of
 * it in turn. Dropping the stack straight to `mark` both pops this level and
 * discards, uncharged, any deeper one whose own END never ran. A `mark` no
 * longer on the stack (already popped, or FRAME_COST_IGNORE_MARK) charges
 * nothing and reads 0. Returns the bracket's whole elapsed time, nested
 * brackets included, for a caller that wants the wall time too. */
static inline int64_t
frame_cost_leave_counted(frame_cost_t* cost, int mark, const char* name, int64_t now_us, uint32_t cycles,
                         uint32_t event) {
    if (mark < 0 || mark >= cost->depth) {
        return 0;
    }
    /* What an abandoned level's own children were charged is still time
     * this one did not spend. */
    frame_cost_level_t* const level = &cost->stack[mark];
    for (int deeper = mark + 1; deeper < cost->depth; deeper++) {
        level->inside_us += cost->stack[deeper].inside_us;
        if (level->counted) {
            level->inside_cycles += cost->stack[deeper].inside_cycles;
            level->inside_event += cost->stack[deeper].inside_event;
        }
    }
    cost->depth = mark;
    const int64_t elapsed = now_us - level->began_us;
    frame_cost_slot_t* const slot = frame_cost_add(cost, name, elapsed - level->inside_us);
    if (mark > 0) {
        cost->stack[mark - 1].inside_us += elapsed;
    }
    if (!level->counted) {
        return elapsed;
    }
    /* Free-running 32-bit counters: a difference is right modulo 2^32. A
     * level and its parent began under one arm, so both are counted. */
    const uint32_t elapsed_cycles = cycles - level->began_cycles;
    const uint32_t elapsed_event = event - level->began_event;
    if (slot != NULL && frame_cost_is_armed(cost, name)) {
        const uint32_t own = elapsed_cycles - level->inside_cycles;
        slot->cycles_min = slot->n == 0 || own < slot->cycles_min ? own : slot->cycles_min;
        slot->cycles_max = own > slot->cycles_max ? own : slot->cycles_max;
        slot->cycles_sum += own;
        slot->event_sum += elapsed_event - level->inside_event;
        slot->n++;
    }
    if (mark > 0) {
        cost->stack[mark - 1].inside_cycles += elapsed_cycles;
        cost->stack[mark - 1].inside_event += elapsed_event;
    }
    return elapsed;
}

static inline void
frame_cost_leave(frame_cost_t* cost, int mark, const char* name, int64_t now_us) {
    (void)frame_cost_leave_counted(cost, mark, name, now_us, 0, 0);
}

/* The " | total T.TT" tail: the sum of every slot's own average, in the same
 * milliseconds-with-two-decimals shape as a slot's line. Appended after
 * every slot, so a reader sees how the frame adds up without summing it
 * themselves. */
static inline int
frame_cost_append_total(char* out, size_t out_size, int length, int64_t total_avg_us) {
    const int wrote = snprintf(out + length, out_size - (size_t)length, " | total %d.%02d", (int)(total_avg_us / 1000),
                               (int)(total_avg_us % 1000 / 10));
    if (wrote < 0 || (size_t)(length + wrote) >= out_size) {
        out[length] = '\0';
        return length;
    }
    return length + wrote;
}

/* " +N dropped", once there is a total to trail: a name refused for want of
 * a slot, this window. */
static inline int
frame_cost_append_dropped(char* out, size_t out_size, int length, int dropped) {
    if (dropped <= 0) {
        return length;
    }
    const int wrote = snprintf(out + length, out_size - (size_t)length, " +%d dropped", dropped);
    if (wrote < 0 || (size_t)(length + wrote) >= out_size) {
        out[length] = '\0';
        return length;
    }
    return length + wrote;
}

/* "perf: name cyc avg/min/max A/B/C event avg D n=N" for the armed slot,
 * when it ran this window: the one line `autana perf` reads. Taken before
 * the report, which forgets the window. Returns its length, 0 for none. */
static inline int
frame_cost_counts_line(const frame_cost_t* cost, const char* event, char* out, size_t out_size) {
    for (int i = 0; i < cost->count; i++) {
        const frame_cost_slot_t* slot = &cost->slots[i];
        if (slot->n == 0 || !frame_cost_is_armed(cost, slot->name)) {
            continue;
        }
        const int wrote =
            snprintf(out, out_size, "perf: %s cyc avg/min/max %u/%u/%u %s avg %u n=%u", slot->name,
                     (unsigned)(slot->cycles_sum / slot->n), (unsigned)slot->cycles_min, (unsigned)slot->cycles_max,
                     event, (unsigned)(slot->event_sum / slot->n), (unsigned)slot->n);
        return wrote < 0 || (size_t)wrote >= out_size ? 0 : wrote;
    }
    return 0;
}

/* "name avg/worst" per slot, then a total and any drop count, then forgets
 * the window and empties the open-bracket stack, so a report taken mid-
 * bracket leaves no stale level for that bracket's own END to find.
 *
 * `frames` zero leaves the window for the next report to find; `out_size`
 * zero writes nothing but still clears the window: the caller did ask for
 * a report. */
static inline int
frame_cost_report(frame_cost_t* cost, uint32_t frames, char* out, size_t out_size) {
    cost->depth = 0;
    if (frames == 0) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    if (out_size == 0) {
        cost->count = 0;
        cost->dropped = 0;
        return 0;
    }

    out[0] = '\0';
    int length = 0;
    int64_t total_avg_us = 0;
    for (int i = 0; i < cost->count; i++) {
        const frame_cost_slot_t* slot = &cost->slots[i];
        const int64_t average_us = slot->total_us / frames;
        total_avg_us += average_us;
        const int wrote = snprintf(out + length, out_size - (size_t)length, "%s%s %d.%02d/%d.%d", i > 0 ? "  " : "",
                                   slot->name, (int)(average_us / 1000), (int)(average_us % 1000 / 10),
                                   (int)(slot->worst_us / 1000), (int)(slot->worst_us % 1000 / 100));
        if (wrote < 0 || (size_t)(length + wrote) >= out_size) {
            out[length] = '\0';
            cost->count = 0;
            cost->dropped = 0;
            return length;
        }
        length += wrote;
    }

    length = frame_cost_append_total(out, out_size, length, total_avg_us);
    length = frame_cost_append_dropped(out, out_size, length, cost->dropped);
    cost->count = 0;
    cost->dropped = 0;
    return length;
}

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
#define FRAME_COST_ENABLED 1
#else
#define FRAME_COST_ENABLED 0
#endif

#if FRAME_COST_ENABLED
int frame_cost_begin(void);
int64_t frame_cost_end(int mark, const char* name);
int frame_cost_take_report(uint32_t frames, char* out, size_t out_size);

int frame_cost_take_counts(char* out, size_t out_size);

/* The shared instance's name index, -1 for a name never bracketed. A posted
 * arm is applied by the frame task at its next outermost bracket; -1 as the
 * name disarms. */
int frame_cost_shared_name_index(const char* name);
void frame_cost_shared_post_arm(int name_index, int event_index);

/* True while nothing is armed or waiting: the counters are free for a
 * caller that programs them itself. */
bool frame_cost_counters_idle(void);

typedef struct {
    const char* name;
    uint16_t select;
    uint16_t mask;
} frame_cost_event_t;

int frame_cost_event_index(const char* name);
const frame_cost_event_t* frame_cost_event_at(int index);
int frame_cost_event_count(void);

/* The names seen so far, for a console listing; NULL past the last. */
const char* frame_cost_name_at(int index);
int frame_cost_names_dropped(void);

#define FRAME_COST_BEGIN(mark) const int mark = frame_cost_begin()
#define FRAME_COST_END(mark, name)                                                                                     \
    do {                                                                                                               \
        _Static_assert(sizeof(name) - 1 <= FRAME_COST_NAME_MAX, name " is too long a frame_cost name");                \
        (void)frame_cost_end((mark), (name));                                                                          \
    } while (0)

/* FRAME_COST_END that also yields the bracket's whole elapsed microseconds,
 * from the clock read it already makes. */
#define FRAME_COST_END_US(mark, name)                                                                                  \
    ({                                                                                                                 \
        _Static_assert(sizeof(name) - 1 <= FRAME_COST_NAME_MAX, name " is too long a frame_cost name");                \
        frame_cost_end((mark), (name));                                                                                \
    })
#else
#define FRAME_COST_BEGIN(mark)     ((void)0)
#define FRAME_COST_END(mark, name) ((void)0)
#endif
