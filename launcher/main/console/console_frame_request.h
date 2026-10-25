/*
 * console_frame_request: what console verbs ask of the frame loop, handed
 * from the console's reader task to the shell through registered slots.
 *
 * A verb parses its line and posts into its own slot; the frame loop runs
 * every pending slot's answer once per pass, before the freeze gate, so a
 * held frame still answers FRAMEWATCH and SOUND. A slot without an answer
 * belongs to the frame loop, which takes it where it acts. The reader task
 * may not act itself: see console.c's top comment.
 *
 * A slot keeps only its latest post, the line the device saw most recently,
 * unless it is held: then a second post is refused while the first is
 * pending or being answered, until console_frame_done().
 *
 * Every call holds the set's lock for its whole copy, so a take never sees
 * a posted flag without its payload, nor a payload a post is still writing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"

typedef struct console_frame_slot {
    const char* name;
    struct console_frame_slot* next;
    void* pending;                       /* `size` bytes a post fills */
    void* taken;                         /* `size` bytes a take hands the frame loop */
    size_t size;                         /* 0: the post itself is the request */
    void (*answer)(const void* payload); /* NULL: the frame loop takes it itself */
    bool held;
    bool posted;
    bool busy; /* held, taken, not yet done */
} console_frame_slot_t;

typedef struct {
    portMUX_TYPE lock;
    console_frame_slot_t* first;
} console_frame_slots_t;

#define CONSOLE_FRAME_SLOTS_INIT {.lock = portMUX_INITIALIZER_UNLOCKED}

/* The set every CONSOLE_FRAME_SLOT() joins and the shell answers from. */
console_frame_slots_t* console_frame_slots(void);

/* False when another slot already has its name. */
bool console_frame_add(console_frame_slots_t* slots, console_frame_slot_t* slot);

/* Any task: `payload` (`slot->size` bytes) becomes the slot's request. False,
 * and nothing copied, when a held slot is pending or busy. */
bool console_frame_post(console_frame_slots_t* slots, console_frame_slot_t* slot, const void* payload);

/* The frame loop: true once per post, its payload in `slot->taken`. A held
 * slot stays busy until console_frame_done(). */
bool console_frame_take(console_frame_slots_t* slots, console_frame_slot_t* slot);

/* The frame loop: a held slot it took has finished. */
void console_frame_done(console_frame_slots_t* slots, console_frame_slot_t* slot);

/* The frame loop, once per pass: takes and answers every slot with an
 * answer, then marks a held one done. */
void console_frame_answer_pending(console_frame_slots_t* slots);

/* A slot of `type` (or none: CONSOLE_FRAME_SIGNAL) in the shared set,
 * registered before app_main() like CONSOLE_VERB(). */
#define CONSOLE_FRAME_SLOT(slot, type, is_held, answer_fn)                                                             \
    static type slot##_pending;                                                                                        \
    static type slot##_taken;                                                                                          \
    console_frame_slot_t slot = {.name = #slot,                                                                        \
                                 .pending = &slot##_pending,                                                           \
                                 .taken = &slot##_taken,                                                               \
                                 .size = sizeof(type),                                                                 \
                                 .answer = (answer_fn),                                                                \
                                 .held = (is_held)};                                                                   \
    CONSOLE_FRAME_REGISTER(slot)

#define CONSOLE_FRAME_SIGNAL(slot, answer_fn)                                                                          \
    console_frame_slot_t slot = {.name = #slot, .answer = (answer_fn)};                                                \
    CONSOLE_FRAME_REGISTER(slot)

#define CONSOLE_FRAME_REGISTER(slot)                                                                                   \
    __attribute__((constructor)) static void slot##_register(void) {                                                   \
        (void)console_frame_add(console_frame_slots(), &slot);                                                         \
    }
