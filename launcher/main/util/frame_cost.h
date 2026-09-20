/*
 * frame_cost - where a frame's time goes, by name: a stretch of code is
 * bracketed, its microseconds are added to a named slot, and once a window
 * the slots are read out as milliseconds per frame and forgotten.
 *
 * The arithmetic is here and pure, on a frame_cost_t a test can own. The one
 * a firmware shares, and its clock, exist in a development build on the chip
 * and nowhere else: on a host and in release the brackets are nothing, so
 * code that carries them needs no clock and links against nothing.
 *
 * A bracket costs two clock reads, about a microsecond each on the chip. Put
 * one around a stage of a frame, never around a pixel.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAME_COST_SLOTS 12

typedef struct {
    const char* name;
    int64_t total_us;
    int64_t worst_us;
} frame_cost_slot_t;

typedef struct {
    frame_cost_slot_t slots[FRAME_COST_SLOTS];
    int count;
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
            return;
        }
        slot = &cost->slots[cost->count++];
        *slot = (frame_cost_slot_t){.name = name};
    }
    slot->total_us += us;
    slot->worst_us = us > slot->worst_us ? us : slot->worst_us;
}

/* "name avg/worst" in milliseconds for every slot, the average over `frames`
 * and the worst a single bracket, then forgets them so that the next window
 * starts clean. Returns the length written. */
static inline int
frame_cost_report(frame_cost_t* cost, uint32_t frames, char* out, size_t out_size) {
    int length = 0;
    if (out_size > 0) {
        out[0] = '\0';
    }
    for (int i = 0; i < cost->count && frames > 0; i++) {
        const frame_cost_slot_t* slot = &cost->slots[i];
        const int64_t average_us = slot->total_us / frames;
        const int wrote = snprintf(out + length, out_size - (size_t)length, "%s%s %d.%02d/%d.%d", i > 0 ? "  " : "",
                                   slot->name, (int)(average_us / 1000), (int)(average_us % 1000 / 10),
                                   (int)(slot->worst_us / 1000), (int)(slot->worst_us % 1000 / 100));
        if (wrote < 0 || (size_t)(length + wrote) >= out_size) {
            out[length] = '\0';
            break;
        }
        length += wrote;
    }
    cost->count = 0;
    return length;
}

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
#define FRAME_COST_ENABLED 1
#else
#define FRAME_COST_ENABLED 0
#endif

#if FRAME_COST_ENABLED
int64_t frame_cost_now_us(void);
void frame_cost_charge(const char* name, int64_t since_us);
int frame_cost_take_report(uint32_t frames, char* out, size_t out_size);

#define FRAME_COST_BEGIN(mark)     const int64_t mark = frame_cost_now_us()
#define FRAME_COST_END(mark, name) frame_cost_charge((name), (mark))
#else
#define FRAME_COST_BEGIN(mark)     ((void)0)
#define FRAME_COST_END(mark, name) ((void)0)
#endif
