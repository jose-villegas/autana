/*
 * frame_cost - where a frame's time goes, by name: a bracket's microseconds
 * go to a named slot, own time only, and once a window the slots are read
 * out as milliseconds per frame and forgotten.
 *
 * Pure here, on a frame_cost_t a test can own. The shared instance and its
 * clock exist only in a development build on the chip; on a host and in
 * release a bracket is nothing, so carrying one costs no clock and no link.
 *
 * Two clock reads per bracket. Around a stage of a frame, never a pixel.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAME_COST_SLOTS       12

/* Deeper than this and a bracket is not a stage; see frame_cost_enter(). */
#define FRAME_COST_STACK_DEPTH 8

/* Never a real mark: frame_cost_enter() hands it back when it could not
 * push, and frame_cost_leave() treats it as already gone. */
#define FRAME_COST_IGNORE_MARK (-1)

typedef struct {
    const char* name;
    int64_t total_us;
    int64_t worst_us;
} frame_cost_slot_t;

typedef struct {
    int64_t began_us;
    int64_t inside_us;
} frame_cost_level_t;

typedef struct {
    frame_cost_slot_t slots[FRAME_COST_SLOTS];
    int count;
    int dropped;
    frame_cost_level_t stack[FRAME_COST_STACK_DEPTH];
    int depth;
} frame_cost_t;

/* Names are string literals: one is found again by its address first. With
 * every slot taken a new name is dropped rather than charged to another. */
static inline void
frame_cost_add(frame_cost_t* cost, const char* name, int64_t us) {
    frame_cost_slot_t* slot = NULL;
    for (int i = 0; i < cost->count && slot == NULL; i++) {
        if (cost->slots[i].name == name || strcmp(cost->slots[i].name, name) == 0) {
            slot = &cost->slots[i];
        }
    }
    if (slot == NULL) {
        if (cost->count >= FRAME_COST_SLOTS) {
            cost->dropped++;
            return;
        }
        slot = &cost->slots[cost->count++];
        *slot = (frame_cost_slot_t){.name = name};
    }
    slot->total_us += us;
    slot->worst_us = us > slot->worst_us ? us : slot->worst_us;
}

/* Pushes the level a bracket just started at and returns its mark, for
 * frame_cost_leave() to read back. A stack already FRAME_COST_STACK_DEPTH
 * deep pushes nothing and returns FRAME_COST_IGNORE_MARK instead. */
static inline int
frame_cost_enter(frame_cost_t* cost, int64_t now_us) {
    if (cost->depth >= FRAME_COST_STACK_DEPTH) {
        return FRAME_COST_IGNORE_MARK;
    }
    const int mark = cost->depth++;
    cost->stack[mark] = (frame_cost_level_t){.began_us = now_us};
    return mark;
}

/* Charges `name` its elapsed time minus whatever ran inside it, then folds
 * the whole elapsed time into the level below, so that one is exclusive of
 * it in turn. Dropping the stack straight to `mark` both pops this level and
 * discards, uncharged, any deeper one whose own END never ran. A `mark` no
 * longer on the stack - already popped, or FRAME_COST_IGNORE_MARK - charges
 * nothing. */
static inline void
frame_cost_leave(frame_cost_t* cost, int mark, const char* name, int64_t now_us) {
    if (mark < 0 || mark >= cost->depth) {
        return;
    }
    /* What an abandoned level's own children were charged is still time
     * this one did not spend. */
    for (int deeper = mark + 1; deeper < cost->depth; deeper++) {
        cost->stack[mark].inside_us += cost->stack[deeper].inside_us;
    }
    const frame_cost_level_t level = cost->stack[mark];
    cost->depth = mark;
    const int64_t elapsed = now_us - level.began_us;
    frame_cost_add(cost, name, elapsed - level.inside_us);
    if (mark > 0) {
        cost->stack[mark - 1].inside_us += elapsed;
    }
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

/* " +N dropped", once there is a total to trail - a name refused for want of
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

/* "name avg/worst" per slot, then a total and any drop count, then forgets
 * the window and empties the open-bracket stack, so a report taken mid-
 * bracket leaves no stale level for that bracket's own END to find.
 *
 * `frames` zero leaves the window for the next report to find; `out_size`
 * zero writes nothing but still clears the window - the caller did ask for
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
void frame_cost_end(int mark, const char* name);
int frame_cost_take_report(uint32_t frames, char* out, size_t out_size);

#define FRAME_COST_BEGIN(mark)     const int mark = frame_cost_begin()
#define FRAME_COST_END(mark, name) frame_cost_end((mark), (name))
#else
#define FRAME_COST_BEGIN(mark)     ((void)0)
#define FRAME_COST_END(mark, name) ((void)0)
#endif
